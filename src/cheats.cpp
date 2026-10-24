// Game hooks for Enshrouded 1.0.
// Each hook: find the pattern, check the original bytes, then either patch in place or build a
// code cave (jmp in, jmp abs back). Constants and slider values live in our own page, never in
// game structs. Every cave was cross-checked against ml64 output.

#include "cheats.h"

#include "log.h"
#include "patch.h"

#include <windows.h>

#include <cstring>
#include <deque>
#include <initializer_list>
#include <vector>

namespace cheats {
namespace {

using Bytes = std::initializer_list<uint8_t>;

struct Hook {
    const char* name = "";
    uintptr_t site = 0;
    size_t len = 0;
    uint8_t original[16] = {};
    uint8_t patched[16] = {};
    bool ready = false;
    bool on = false;
};

// Written by the caves: how often they ran and the last attribute slot they touched.
struct Probe {
    volatile uint32_t hits;
    uint32_t pad;
    volatile uint64_t ptr;
};

struct Cheat {
    Info info;
    bool (*prepare)(Cheat&) = nullptr;
    bool (*customApply)(Cheat&, bool on) = nullptr; // default: all hooks follow `on`
    void (*onValue)(Cheat&) = nullptr;              // extra work when the value changes
    std::vector<Hook*> hooks;
    float* value = nullptr; // read by the cave (sliders)
    bool ready = false;
    bool on = false;
    float cur = 0;
};

constexpr size_t kPageSize = 0x4000;
constexpr uint32_t kTenF = 0x41200000; // 10.0f, marks the player's structs

bool g_initDone = false;
uint8_t* g_page = nullptr;
size_t g_used = 0;
Probe* g_probes = nullptr; // health, stamina, mana
std::deque<Hook> g_hooks;  // stable addresses
std::vector<Cheat> g_cheats;

// --- memory in our page ---------------------------------------------------------------------

void* take(size_t n, size_t align = 16)
{
    size_t p = (g_used + align - 1) & ~(align - 1);
    if (!g_page || p + n > kPageSize)
        return nullptr;
    g_used = p + n;
    return g_page + p;
}

float* dataF(float v)
{
    auto* p = static_cast<float*>(take(4, 4));
    *p = v;
    return p;
}

uint8_t* dataB()
{
    auto* p = static_cast<uint8_t*>(take(1, 1));
    *p = 0;
    return p;
}

uintptr_t* dataPtr()
{
    auto* p = static_cast<uintptr_t*>(take(8, 8));
    *p = 0;
    return p;
}

uint32_t bitsOf(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

bool safeRead(const void* src, void* dst, size_t n)
{
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool safeWrite(void* dst, const void* src, size_t n)
{
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- hooks ----------------------------------------------------------------------------------

Hook& newHook()
{
    return g_hooks.emplace_back();
}

uintptr_t find(const char* name, const char* pattern, int offset = 0)
{
    uintptr_t m = patch::scan(name, pattern);
    return m ? m + offset : 0;
}

// Records the site and checks the bytes we replace are what we expect.
bool locate(Hook& h, const char* name, uintptr_t site, Bytes expected)
{
    h.name = name;
    if (!site)
        return false;
    if (memcmp(reinterpret_cast<const void*>(site), expected.begin(), expected.size()) != 0) {
        LOG("%s: unexpected bytes at site, game code changed, cheat disabled", name);
        return false;
    }
    h.site = site;
    h.len = expected.size();
    memcpy(h.original, expected.begin(), h.len);
    return true;
}

bool inPlace(Hook& h, const char* name, uintptr_t site, Bytes expected, Bytes patched)
{
    if (!locate(h, name, site, expected))
        return false;
    memcpy(h.patched, patched.begin(), patched.size());
    h.ready = true;
    return true;
}

// `build(e, back)` emits the cave body and must end with e.jmpAbs(back) or a ret.
template <typename Build> bool caveHook(Hook& h, const char* name, uintptr_t site, Bytes expected, Build build)
{
    if (!locate(h, name, site, expected))
        return false;
    auto* cave = static_cast<uint8_t*>(take(0x80));
    if (!cave)
        return false;
    patch::Emitter e(cave);
    build(e, h.site + h.len);
    e.commit();

    auto rel = int32_t(reinterpret_cast<intptr_t>(cave) - intptr_t(h.site + 5));
    h.patched[0] = 0xE9;
    memcpy(&h.patched[1], &rel, 4);
    for (size_t i = 5; i < h.len; ++i)
        h.patched[i] = 0x90;
    h.ready = true;
    return true;
}

bool apply(Hook& h, bool on)
{
    if (!h.ready || h.on == on)
        return h.on;
    const uint8_t* expect = on ? h.original : h.patched;
    if (memcmp(reinterpret_cast<const void*>(h.site), expect, h.len) != 0) {
        LOG("%s: site bytes changed by someone else, not touching it", h.name);
        return h.on;
    }
    if (patch::writeCode(h.site, on ? h.patched : h.original, h.len)) {
        h.on = on;
        LOG("%s: %s", h.name, on ? "ON" : "OFF");
    }
    return h.on;
}

bool applyAll(Cheat& c, bool on)
{
    bool all = true;
    for (Hook* h : c.hooks)
        all = apply(*h, on) == on && all;
    if (!all && on) { // keep multi-hook cheats consistent
        for (Hook* h : c.hooks)
            apply(*h, false);
        return false;
    }
    return on;
}

// lock inc [hits] ; push rax ; lea rax,[rcx+rdx*4] ; mov [ptr],rax ; pop rax  (clobbers flags)
void emitProbe(patch::Emitter& e, Probe* p)
{
    e.ripRel({0xF0, 0xFF, 0x05}, const_cast<uint32_t*>(&p->hits))
        .bytes({0x50, 0x48, 0x8D, 0x04, 0x91})
        .ripRel({0x48, 0x89, 0x05}, const_cast<uint64_t*>(&p->ptr))
        .bytes({0x58});
}

// cmp dword [reg+disp32],10.0f  for the player check. `modrm` selects the base register.
void emitIsPlayer(patch::Emitter& e, uint8_t modrm, int32_t disp)
{
    e.bytes({0x81, modrm}).imm32(uint32_t(disp)).imm32(kTenF);
}

// --- PLAYER -----------------------------------------------------------------------------------

// Health change: add [rcx+rdx*4],edi ; lea rcx,[rsp+30h]. edi < 0 is damage. The player's
// attribute block has 800 at +0x14. God Mode and One-Hit Kill share this cave
// and each have a flag byte; the hook is on while either flag is set.
Hook* g_health = nullptr;
uint8_t* g_godFlag = nullptr;
uint8_t* g_killFlag = nullptr;

bool prepareHealth()
{
    if (g_health)
        return g_health->ready;
    g_health = &newHook();
    g_godFlag = dataB();
    g_killFlag = dataB();
    uintptr_t site =
        find("health", "B0 01 48 83 C4 38 C3 CC CC CC CC CC CC CC CC CC CC CC CC 48 89 5C 24 10", 0x7C);
    return caveHook(*g_health, "Health", site, {0x01, 0x3C, 0x91, 0x48, 0x8D, 0x4C, 0x24, 0x30},
                    [](patch::Emitter& e, uintptr_t back) {
                        emitProbe(e, &g_probes[0]);
                        e.bytes({0x81, 0x7C, 0x91, 0x14}).imm32(800); // cmp dword [rcx+rdx*4+14h],800
                        size_t toOther = e.jcc(0x75);                  // jne other
                        e.ripRel({0x80, 0x3D}, g_godFlag, 1).bytes({0x00}); // cmp byte [god],0
                        size_t toAdd = e.jcc(0x74);                    // je add
                        e.bytes({0x85, 0xFF});                         // test edi,edi
                        size_t toSkip = e.jcc(0x78);                   // js skip
                        e.bind(toAdd);
                        e.bytes({0x01, 0x3C, 0x91});                   // add [rcx+rdx*4],edi
                        e.bind(toSkip);
                        e.bytes({0x48, 0x8D, 0x4C, 0x24, 0x30});       // lea rcx,[rsp+30h]
                        e.jmpAbs(back);
                        e.bind(toOther);                               // other entity:
                        e.bytes({0x01, 0x3C, 0x91});                   // add [rcx+rdx*4],edi
                        e.ripRel({0x80, 0x3D}, g_killFlag, 1).bytes({0x00}); // cmp byte [kill],0
                        size_t toDone = e.jcc(0x74);                   // je done
                        e.bytes({0x85, 0xFF});                         // test edi,edi
                        size_t toDone2 = e.jcc(0x79);                  // jns done (heals stay)
                        e.bytes({0xC7, 0x04, 0x91}).imm32(0);          // mov dword [rcx+rdx*4],0
                        e.bind(toDone);
                        e.bind(toDone2);
                        e.bytes({0x48, 0x8D, 0x4C, 0x24, 0x30});       // lea rcx,[rsp+30h]
                        e.jmpAbs(back);
                    });
}

bool applyHealthFlag(uint8_t* flag, bool on)
{
    *flag = on ? 1 : 0;
    bool want = *g_godFlag || *g_killFlag;
    bool now = apply(*g_health, want);
    if (now != want) { // could not patch: undo the flag
        *flag = 0;
        return false;
    }
    return on;
}

bool prepareStamina(Cheat& c)
{
    // Stamina read: mov eax,[rcx+rdx*4] ; mov [rsp+4Ch],eax. Max stamina sits at +8.
    Hook& h = newHook();
    c.hooks = {&h};
    return caveHook(h, "Infinite Stamina", find("stamina", "8B 04 91 89 44 24 4C"),
                    {0x8B, 0x04, 0x91, 0x89, 0x44, 0x24, 0x4C}, [](patch::Emitter& e, uintptr_t back) {
                        emitProbe(e, &g_probes[1]);
                        e.bytes({0x8B, 0x44, 0x91, 0x08})  // mov eax,[rcx+rdx*4+8]
                            .bytes({0x89, 0x04, 0x91})     // mov [rcx+rdx*4],eax
                            .bytes({0x89, 0x44, 0x24, 0x4C}) // mov [rsp+4Ch],eax
                            .jmpAbs(back);
                    });
}

bool prepareMana(Cheat& c)
{
    // Mana read: add rcx,rbx ; mov ebx,[rcx+rdx*4]. The function writes ebx (+regen -cost) back
    // later, so both ebx and the slot become max mana, which sits at slot+4 (the game clamps to it).
    Hook& h = newHook();
    c.hooks = {&h};
    return caveHook(h, "Infinite Mana",
                    find("mana", "0F B7 4B 04 48 03 CB 8B 1C 91 * * 33 C0 8B D8 48 8B 4D C8", 4),
                    {0x48, 0x03, 0xCB, 0x8B, 0x1C, 0x91}, [](patch::Emitter& e, uintptr_t back) {
                        e.bytes({0x48, 0x03, 0xCB}); // add rcx,rbx
                        emitProbe(e, &g_probes[2]);
                        e.bytes({0x8B, 0x5C, 0x91, 0x04}) // mov ebx,[rcx+rdx*4+4]  (max mana)
                            .bytes({0x89, 0x1C, 0x91})    // mov [rcx+rdx*4],ebx    (current = max)
                            .jmpAbs(back);
                    });
}

bool prepareFallDamage(Cheat& c)
{
    // mov [r8+rcx*4],r9d -> add [r8+rcx*4],r9d (in place).
    Hook& h = newHook();
    c.hooks = {&h};
    return inPlace(h, "No Fall Damage", find("fall_damage", "45 89 0C 88 48 8B CB"), {0x45, 0x89, 0x0C, 0x88},
                   {0x45, 0x01, 0x0C, 0x88});
}

bool prepareOxygen(Cheat& c)
{
    // sub [r8+rcx*4],eax -> add (counts up instead of down).
    Hook& h = newHook();
    c.hooks = {&h};
    return inPlace(h, "Infinite Oxygen", find("oxygen", "4C 03 C3 41 29 04 88 48 8B CB E8 * * * * E9", 3),
                   {0x41, 0x29, 0x04, 0x88}, {0x41, 0x01, 0x04, 0x88});
}

bool prepareFrost(Cheat& c)
{
    // Same sub -> add on the frost timer. The pattern has two matches; the first is the right one.
    Hook& h = newHook();
    c.hooks = {&h};
    return inPlace(h, "Infinite Frost", find("frost", "48 8B 45 08 0F B7 40 0C 0F B7 C8 48 85 DB", 0x25),
                   {0x41, 0x29, 0x04, 0x88}, {0x41, 0x01, 0x04, 0x88});
}

bool prepareShroud(Cheat& c)
{
    // Shroud timer: add [r8+rcx*4],eax ; mov rcx,rbx -> pin it at 300.
    Hook& h = newHook();
    c.hooks = {&h};
    return caveHook(h, "Infinite Shroud Timer", find("fog_timer", "41 01 04 88 48 8B CB * * * * * 48 8B 4C"),
                    {0x41, 0x01, 0x04, 0x88, 0x48, 0x8B, 0xCB}, [](patch::Emitter& e, uintptr_t back) {
                        e.bytes({0x41, 0xC7, 0x04, 0x88}).imm32(300) // mov dword [r8+rcx*4],300
                            .bytes({0x48, 0x8B, 0xCB})               // mov rcx,rbx
                            .jmpAbs(back);
                    });
}

// --- MOVE -------------------------------------------------------------------------------------

bool prepareRunSpeed(Cheat& c)
{
    // Player movement: x/z lanes scaled, y left alone. Player = 10.0f at [rax-2B74h].
    Hook& a = newHook();
    Hook& b = newHook();
    c.hooks = {&a, &b};
    c.value = dataF(1.0f);
    static float* factor;
    factor = c.value;
    bool okA = caveHook(a, "Run Speed A", find("speed_a", "F2 0F 10 08 F2 41 0F 59 C5"),
                        {0xF2, 0x0F, 0x10, 0x08, 0xF2, 0x41, 0x0F, 0x59, 0xC5},
                        [](patch::Emitter& e, uintptr_t back) {
                            emitIsPlayer(e, 0xB8, -0x2B74);           // cmp dword [rax-2B74h],10.0f
                            e.bytes({0xF2, 0x0F, 0x10, 0x08});        // movsd xmm1,[rax]
                            e.bytes({0xF2, 0x41, 0x0F, 0x59, 0xC5});  // mulsd xmm0,xmm13
                            size_t done = e.jcc(0x75);                // jne done
                            e.ripRel({0xF3, 0x0F, 0x59, 0x0D}, factor); // mulss xmm1,[factor]
                            e.bind(done);
                            e.jmpAbs(back);
                        });
    bool okB = caveHook(b, "Run Speed B", find("speed_b", "F3 0F 59 78 08 F3 44 0F 59"),
                        {0xF3, 0x0F, 0x59, 0x78, 0x08}, [](patch::Emitter& e, uintptr_t back) {
                            e.bytes({0xF3, 0x0F, 0x59, 0x78, 0x08});  // mulss xmm7,[rax+8]
                            emitIsPlayer(e, 0xB8, -0x2B74);
                            size_t done = e.jcc(0x75);
                            e.ripRel({0xF3, 0x0F, 0x59, 0x3D}, factor); // mulss xmm7,[factor]
                            e.bind(done);
                            e.jmpAbs(back);
                        });
    return okA && okB;
}

bool prepareHighJump(Cheat& c)
{
    // Vertical velocity store: movss [r15+4],xmm1. Scale it first.
    Hook& h = newHook();
    c.hooks = {&h};
    c.value = dataF(1.0f);
    static float* mul;
    mul = c.value;
    return caveHook(h, "High Jump", find("high_jump", "F3 41 0F 11 4F 04 0F 57"),
                    {0xF3, 0x41, 0x0F, 0x11, 0x4F, 0x04}, [](patch::Emitter& e, uintptr_t back) {
                        e.ripRel({0xF3, 0x0F, 0x59, 0x0D}, mul);         // mulss xmm1,[mul]
                        e.bytes({0xF3, 0x41, 0x0F, 0x11, 0x4F, 0x04});   // movss [r15+4],xmm1
                        e.jmpAbs(back);
                    });
}

bool prepareGravity(Cheat& c)
{
    // The gravity getter ends with this ret; return our value instead (vanilla 25).
    Hook& h = newHook();
    c.hooks = {&h};
    c.value = dataF(25.0f);
    static float* g;
    g = c.value;
    return caveHook(h, "Gravity", find("gravity", "C3 CC CC CC CC CC CC CC C7 01 00 00 00 80"),
                    {0xC3, 0xCC, 0xCC, 0xCC, 0xCC}, [](patch::Emitter& e, uintptr_t) {
                        e.ripRel({0xF3, 0x0F, 0x10, 0x05}, g); // movss xmm0,[g]
                        e.bytes({0xC3});                       // ret
                    });
}

bool prepareFallSpeed(Cheat& c)
{
    // Max fall speed getter epilogue: add rsp,28h ; ret. Scale the returned value.
    Hook& h = newHook();
    c.hooks = {&h};
    c.value = dataF(1.0f);
    static float* f;
    f = c.value;
    return caveHook(h, "Fall Speed", find("fall_speed", "48 83 C4 28 C3 CC CC CC CC CC CC CC CC CC F3"),
                    {0x48, 0x83, 0xC4, 0x28, 0xC3}, [](patch::Emitter& e, uintptr_t) {
                        e.ripRel({0xF3, 0x0F, 0x59, 0x05}, f); // mulss xmm0,[f]
                        e.bytes({0x48, 0x83, 0xC4, 0x28});     // add rsp,28h
                        e.bytes({0xC3});                       // ret
                    });
}

bool prepareInfiniteJumps(Cheat& c)
{
    // seta al ; mov [rdi+49h],al stores the "jump used" flag; clear it for the player (E to jump).
    Hook& h = newHook();
    c.hooks = {&h};
    return caveHook(h, "Infinite Jumps", find("force_jump", "0F 97 C0 88 47 49 84"),
                    {0x0F, 0x97, 0xC0, 0x88, 0x47, 0x49}, [](patch::Emitter& e, uintptr_t back) {
                        e.bytes({0x0F, 0x97, 0xC0, 0x88, 0x47, 0x49}); // seta al ; mov [rdi+49h],al
                        emitIsPlayer(e, 0xBF, -0x532C);                // cmp dword [rdi-532Ch],10.0f
                        size_t done = e.jcc(0x75);
                        e.bytes({0xC6, 0x47, 0x49, 0x00});             // mov byte [rdi+49h],0
                        e.bind(done);
                        e.jmpAbs(back);
                    });
}

bool prepareFly(Cheat& c)
{
    // Vertical velocity store movss [rax+4],xmm2; the player gets a small constant upward value.
    Hook& h = newHook();
    c.hooks = {&h};
    return caveHook(h, "Fly (Build Mode)", find("flying", "F3 0F 11 50 04 F3 0F 11 40 08 48 8B 4D AF"),
                    {0xF3, 0x0F, 0x11, 0x50, 0x04}, [](patch::Emitter& e, uintptr_t back) {
                        e.bytes({0xF3, 0x0F, 0x11, 0x50, 0x04});  // movss [rax+4],xmm2
                        emitIsPlayer(e, 0xB8, -0x2B74);
                        size_t done = e.jcc(0x75);
                        e.bytes({0xC7, 0x40, 0x04}).f32(0.03f);   // mov dword [rax+4],0.03f
                        e.bind(done);
                        e.jmpAbs(back);
                    });
}

bool prepareSwimInAir(Cheat& c)
{
    // Movement state byte [rdx+59h]; 0x0A = swimming. Player = 10.0f at [rdx+63CCh].
    Hook& h = newHook();
    c.hooks = {&h};
    return caveHook(h, "Swim in Air", find("swimming", "88 5A 59 48 8B 55 90"),
                    {0x88, 0x5A, 0x59, 0x48, 0x8B, 0x55, 0x90}, [](patch::Emitter& e, uintptr_t back) {
                        e.bytes({0x88, 0x5A, 0x59});            // mov [rdx+59h],bl
                        emitIsPlayer(e, 0xBA, 0x63CC);          // cmp dword [rdx+63CCh],10.0f
                        size_t skip = e.jcc(0x75);
                        e.bytes({0xC6, 0x42, 0x59, 0x0A});      // mov byte [rdx+59h],0Ah
                        e.bind(skip);
                        e.bytes({0x48, 0x8B, 0x55, 0x90});      // mov rdx,[rbp-70h]
                        e.jmpAbs(back);
                    });
}

// Climb thresholds (vanilla 45/55/65 degrees). The cave remembers the struct so OFF can restore.
uintptr_t* g_slopePtr = nullptr;

bool prepareClimb(Cheat& c)
{
    Hook& h = newHook();
    c.hooks = {&h};
    g_slopePtr = dataPtr();
    c.customApply = [](Cheat& self, bool on) {
        bool now = applyAll(self, on);
        if (!on && *g_slopePtr) {
            const float vanilla[3] = {45.0f, 55.0f, 65.0f};
            if (safeWrite(reinterpret_cast<void*>(*g_slopePtr), vanilla, sizeof(vanilla)))
                LOG("Climb Any Slope: restored 45/55/65");
        }
        return now;
    };
    return caveHook(h, "Climb Any Slope", find("slope_angle", "28 F0 F3 0F 10 46 08", 2),
                    {0xF3, 0x0F, 0x10, 0x46, 0x08}, [](patch::Emitter& e, uintptr_t back) {
                        e.bytes({0x81, 0x7E, 0x0C}).imm32(kTenF);    // cmp dword [rsi+0Ch],10.0f
                        size_t load = e.jcc(0x75);                   // jne load
                        e.ripRel({0x48, 0x89, 0x35}, g_slopePtr);    // mov [slopePtr],rsi
                        e.bytes({0xC7, 0x06}).f32(89.9996f);         // mov dword [rsi],89.9996
                        e.bytes({0xC7, 0x46, 0x04}).f32(89.9997f);   // mov dword [rsi+4],89.9997
                        e.bytes({0xC7, 0x46, 0x08}).f32(89.9998f);   // mov dword [rsi+8],89.9998
                        e.bind(load);
                        e.bytes({0xF3, 0x0F, 0x10, 0x46, 0x08});     // movss xmm0,[rsi+8]
                        e.jmpAbs(back);
                    });
}

bool prepareGlider(Cheat& c)
{
    // Glider pitch/turn limits, one function: look-up limit -1.25 rad, look-down 0, turn rate 0.036.
    // Flags set before glide_up are used by the ja after it: only movss there.
    Hook& up = newHook();
    Hook& down = newHook();
    Hook& turn = newHook();
    c.hooks = {&up, &down, &turn};
    static float *upV, *downV, *turnV;
    upV = dataF(-1.25f);
    downV = dataF(0.0f);
    turnV = dataF(0.036f);
    bool ok = caveHook(up, "Glider Up", find("upward_pitch_angle", "F2 0F 11 4C 24 40 77"),
                       {0xF2, 0x0F, 0x11, 0x4C, 0x24, 0x40}, [](patch::Emitter& e, uintptr_t back) {
                           e.ripRel({0xF3, 0x0F, 0x10, 0x05}, upV);        // movss xmm0,[up]
                           e.bytes({0xF2, 0x0F, 0x11, 0x4C, 0x24, 0x40});  // movsd [rsp+40h],xmm1
                           e.jmpAbs(back);
                       });
    ok = caveHook(down, "Glider Down", find("downward_pitch_angle", "F3 41 0F 5D F0 EB 03"),
                  {0xF3, 0x41, 0x0F, 0x5D, 0xF0}, [](patch::Emitter& e, uintptr_t back) {
                      e.ripRel({0xF3, 0x0F, 0x10, 0x35}, downV);   // movss xmm6,[down]
                      e.bytes({0xF3, 0x41, 0x0F, 0x5D, 0xF0});     // minss xmm6,xmm8
                      e.jmpAbs(back);
                  }) && ok;
    ok = caveHook(turn, "Glider Turn", find("turn_angle", "F3 0F 59 D4 0F 28 CF 0F"),
                  {0xF3, 0x0F, 0x59, 0xD4, 0x0F, 0x28, 0xCF}, [](patch::Emitter& e, uintptr_t back) {
                      e.ripRel({0xF3, 0x0F, 0x10, 0x25}, turnV);   // movss xmm4,[turn]
                      e.bytes({0xF3, 0x0F, 0x59, 0xD4});           // mulss xmm2,xmm4
                      e.bytes({0x0F, 0x28, 0xCF});                 // movaps xmm1,xmm7
                      e.jmpAbs(back);
                  }) && ok;
    return ok;
}

// --- WORLD ------------------------------------------------------------------------------------

bool prepareAlwaysDay(Cheat& c)
{
    // World clock store: mov [rdi+48h],rax ; cmp rax,[r15]. 32.32 fixed point, hour*1000 in the
    // high dword at +4Ch; pin it to 12000 (midday). The cmp's flags are used after we return.
    Hook& h = newHook();
    c.hooks = {&h};
    return caveHook(h, "Always Day", find("day_time", "48 89 47 48 49 3B 07"),
                    {0x48, 0x89, 0x47, 0x48, 0x49, 0x3B, 0x07}, [](patch::Emitter& e, uintptr_t back) {
                        e.bytes({0x48, 0x89, 0x47, 0x48})            // mov [rdi+48h],rax
                            .bytes({0xC7, 0x47, 0x4C}).imm32(12000)  // mov dword [rdi+4Ch],12000
                            .bytes({0x49, 0x3B, 0x07})               // cmp rax,[r15]
                            .jmpAbs(back);
                    });
}

// movss [rbp+disp32],xmmN replaced by a constant store (water render parameters).
template <uint8_t B4, uint8_t B5> void emitRbpZero(patch::Emitter& e, uintptr_t back)
{
    e.bytes({0xC7, 0x85, B4, B5, 0x00, 0x00}).imm32(0); // mov dword [rbp+disp],0
    e.jmpAbs(back);
}

bool prepareTransparentWater(Cheat& c)
{
    Hook& h = newHook();
    c.hooks = {&h};
    return caveHook(h, "Transparent Water", find("transparent_water", "F3 0F 11 8D C0 5A 00 00"),
                    {0xF3, 0x0F, 0x11, 0x8D, 0xC0, 0x5A, 0x00, 0x00}, emitRbpZero<0xC0, 0x5A>);
}

bool prepareInvisibleWater(Cheat& c)
{
    Hook& h = newHook();
    c.hooks = {&h};
    return caveHook(h, "Invisible Water", find("invisible_water", "F3 0F 11 8D 94 39 00 00"),
                    {0xF3, 0x0F, 0x11, 0x8D, 0x94, 0x39, 0x00, 0x00}, emitRbpZero<0x94, 0x39>);
}

bool prepareWaterGlow(Cheat& c)
{
    // movss [rbp+6350h],xmm0 -> store our value instead.
    Hook& h = newHook();
    c.hooks = {&h};
    c.value = dataF(c.info.def);
    static float* glow;
    glow = c.value;
    return caveHook(h, "Water Glow", find("water_glow", "F3 0F 11 85 50 63 00 00"),
                    {0xF3, 0x0F, 0x11, 0x85, 0x50, 0x63, 0x00, 0x00}, [](patch::Emitter& e, uintptr_t back) {
                        e.bytes({0x50});                                   // push rax
                        e.ripRel({0x8B, 0x05}, glow);                      // mov eax,[glow]
                        e.bytes({0x89, 0x85, 0x50, 0x63, 0x00, 0x00});     // mov [rbp+6350h],eax
                        e.bytes({0x58});                                   // pop rax
                        e.jmpAbs(back);
                    });
}

// --- BUILD ------------------------------------------------------------------------------------

// or dword [r15],imm8 raises a "can't build here" flag; imm 0 makes it a no-op.
bool prepareBuildFlag(Cheat& c, const char* name, const char* aobName, const char* pattern, uint8_t flag)
{
    Hook& h = newHook();
    c.hooks = {&h};
    return inPlace(h, name, find(aobName, pattern), {0x41, 0x83, 0x0F, flag}, {0x41, 0x83, 0x0F, 0x00});
}

bool prepareAltarFar(Cheat& c)
{
    // mov [rsi+69h],dl ; cmp qword [rbp+200h],0. Force the "in altar range" byte to 4.
    // The cmp stays last: a je follows the return.
    Hook& h = newHook();
    c.hooks = {&h};
    return caveHook(h, "Altar From Any Distance", find("altar_far", "88 56 69 48 83 BD 00 02 00 00 00"),
                    {0x88, 0x56, 0x69, 0x48, 0x83, 0xBD, 0x00, 0x02, 0x00, 0x00, 0x00},
                    [](patch::Emitter& e, uintptr_t back) {
                        e.bytes({0x88, 0x56, 0x69})                                // mov [rsi+69h],dl
                            .bytes({0xC6, 0x46, 0x69, 0x04})                       // mov byte [rsi+69h],4
                            .bytes({0x48, 0x83, 0xBD, 0x00, 0x02, 0x00, 0x00, 0x00}) // cmp qword [rbp+200h],0
                            .jmpAbs(back);
                    });
}

Hook* g_buildRange = nullptr;

bool prepareBuildRange(Cheat& c)
{
    // mov r12d,3 caps how far build parts reach; patch the immediate in place.
    Hook& h = newHook();
    g_buildRange = &h;
    c.hooks = {&h};
    c.onValue = [](Cheat& self) {
        uint32_t range = uint32_t(self.cur + 0.5f);
        memcpy(&g_buildRange->patched[2], &range, 4);
        if (g_buildRange->on) // already patched: rewrite the immediate in place
            patch::writeCode(g_buildRange->site, g_buildRange->patched, g_buildRange->len);
    };
    return inPlace(h, "Build Range", find("hologram_range_limit", "41 BC 03 00 00 00 0F B6 C8 41"),
                   {0x41, 0xBC, 0x03, 0x00, 0x00, 0x00}, {0x41, 0xBC, 0x03, 0x00, 0x00, 0x00});
}

bool prepareFreeCrafting(Cheat& c)
{
    // Recipe cost: mov r15d,[rbp+138h] -> mov r15d,0 ; nop.
    Hook& h = newHook();
    c.hooks = {&h};
    return inPlace(h, "Free Crafting", find("free_craft", "44 8B BD 38 01 00 00"),
                   {0x44, 0x8B, 0xBD, 0x38, 0x01, 0x00, 0x00}, {0x41, 0xBF, 0x00, 0x00, 0x00, 0x00, 0x90});
}

bool prepareDurability(Cheat& c)
{
    // Durability change: add [rcx+rdx*4],r13d ; lea rcx,[rbp-31h]. Skip wear (r13d < 0).
    Hook& h = newHook();
    c.hooks = {&h};
    return caveHook(h, "No Durability Loss", find("weapon_durability", "44 01 2C 91 48 8D 4D CF"),
                    {0x44, 0x01, 0x2C, 0x91, 0x48, 0x8D, 0x4D, 0xCF}, [](patch::Emitter& e, uintptr_t back) {
                        e.bytes({0x45, 0x85, 0xED});          // test r13d,r13d
                        size_t skip = e.jcc(0x78);            // js skip
                        e.bytes({0x44, 0x01, 0x2C, 0x91});    // add [rcx+rdx*4],r13d
                        e.bind(skip);
                        e.bytes({0x48, 0x8D, 0x4D, 0xCF});    // lea rcx,[rbp-31h]
                        e.jmpAbs(back);
                    });
}

// --- CAMERA -----------------------------------------------------------------------------------

bool prepareFirstPerson(Cheat& c)
{
    // mov [rsi+7],al ; mov dl,23h. Force the build-mode first person byte.
    Hook& h = newHook();
    c.hooks = {&h};
    return caveHook(h, "First Person", find("firstperson_view", "88 46 07 B2 23"), {0x88, 0x46, 0x07, 0xB2, 0x23},
                    [](patch::Emitter& e, uintptr_t back) {
                        e.bytes({0x88, 0x46, 0x07})         // mov [rsi+7],al
                            .bytes({0xC6, 0x46, 0x07, 0x01}) // mov byte [rsi+7],1
                            .bytes({0xB2, 0x23})            // mov dl,23h
                            .jmpAbs(back);
                    });
}

bool prepareStableCamera(Cheat& c)
{
    // divss xmm7,xmm6 -> divss xmm6,xmm7: distance no longer rescaled.
    Hook& h = newHook();
    c.hooks = {&h};
    return inPlace(h, "Stable Camera Distance", find("stable_camera", "18 F2 0F 11 4C 24 50 0F", 0x0A),
                   {0xF3, 0x0F, 0x5E, 0xFE}, {0xF3, 0x0F, 0x5E, 0xF7});
}

bool prepareCameraDistance(Cheat& c)
{
    // movss [rsi+40h],xmm0 is the zoom distance; overwrite it with our value.
    Hook& h = newHook();
    c.hooks = {&h};
    c.value = dataF(c.info.def);
    static float* dist;
    dist = c.value;
    return caveHook(h, "Camera Distance", find("camera_distance", "F3 0F 11 46 40 F2"),
                    {0xF3, 0x0F, 0x11, 0x46, 0x40}, [](patch::Emitter& e, uintptr_t back) {
                        e.bytes({0xF3, 0x0F, 0x11, 0x46, 0x40}); // movss [rsi+40h],xmm0
                        e.bytes({0x50});                          // push rax
                        e.ripRel({0x8B, 0x05}, dist);             // mov eax,[dist]
                        e.bytes({0x89, 0x46, 0x40});              // mov [rsi+40h],eax
                        e.bytes({0x58});                          // pop rax
                        e.jmpAbs(back);
                    });
}

bool prepareCenterCamera(Cheat& c)
{
    // Camera offsets x/y/z (movss [rsi+34h/38h/3Ch],xmm) forced to 0 after each store.
    Hook& a = newHook();
    Hook& b = newHook();
    Hook& z = newHook();
    c.hooks = {&a, &b, &z};
    bool ok = caveHook(a, "Center Camera X", find("camera_offset_a", "F3 0F 11 46 34 0F"),
                       {0xF3, 0x0F, 0x11, 0x46, 0x34}, [](patch::Emitter& e, uintptr_t back) {
                           e.bytes({0xF3, 0x0F, 0x11, 0x46, 0x34}).bytes({0xC7, 0x46, 0x34}).imm32(0).jmpAbs(back);
                       });
    ok = caveHook(b, "Center Camera Y", find("camera_offset_b", "F3 0F 11 4E 38"), {0xF3, 0x0F, 0x11, 0x4E, 0x38},
                  [](patch::Emitter& e, uintptr_t back) {
                      e.bytes({0xF3, 0x0F, 0x11, 0x4E, 0x38}).bytes({0xC7, 0x46, 0x38}).imm32(0).jmpAbs(back);
                  }) && ok;
    ok = caveHook(z, "Center Camera Z", find("camera_offset_c", "F3 0F 11 4E 3C F3 0F 10"),
                  {0xF3, 0x0F, 0x11, 0x4E, 0x3C}, [](patch::Emitter& e, uintptr_t back) {
                      e.bytes({0xF3, 0x0F, 0x11, 0x4E, 0x3C}).bytes({0xC7, 0x46, 0x3C}).imm32(0).jmpAbs(back);
                  }) && ok;
    return ok;
}

// --- more movement, items and position --------------------------------------------------------

// Fly in look direction: the camera pitch store (mov [rbx+149Ch],eax) copies the pitch into our
// data; the vertical velocity add (addss xmm2,[rax+4]) becomes (pitch + 0.5) * speed for the
// player.
bool prepareFlyLook(Cheat& c)
{
    Hook& pitch = newHook();
    Hook& lift = newHook();
    c.hooks = {&pitch, &lift};
    c.value = dataF(c.info.def);
    static float *pitchV, *half, *speed;
    pitchV = dataF(0.0f);
    half = dataF(0.5f);
    speed = c.value;
    bool ok = caveHook(pitch, "Fly Look Pitch", find("pitch_angle", "89 83 9C 14 00 00"),
                       {0x89, 0x83, 0x9C, 0x14, 0x00, 0x00}, [](patch::Emitter& e, uintptr_t back) {
                           e.bytes({0x89, 0x83, 0x9C, 0x14, 0x00, 0x00}); // mov [rbx+149Ch],eax
                           e.ripRel({0x89, 0x05}, pitchV);                // mov [pitch],eax
                           e.jmpAbs(back);
                       });
    ok = caveHook(lift, "Fly Look Lift", find("flying_alt", "F3 0F 58 50 04 F3 0F 58 40 08"),
                  {0xF3, 0x0F, 0x58, 0x50, 0x04}, [](patch::Emitter& e, uintptr_t back) {
                      emitIsPlayer(e, 0xB8, -0x2B74);
                      size_t orig = e.jcc(0x75);
                      e.ripRel({0xF3, 0x0F, 0x58, 0x15}, pitchV); // addss xmm2,[pitch]
                      e.ripRel({0xF3, 0x0F, 0x58, 0x15}, half);   // addss xmm2,[0.5]
                      e.ripRel({0xF3, 0x0F, 0x59, 0x15}, speed);  // mulss xmm2,[speed]
                      e.jmpAbs(back);
                      e.bind(orig);
                      e.bytes({0xF3, 0x0F, 0x58, 0x50, 0x04});    // addss xmm2,[rax+4]
                      e.jmpAbs(back);
                  }) && ok;
    return ok;
}

bool prepareWalkUnderWater(Cheat& c)
{
    // Two returns of the movement-state function: mov al,2Ch / mov al,2Bh -> mov al,0.
    Hook& a = newHook();
    Hook& b = newHook();
    c.hooks = {&a, &b};
    const char* pattern = "48 8B 43 38 0F 57 C0 F3";
    bool ok = inPlace(a, "Walk Under Water A", find("walking_underwater_alt", pattern, -0x15), {0xB0, 0x2C},
                      {0xB0, 0x00});
    ok = inPlace(b, "Walk Under Water B", find("walking_underwater_alt", pattern, 0x15), {0xB0, 0x2B}, {0xB0, 0x00}) &&
         ok;
    return ok;
}

// Max stack: movzx eax,word [rax+14h] ; cmp ecx,eax ; ja full. Raise the limit in the register for
// items that already stack (limit > 1). Item definitions in memory are not written.
int32_t* g_stackMax = nullptr;

bool prepareStackSize(Cheat& c)
{
    Hook& h = newHook();
    c.hooks = {&h};
    g_stackMax = static_cast<int32_t*>(take(4, 4));
    *g_stackMax = int32_t(c.info.def);
    c.onValue = [](Cheat& self) { *g_stackMax = int32_t(self.cur + 0.5f); };
    return caveHook(h, "Max Stack Size", find("stack_size", "0F B7 40 14 3B C8"), {0x0F, 0xB7, 0x40, 0x14, 0x3B, 0xC8},
                    [](patch::Emitter& e, uintptr_t back) {
                        e.bytes({0x0F, 0xB7, 0x40, 0x14});        // movzx eax,word [rax+14h]
                        e.bytes({0x83, 0xF8, 0x01});              // cmp eax,1
                        size_t d1 = e.jcc(0x76);                  // jbe done
                        e.ripRel({0x3B, 0x05}, g_stackMax);       // cmp eax,[max]
                        size_t d2 = e.jcc(0x73);                  // jae done
                        e.ripRel({0x8B, 0x05}, g_stackMax);       // mov eax,[max]
                        e.bind(d1);
                        e.bind(d2);
                        e.bytes({0x3B, 0xC8});                    // cmp ecx,eax (flags for ja)
                        e.jmpAbs(back);
                    });
}

// Building material: movzx edx,byte [r14+454h] reads the selected block id from the build tool
// state (which the game itself writes). Write our id there first.
uint8_t* g_blockId = nullptr;

bool prepareBlockId(Cheat& c)
{
    Hook& h = newHook();
    c.hooks = {&h};
    g_blockId = dataB();
    *g_blockId = uint8_t(c.info.def);
    c.onValue = [](Cheat& self) { *g_blockId = uint8_t(self.cur + 0.5f); };
    return caveHook(h, "Block Material", find("id_building_block", "41 0F B6 96 54 04 00 00"),
                    {0x41, 0x0F, 0xB6, 0x96, 0x54, 0x04, 0x00, 0x00}, [](patch::Emitter& e, uintptr_t back) {
                        e.bytes({0x50});                                        // push rax
                        e.ripRel({0x8A, 0x05}, g_blockId);                      // mov al,[id]
                        e.bytes({0x41, 0x88, 0x86, 0x54, 0x04, 0x00, 0x00});    // mov [r14+454h],al
                        e.bytes({0x58});                                        // pop rax
                        e.bytes({0x41, 0x0F, 0xB6, 0x96, 0x54, 0x04, 0x00, 0x00}); // movzx edx,byte [r14+454h]
                        e.jmpAbs(back);
                    });
}

// Fall damage heights (vanilla min 8 / max 16) in the player movement config, the same block the
// slope thresholds live in. Player = 10.0f at [rax+3E94h]. OFF restores 8/16.
float* g_fallMin = nullptr;
float* g_fallMax = nullptr;
uintptr_t* g_fallPtr = nullptr;

bool prepareFallHeight(Cheat& c)
{
    Hook& h = newHook();
    c.hooks = {&h};
    g_fallMin = dataF(c.info.def);
    g_fallMax = dataF(c.info.def * 2.0f);
    g_fallPtr = dataPtr();
    c.onValue = [](Cheat& self) {
        *g_fallMin = self.cur;
        *g_fallMax = self.cur * 2.0f;
    };
    c.customApply = [](Cheat& self, bool on) {
        bool now = applyAll(self, on);
        if (!on && *g_fallPtr) {
            const float vanilla[2] = {8.0f, 16.0f};
            if (safeWrite(reinterpret_cast<void*>(*g_fallPtr), vanilla, sizeof(vanilla)))
                LOG("Safe Fall Height: restored 8/16");
        }
        return now;
    };
    return caveHook(h, "Safe Fall Height", find("fall_damage_height", "F3 0F 10 00 0F 2F 40 04"),
                    {0xF3, 0x0F, 0x10, 0x00, 0x0F, 0x2F, 0x40, 0x04}, [](patch::Emitter& e, uintptr_t back) {
                        emitIsPlayer(e, 0xB8, 0x3E94);            // cmp dword [rax+3E94h],10.0f
                        size_t orig = e.jcc(0x75);
                        e.ripRel({0x48, 0x89, 0x05}, g_fallPtr);  // mov [fallPtr],rax
                        e.bytes({0x52});                          // push rdx
                        e.ripRel({0x8B, 0x15}, g_fallMin);        // mov edx,[min]
                        e.bytes({0x89, 0x10});                    // mov [rax],edx
                        e.ripRel({0x8B, 0x15}, g_fallMax);        // mov edx,[max]
                        e.bytes({0x89, 0x50, 0x04});              // mov [rax+4],edx
                        e.bytes({0x5A});                          // pop rdx
                        e.bind(orig);
                        e.bytes({0xF3, 0x0F, 0x10, 0x00});        // movss xmm0,[rax]
                        e.bytes({0x0F, 0x2F, 0x40, 0x04});        // comiss xmm0,[rax+4] (flags for jb)
                        e.jmpAbs(back);
                    });
}

// Position capture: mov rax,[rdx+8] ; sub rax,[rcx+8] in a distance function. rdx is captured
// only when it looks like the player movement component (run speed 5.0 at +2958h). The position
// is three int64 32.32 fixed-point values at +0/+8/+10h; the high dwords are the integer part.
Hook g_coordHook;
uintptr_t* g_coordPtr = nullptr;
int64_t g_savedPos[3] = {};
bool g_hasSaved = false;

void prepareCoordinates()
{
    g_coordPtr = dataPtr();
    bool ok = caveHook(g_coordHook, "Position Capture",
                       find("coordinates", "48 8B 42 08 48 2B 41 08 F3 48 0F 2A F0 48 8B 42 10 48 2B 41 10 F3 0F"),
                       {0x48, 0x8B, 0x42, 0x08, 0x48, 0x2B, 0x41, 0x08}, [](patch::Emitter& e, uintptr_t back) {
                           e.bytes({0x81, 0xBA, 0x58, 0x29, 0x00, 0x00}).f32(5.0f); // cmp dword [rdx+2958h],5.0f
                           size_t orig = e.jcc(0x75);
                           e.ripRel({0x48, 0x89, 0x15}, g_coordPtr);              // mov [coordPtr],rdx
                           e.bind(orig);
                           e.bytes({0x48, 0x8B, 0x42, 0x08});                     // mov rax,[rdx+8]
                           e.bytes({0x48, 0x2B, 0x41, 0x08});                     // sub rax,[rcx+8]
                           e.jmpAbs(back);
                       });
    if (ok)
        apply(g_coordHook, true); // harmless read-only capture, always on
}

// --- registry ---------------------------------------------------------------------------------

Info toggle(const char* tab, const char* section, const char* label)
{
    Info i;
    i.tab = tab;
    i.section = section;
    i.label = label;
    return i;
}

Info slider(const char* tab, const char* section, const char* label, float min, float max, float def, float step,
            const char* fmt)
{
    Info i = toggle(tab, section, label);
    i.kind = Kind::Slider;
    i.min = min;
    i.max = max;
    i.def = def;
    i.step = step;
    i.fmt = fmt;
    return i;
}

Info toggleSlider(const char* tab, const char* section, const char* label, float min, float max, float def,
                  float step, const char* fmt)
{
    Info i = slider(tab, section, label, min, max, def, step, fmt);
    i.kind = Kind::ToggleSlider;
    return i;
}

Info soon(const char* tab, const char* section, const char* label)
{
    Info i = toggle(tab, section, label);
    i.soon = true;
    return i;
}

void add(Info info, bool (*prepare)(Cheat&))
{
    Cheat c;
    c.info = info;
    c.prepare = prepare;
    c.cur = info.def;
    g_cheats.push_back(std::move(c));
}

void buildRegistry()
{
    add(toggle("Player", "SURVIVAL", "God Mode"), [](Cheat& c) {
        bool ok = prepareHealth(); // creates g_health
        c.hooks = {g_health};
        c.customApply = [](Cheat&, bool on) { return applyHealthFlag(g_godFlag, on); };
        return ok;
    });
    add(toggle("Player", "SURVIVAL", "One-Hit Kill"), [](Cheat& c) {
        bool ok = prepareHealth();
        c.hooks = {g_health};
        c.customApply = [](Cheat&, bool on) { return applyHealthFlag(g_killFlag, on); };
        return ok;
    });
    add(toggle("Player", "SURVIVAL", "Infinite Stamina"), prepareStamina);
    add(toggle("Player", "SURVIVAL", "Infinite Mana"), prepareMana);
    add(toggle("Player", "SURVIVAL", "No Fall Damage"), prepareFallDamage);
    add(toggleSlider("Player", "SURVIVAL", "Safe Fall Height", 8.0f, 200.0f, 50.0f, 1.0f, "%.0f m"),
        prepareFallHeight);
    add(toggle("Player", "ENVIRONMENT", "Infinite Oxygen"), prepareOxygen);
    add(toggle("Player", "ENVIRONMENT", "Infinite Frost"), prepareFrost);
    add(toggle("Player", "ENVIRONMENT", "Infinite Shroud Timer"), prepareShroud);

    add(slider("Move", "SPEED", "Run Speed", 1.0f, 3.0f, 1.0f, 0.1f, "%.1fx"), prepareRunSpeed);
    add(slider("Move", "SPEED", "High Jump", 1.0f, 5.0f, 1.0f, 0.1f, "%.1fx"), prepareHighJump);
    add(slider("Move", "SPEED", "Gravity", 0.0f, 25.0f, 25.0f, 0.5f, "%.1f"), prepareGravity);
    add(slider("Move", "SPEED", "Fall Speed", 0.05f, 1.0f, 1.0f, 0.05f, "%.2fx"), prepareFallSpeed);
    add(toggle("Move", "ABILITIES", "Infinite Jumps (E)"), prepareInfiniteJumps);
    add(toggle("Move", "ABILITIES", "Fly (Build Mode)"), prepareFly);
    add(toggleSlider("Move", "ABILITIES", "Fly (Look Direction)", 1.0f, 20.0f, 7.5f, 0.5f, "%.1f"), prepareFlyLook);
    add(toggle("Move", "ABILITIES", "Walk Under Water"), prepareWalkUnderWater);
    add(toggle("Move", "ABILITIES", "Swim in Air"), prepareSwimInAir);
    add(toggle("Move", "ABILITIES", "Climb Any Slope"), prepareClimb);
    add(toggle("Move", "ABILITIES", "Glider Freedom"), prepareGlider);

    add(toggle("World", "TIME", "Always Day"), prepareAlwaysDay);
    add(soon("World", "TIME", "Freeze Time"), nullptr);
    add(toggle("World", "WATER", "Transparent Water"), prepareTransparentWater);
    add(toggle("World", "WATER", "Invisible Water"), prepareInvisibleWater);
    add(toggleSlider("World", "WATER", "Water Glow", 0.0f, 50.0f, 25.0f, 0.5f, "%.1f"), prepareWaterGlow);

    add(toggle("Build", "BUILDING", "Build Outside Altar"),
        [](Cheat& c) { return prepareBuildFlag(c, "Build Outside Altar", "altar_area", "41 83 0F 02 8B C3", 0x02); });
    add(toggle("Build", "BUILDING", "Build in Shroud"), [](Cheat& c) {
        return prepareBuildFlag(c, "Build in Shroud", "fog_area", "41 83 0F 04 F3 0F 10 44 24", 0x04);
    });
    add(toggle("Build", "BUILDING", "Build in Crypts"),
        [](Cheat& c) { return prepareBuildFlag(c, "Build in Crypts", "crypt_area", "41 83 0F 10 * * 0F 2F D5", 0x10); });
    add(toggle("Build", "BUILDING", "Altar From Any Distance"), prepareAltarFar);
    add(slider("Build", "BUILDING", "Build Range", 3.0f, 32.0f, 3.0f, 1.0f, "%.0f"), prepareBuildRange);
    add(toggleSlider("Build", "BUILDING", "Block Material", 0.0f, 255.0f, 167.0f, 1.0f, "id %.0f"), prepareBlockId);
    add(toggle("Build", "CRAFTING", "Free Crafting"), prepareFreeCrafting);
    add(toggle("Build", "CRAFTING", "No Durability Loss"), prepareDurability);
    add(toggleSlider("Build", "CRAFTING", "Max Stack Size", 10.0f, 9990.0f, 999.0f, 10.0f, "%.0f"), prepareStackSize);

    add(toggle("Camera", "CAMERA", "First Person"), prepareFirstPerson);
    add(toggle("Camera", "CAMERA", "Stable Camera Distance"), prepareStableCamera);
    add(toggleSlider("Camera", "CAMERA", "Camera Distance", 0.0f, 20.0f, 4.0f, 0.5f, "%.1f"), prepareCameraDistance);
    add(toggle("Camera", "CAMERA", "Center Camera"), prepareCenterCamera);
}

Cheat* at(int i)
{
    return i >= 0 && i < int(g_cheats.size()) ? &g_cheats[i] : nullptr;
}

bool applyCheat(Cheat& c, bool on)
{
    return c.customApply ? c.customApply(c, on) : applyAll(c, on);
}

} // namespace

void init()
{
    if (g_initDone)
        return;
    g_initDone = true;

    g_page = patch::allocNear(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)), kPageSize);
    if (!g_page)
        LOG("cheats: no code cave memory, all cheats unavailable");
    else
        g_probes = static_cast<Probe*>(take(3 * sizeof(Probe), 16));

    buildRegistry();
    if (!g_page)
        return;
    for (Cheat& c : g_cheats) {
        if (c.prepare)
            c.ready = c.prepare(c);
        if (c.value)
            *c.value = c.cur;
    }
    prepareCoordinates();
    LOG("cheats: %d registered, %d bytes of cave memory used", int(g_cheats.size()), int(g_used));
}

int count()
{
    return int(g_cheats.size());
}

const Info& info(int i)
{
    return g_cheats[i].info;
}

bool available(int i)
{
    Cheat* c = at(i);
    return c && c->ready && !c->info.soon;
}

bool enabled(int i)
{
    Cheat* c = at(i);
    return c && c->on;
}

float value(int i)
{
    Cheat* c = at(i);
    return c ? c->cur : 0.0f;
}

bool setEnabled(int i, bool on)
{
    Cheat* c = at(i);
    if (!c || !available(i))
        return false;
    c->on = applyCheat(*c, on);
    return c->on;
}

void setValue(int i, float v)
{
    Cheat* c = at(i);
    if (!c || !available(i))
        return;
    c->cur = v;
    if (c->value)
        *c->value = v;
    if (c->onValue)
        c->onValue(*c);
    if (c->info.kind == Kind::Slider) {
        bool want = v < c->info.def - 0.001f || v > c->info.def + 0.001f;
        c->on = applyCheat(*c, want);
    }
}

int activeCount()
{
    int n = 0;
    for (const Cheat& c : g_cheats)
        n += c.on;
    return n;
}

bool position(int pos[3])
{
    if (!g_coordPtr || !*g_coordPtr)
        return false;
    int64_t raw[3];
    if (!safeRead(reinterpret_cast<const void*>(*g_coordPtr), raw, sizeof(raw)))
        return false;
    for (int i = 0; i < 3; ++i)
        pos[i] = int(raw[i] >> 32);
    return true;
}

bool hasSavedPosition()
{
    return g_hasSaved;
}

void savePosition()
{
    if (g_coordPtr && *g_coordPtr && safeRead(reinterpret_cast<const void*>(*g_coordPtr), g_savedPos, sizeof(g_savedPos))) {
        g_hasSaved = true;
        LOG("position saved: %d %d %d", int(g_savedPos[0] >> 32), int(g_savedPos[1] >> 32), int(g_savedPos[2] >> 32));
    }
}

bool teleportToSaved()
{
    if (!g_hasSaved || !g_coordPtr || !*g_coordPtr)
        return false;
    bool ok = safeWrite(reinterpret_cast<void*>(*g_coordPtr), g_savedPos, sizeof(g_savedPos));
    LOG("teleport to saved position: %s", ok ? "ok" : "failed");
    return ok;
}

void nudgeUp(int units)
{
    if (!g_coordPtr || !*g_coordPtr)
        return;
    auto* y = reinterpret_cast<int64_t*>(*g_coordPtr + 8);
    int64_t v;
    if (safeRead(y, &v, sizeof(v))) {
        v += int64_t(units) << 32;
        safeWrite(y, &v, sizeof(v));
    }
}

void debugTick()
{
    static ULONGLONG last = 0;
    static uint32_t prevHits[3] = {};
    ULONGLONG now = GetTickCount64();
    if (!g_probes || now - last < 10000)
        return;
    last = now;

    const char* names[3] = {"Health", "Stamina", "Mana"};
    for (int i = 0; i < 3; ++i) {
        uint32_t hits = g_probes[i].hits;
        if (hits == prevHits[i])
            continue; // hook off or idle
        auto ptr = reinterpret_cast<const int32_t*>(uintptr_t(g_probes[i].ptr));
        int32_t v[12] = {};
        bool ok = ptr && safeRead(ptr - 4, v, sizeof(v));
        // v[4] is the slot itself; 4 dwords before, 7 after.
        LOG("%s probe: +%u hits, slot %p = %d | -10h: %d %d %d %d | +4h..: %d %d %d %d %d %d %d%s", names[i],
            hits - prevHits[i], ptr, v[4], v[0], v[1], v[2], v[3], v[5], v[6], v[7], v[8], v[9], v[10], v[11],
            ok ? "" : " (unreadable)");
        prevHits[i] = hits;
    }
}

} // namespace cheats
