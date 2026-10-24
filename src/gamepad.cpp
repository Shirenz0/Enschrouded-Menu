// Controller support. Enshrouded loads xinput1_4.dll at runtime and calls XInputGetState through
// GetProcAddress, so the function itself is hooked: its first instruction (mov [rsp+8],rbx, 5
// bytes) moves to a trampoline and the entry jumps to our wrapper. The menu reads the pad through
// the trampoline too, so it works whether or not the game polls XInput.

#include "gamepad.h"

#include "input.h"
#include "log.h"
#include "patch.h"
#include "steaminput.h"

#include "imgui.h"

#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace gamepad {
namespace {

using GetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

// L3 + R3 (both sticks pressed). View+Menu is often bound to a Steam or system chord.
constexpr WORD kCombo = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
constexpr SHORT kStickDead = 12000;

GetState_t g_real = nullptr; // trampoline (or the plain export if hooking failed)
bool g_installed = false;
int g_pad = -1;              // first connected pad index
int g_rescan = 0;
WORD g_prevButtons = 0;
XINPUT_GAMEPAD g_pad_state = {};
std::atomic<bool> g_active{false};
std::atomic<uint32_t> g_gameCalls{0}; // calls that came from the game (we read via g_real directly)

// The export may already carry a hook (Steam's overlay). Its handler reaches the original by calling
// the export again, which now lands in our hook: without a guard that recursed until the stack ran
// out (crash 2026-10-24). So: g_real is the chain we found (their hook, or the original), g_orig is
// the original code rebuilt from the DLL on disk, and a per-thread depth sends re-entries to g_orig.
GetState_t g_orig = nullptr;
thread_local int t_depth = 0;

DWORD callChain(DWORD index, XINPUT_STATE* state)
{
    ++t_depth;
    DWORD res = g_real(index, state);
    --t_depth;
    return res;
}

DWORD WINAPI hookGetState(DWORD index, XINPUT_STATE* state)
{
    if (t_depth > 0) // a hook further down the chain calling the export again
        return g_orig(index, state);
    ++g_gameCalls;
    DWORD res = callChain(index, state);
    if (res == ERROR_SUCCESS && state) {
        bool combo = (state->Gamepad.wButtons & kCombo) == kCombo;
        if (input::menuOpen() || combo) {
            // Idle pad for the game; a different packet number makes it pick up the change.
            memset(&state->Gamepad, 0, sizeof(state->Gamepad));
            state->dwPacketNumber ^= 0x5A5A5A5A;
        }
    }
    return res;
}

// Where the export continues when someone already put a jump on it, and how many bytes that jump
// covers; 0 if the first bytes are not a jump we understand.
uintptr_t existingJump(const uint8_t* fn, size_t* len)
{
    if (fn[0] == 0xE9) { // jmp rel32
        int32_t rel;
        memcpy(&rel, fn + 1, 4);
        *len = 5;
        return reinterpret_cast<uintptr_t>(fn) + 5 + rel;
    }
    if (fn[0] == 0xFF && fn[1] == 0x25) { // jmp qword [rip+rel32]
        int32_t rel;
        memcpy(&rel, fn + 2, 4);
        uintptr_t target;
        memcpy(&target, fn + 6 + rel, 8);
        *len = rel == 0 ? 14 : 6; // rel 0: the address sits right behind the instruction
        return target;
    }
    if (fn[0] == 0x48 && fn[1] == 0xB8 && fn[10] == 0xFF && fn[11] == 0xE0) { // mov rax,imm64 ; jmp rax
        uintptr_t target;
        memcpy(&target, fn + 2, 8);
        *len = 12;
        return target;
    }
    return 0;
}

// The export's first bytes as they are in the DLL file (memory may already be patched).
bool readFromDisk(HMODULE dll, const uint8_t* fn, uint8_t* out, size_t n)
{
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(dll, path, MAX_PATH))
        return false;
    auto* base = reinterpret_cast<const uint8_t*>(dll);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    DWORD rva = DWORD(fn - base);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (rva < sec->VirtualAddress || rva >= sec->VirtualAddress + sec->Misc.VirtualSize)
            continue;
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            return false;
        LARGE_INTEGER off;
        off.QuadPart = sec->PointerToRawData + (rva - sec->VirtualAddress);
        DWORD got = 0;
        bool ok = SetFilePointerEx(f, off, nullptr, FILE_BEGIN) && ReadFile(f, out, DWORD(n), &got, nullptr) && got == n;
        CloseHandle(f);
        return ok;
    }
    return false;
}

bool hookExport(HMODULE dll, uint8_t* fn)
{
    // xinput1_4!XInputGetState as shipped: mov [rsp+8],rbx ; push rsi ; push rdi ; push r14 ;
    // sub rsp,30h ; mov rdi,rdx ; mov esi,ecx. None of these is rip-relative, so any prefix that
    // ends on an instruction boundary can run from a copy.
    const uint8_t expected[18] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x56, 0x57, 0x41, 0x56,
                                  0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xFA, 0x8B, 0xF1};
    const size_t boundaries[] = {5, 6, 7, 9, 13, 16, 18};
    uint8_t disk[18];
    if (!readFromDisk(dll, fn, disk, sizeof(disk)) || memcmp(disk, expected, sizeof(disk)) != 0) {
        LOG("gamepad: xinput1_4 XInputGetState is not the expected build, game input not blocked");
        return false;
    }

    size_t covered = 0; // bytes someone else's jump overwrote
    uintptr_t chained = 0;
    if (memcmp(fn, expected, 5) != 0) {
        chained = existingJump(fn, &covered);
        if (!chained) {
            LOG("gamepad: unknown XInputGetState start %02X %02X %02X %02X %02X %02X, game input not blocked", fn[0],
                fn[1], fn[2], fn[3], fn[4], fn[5]);
            return false;
        }
    }
    // Copy whole original instructions covering both their jump and our 5-byte one.
    size_t need = covered > 5 ? covered : 5, copy = 0;
    for (size_t b : boundaries)
        if (b >= need) {
            copy = b;
            break;
        }
    if (!copy || memcmp(fn + copy, expected + copy, sizeof(expected) - copy) != 0) {
        LOG("gamepad: XInputGetState body differs past the hook, game input not blocked");
        return false;
    }

    uint8_t* page = patch::allocNear(reinterpret_cast<uintptr_t>(fn), 0x1000);
    if (!page)
        return false;

    // Original: copied instructions, then on into the untouched rest of the export.
    patch::Emitter orig(page);
    for (size_t i = 0; i < copy; ++i)
        orig.bytes({disk[i]});
    orig.jmpAbs(reinterpret_cast<uintptr_t>(fn) + copy).commit();
    g_orig = reinterpret_cast<GetState_t>(page);

    // Chain: whoever hooked the export before us, or the original.
    uint8_t* chain = page + 0x40;
    if (chained) {
        patch::Emitter(chain).jmpAbs(chained).commit();
        LOG("gamepad: XInputGetState already hooked (Steam?) [%02X %02X %02X %02X %02X %02X, %d bytes], chaining to "
            "%p with re-entry guard",
            fn[0], fn[1], fn[2], fn[3], fn[4], fn[5], int(covered), reinterpret_cast<void*>(chained));
    } else {
        chain = page;
    }

    // Entry stub reachable with rel32 from the export: jump to our wrapper anywhere in memory.
    uint8_t* stub = page + 0x80;
    patch::Emitter(stub).jmpAbs(reinterpret_cast<uintptr_t>(&hookGetState)).commit();

    g_real = reinterpret_cast<GetState_t>(chain);
    uint8_t jmp[5] = {0xE9};
    auto rel = int32_t(reinterpret_cast<intptr_t>(stub) - reinterpret_cast<intptr_t>(fn + 5));
    memcpy(&jmp[1], &rel, 4);
    if (!patch::writeCode(reinterpret_cast<uintptr_t>(fn), jmp, 5)) {
        g_real = reinterpret_cast<GetState_t>(fn); // nothing patched, read directly
        return false;
    }
    (void)dll;
    return true;
}

bool read(XINPUT_STATE& s)
{
    if (!g_real)
        return false;
    if (g_pad < 0) {
        if (g_rescan-- > 0) // probing disconnected pads is slow; only every ~60 frames
            return false;
        g_rescan = 60;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            if (callChain(i, &s) == ERROR_SUCCESS) {
                g_pad = int(i);
                LOG("gamepad: controller %d connected", g_pad);
                return true;
            }
        }
        return false;
    }
    if (callChain(DWORD(g_pad), &s) != ERROR_SUCCESS) {
        LOG("gamepad: controller %d disconnected", g_pad);
        g_pad = -1;
        return false;
    }
    return true;
}

} // namespace

void install()
{
    if (g_installed)
        return;
    g_installed = true;
    HMODULE dll = LoadLibraryW(L"xinput1_4.dll");
    auto* fn = dll ? reinterpret_cast<uint8_t*>(GetProcAddress(dll, "XInputGetState")) : nullptr;
    if (!fn) {
        LOG("gamepad: xinput1_4.dll / XInputGetState not available");
        return;
    }
    if (hookExport(dll, fn))
        LOG("gamepad: XInputGetState hooked, L3+R3 toggles the menu");
    else if (!g_real)
        g_real = reinterpret_cast<GetState_t>(fn);
}

void poll()
{
    // Shows whether the game itself reads XInput at all (it may use Steam Input / DirectInput).
    static ULONGLONG lastReport = 0;
    ULONGLONG now = GetTickCount64();
    steaminput::tryInstall();
    if (now - lastReport > 10000) {
        if (lastReport) {
            LOG("gamepad: game reads in last 10 s: XInput %u, raw input HID %u", g_gameCalls.exchange(0),
                input::takeHidReads());
            steaminput::logCounts();
        }
        lastReport = now;
    }

    XINPUT_STATE s{};
    if (!read(s)) {
        g_pad_state = {};
        g_prevButtons = 0;
        return;
    }
    g_pad_state = s.Gamepad;
    WORD b = s.Gamepad.wButtons;
    WORD pressed = b & ~g_prevButtons;
    g_prevButtons = b;

    if ((b & kCombo) == kCombo && (pressed & kCombo)) {
        input::toggleMenu();
        g_active = true;
    } else if (input::menuOpen() && (pressed & XINPUT_GAMEPAD_B)) {
        input::toggleMenu();
    }
    if (input::menuOpen() && (b || abs(s.Gamepad.sThumbLX) > kStickDead || abs(s.Gamepad.sThumbLY) > kStickDead))
        g_active = true;
}

void feedImGui(ImGuiIO& io)
{
    // ImGui's own gamepad nav stays off: the menu runs its own focus (menu.cpp PadNav), and with
    // both active A would trigger twice.
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    if (!input::menuOpen())
        return;
    const XINPUT_GAMEPAD& g = g_pad_state;
    auto key = [&](ImGuiKey k, WORD mask) { io.AddKeyEvent(k, (g.wButtons & mask) != 0); };
    key(ImGuiKey_GamepadDpadUp, XINPUT_GAMEPAD_DPAD_UP);
    key(ImGuiKey_GamepadDpadDown, XINPUT_GAMEPAD_DPAD_DOWN);
    key(ImGuiKey_GamepadDpadLeft, XINPUT_GAMEPAD_DPAD_LEFT);
    key(ImGuiKey_GamepadDpadRight, XINPUT_GAMEPAD_DPAD_RIGHT);
    key(ImGuiKey_GamepadFaceDown, XINPUT_GAMEPAD_A);
    key(ImGuiKey_GamepadFaceRight, XINPUT_GAMEPAD_B);
    key(ImGuiKey_GamepadL1, XINPUT_GAMEPAD_LEFT_SHOULDER);
    key(ImGuiKey_GamepadR1, XINPUT_GAMEPAD_RIGHT_SHOULDER);
    auto stick = [&](ImGuiKey k, SHORT v, bool positive) {
        float f = positive ? float(v - kStickDead) / (32767 - kStickDead) : float(-v - kStickDead) / (32767 - kStickDead);
        f = f < 0 ? 0 : (f > 1 ? 1 : f);
        io.AddKeyAnalogEvent(k, f > 0.1f, f);
    };
    stick(ImGuiKey_GamepadLStickLeft, g.sThumbLX, false);
    stick(ImGuiKey_GamepadLStickRight, g.sThumbLX, true);
    stick(ImGuiKey_GamepadLStickUp, g.sThumbLY, true);
    stick(ImGuiKey_GamepadLStickDown, g.sThumbLY, false);
}

bool active()
{
    return g_active;
}

void mouseUsed()
{
    g_active = false;
}

} // namespace gamepad
