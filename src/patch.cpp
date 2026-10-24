#include "patch.h"

#include "log.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstring>

namespace patch {
namespace {

struct Range {
    const uint8_t* begin = nullptr;
    size_t size = 0;
};

Range textSection()
{
    static Range text;
    if (text.begin)
        return text;
    auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 5) == 0) {
            text.begin = base + sec->VirtualAddress;
            text.size = sec->Misc.VirtualSize;
            break;
        }
    }
    return text;
}

bool parsePattern(const char* pattern, std::vector<uint8_t>& bytes, std::vector<bool>& mask)
{
    for (const char* p = pattern; *p;) {
        if (*p == ' ') {
            ++p;
        } else if (*p == '*' || *p == '?') {
            while (*p == '*' || *p == '?')
                ++p;
            bytes.push_back(0);
            mask.push_back(false);
        } else {
            char* end = nullptr;
            unsigned long v = strtoul(p, &end, 16);
            if (end == p || v > 0xFF)
                return false;
            bytes.push_back(uint8_t(v));
            mask.push_back(true);
            p = end;
        }
    }
    return !bytes.empty() && mask[0];
}

} // namespace

uintptr_t scan(const char* name, const char* pattern)
{
    std::vector<uint8_t> bytes;
    std::vector<bool> mask;
    if (!parsePattern(pattern, bytes, mask)) {
        LOG("scan %s: bad pattern", name);
        return 0;
    }
    Range text = textSection();
    const uint8_t* end = text.begin + text.size - bytes.size();
    uintptr_t first = 0;
    int count = 0;
    for (const uint8_t* p = text.begin; p <= end;) {
        p = static_cast<const uint8_t*>(memchr(p, bytes[0], size_t(end - p) + 1));
        if (!p)
            break;
        size_t i = 1;
        while (i < bytes.size() && (!mask[i] || p[i] == bytes[i]))
            ++i;
        if (i == bytes.size()) {
            if (!first)
                first = reinterpret_cast<uintptr_t>(p);
            ++count;
        }
        ++p;
    }
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (first)
        LOG("scan %s: %d match(es), first at enshrouded.exe+%llX", name, count,
            (unsigned long long)(first - base));
    else
        LOG("scan %s: NOT FOUND", name);
    return first;
}

uint8_t* allocNear(uintptr_t target, size_t size)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t span = 0x7FF00000; // stay safely inside rel32 range
    uintptr_t lo = target > span ? target - span : gran;
    uintptr_t hi = target + span;

    // Walk outward from the target in both directions, one allocation granule at a time.
    for (uintptr_t step = gran; step < span; step += gran) {
        for (uintptr_t addr : {(target & ~(gran - 1)) + step, (target & ~(gran - 1)) - step}) {
            if (addr < lo || addr > hi)
                continue;
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) || mbi.State != MEM_FREE)
                continue;
            void* p = VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_RESERVE | MEM_COMMIT,
                                   PAGE_EXECUTE_READWRITE);
            if (p)
                return static_cast<uint8_t*>(p);
        }
    }
    LOG("allocNear failed for %p", reinterpret_cast<void*>(target));
    return nullptr;
}

bool writeCode(uintptr_t addr, const void* bytes, size_t n)
{
    // Collect thread ids first; nothing below may allocate while threads are suspended
    // (a suspended thread could hold the heap lock).
    constexpr int kMaxThreads = 1024;
    static DWORD ids[kMaxThreads];
    static HANDLE handles[kMaxThreads];
    int idCount = 0;
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok && idCount < kMaxThreads; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self)
            ids[idCount++] = te.th32ThreadID;
    CloseHandle(snap);

    for (int attempt = 0; attempt < 20; ++attempt) {
        int suspended = 0;
        bool busy = false;
        for (int i = 0; i < idCount; ++i) {
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, ids[i]);
            if (!h)
                continue;
            if (SuspendThread(h) == DWORD(-1)) {
                CloseHandle(h);
                continue;
            }
            handles[suspended++] = h;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(h, &ctx) && ctx.Rip >= addr && ctx.Rip < addr + n)
                busy = true;
        }

        if (!busy) {
            DWORD old;
            VirtualProtect(reinterpret_cast<void*>(addr), n, PAGE_EXECUTE_READWRITE, &old);
            memcpy(reinterpret_cast<void*>(addr), bytes, n);
            VirtualProtect(reinterpret_cast<void*>(addr), n, old, &old);
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), n);
        }
        for (int i = 0; i < suspended; ++i) {
            ResumeThread(handles[i]);
            CloseHandle(handles[i]);
        }
        if (!busy)
            return true;
        Sleep(1);
    }
    LOG("writeCode %p: a thread kept executing the patch site, gave up", reinterpret_cast<void*>(addr));
    return false;
}

Emitter& Emitter::bytes(std::initializer_list<uint8_t> b)
{
    code_.insert(code_.end(), b);
    return *this;
}

Emitter& Emitter::imm32(uint32_t v)
{
    const auto* b = reinterpret_cast<const uint8_t*>(&v);
    code_.insert(code_.end(), b, b + 4);
    return *this;
}

Emitter& Emitter::f32(float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    return imm32(u);
}

size_t Emitter::jcc(uint8_t opcode)
{
    code_.push_back(opcode);
    code_.push_back(0);
    return code_.size() - 1;
}

void Emitter::bind(size_t fixup)
{
    code_[fixup] = uint8_t(int8_t(code_.size() - (fixup + 1)));
}

Emitter& Emitter::ripRel(std::initializer_list<uint8_t> prefix, const void* target, int trailing)
{
    code_.insert(code_.end(), prefix);
    auto next = reinterpret_cast<intptr_t>(base_ + code_.size() + 4 + trailing);
    auto rel = int32_t(reinterpret_cast<intptr_t>(target) - next);
    const auto* r = reinterpret_cast<const uint8_t*>(&rel);
    code_.insert(code_.end(), r, r + 4);
    return *this;
}

Emitter& Emitter::jmpAbs(uintptr_t target)
{
    bytes({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
    const auto* t = reinterpret_cast<const uint8_t*>(&target);
    code_.insert(code_.end(), t, t + 8);
    return *this;
}

void Emitter::commit() const
{
    memcpy(base_, code_.data(), code_.size());
    FlushInstructionCache(GetCurrentProcess(), base_, code_.size());
}

} // namespace patch
