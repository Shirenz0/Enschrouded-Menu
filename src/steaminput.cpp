// Controller blocking and diagnostics for the pad APIs the game might use. XInput and raw input HID
// both showed 0 game reads, so this covers the rest:
// - ISteamInput (SteamInput007): slot 3 RunFrame, 17 GetDigitalActionData, 21 GetAnalogActionData
// - ISteamController (SteamController008): slot 2 RunFrame, 12 GetDigitalActionData, 15 GetAnalogActionData
// - IDirectInputDevice8W: slot 9 GetDeviceState, 10 GetDeviceData (counted only, per buffer size)
// Slot numbers come from the Steamworks SDK headers and dinput.h. Steam wrappers are member
// functions so they share the originals' exact ABI (this in rcx, hidden return pointer in rdx).

#include "steaminput.h"

#include "input.h"
#include "log.h"

#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <atomic>
#include <cstring>

namespace steaminput {
namespace {

#pragma pack(push, 1)
struct DigitalData {
    bool state;
    bool active;
};
struct AnalogData {
    int mode;
    float x, y;
    bool active;
};
// SteamInputActionEvent_t (isteaminput.h, pack 1): controller 0, type 8, action handle 12, data 20.
struct ActionEvent {
    uint64_t controller;
    int type; // 0 digital, 1 analog
    uint64_t action;
    union {
        DigitalData digital;
        AnalogData analog;
    };
};
#pragma pack(pop)
static_assert(sizeof(ActionEvent) == 33, "SteamInputActionEvent_t layout");

using EventCallback = void (*)(ActionEvent*);
EventCallback g_gameCallback = nullptr; // the game's own handler, found by diffing memory
std::atomic<uint32_t> g_events{0};

// Last event per action, to send neutral copies when the menu opens (a held stick would
// otherwise keep the character moving: Steam only sends events when values change).
constexpr int kMaxActions = 64;
ActionEvent g_last[kMaxActions];
int g_lastCount = 0;
bool g_neutralSent = false;

void rememberEvent(const ActionEvent& ev)
{
    for (int i = 0; i < g_lastCount; ++i)
        if (g_last[i].action == ev.action && g_last[i].type == ev.type) {
            g_last[i] = ev;
            return;
        }
    if (g_lastCount < kMaxActions)
        g_last[g_lastCount++] = ev;
}

void neutralize(ActionEvent& ev)
{
    if (ev.type == 0)
        ev.digital.state = false;
    else
        ev.analog.x = ev.analog.y = 0.0f;
}

void onActionEvent(ActionEvent* ev)
{
    ++g_events;
    if (!ev)
        return;
    rememberEvent(*ev);
    if (input::menuOpen())
        neutralize(*ev);
    if (g_gameCallback)
        g_gameCallback(ev);
}

// Called on the game's thread (its RunFrame): one neutral event per known action as the menu opens.
void sendNeutralOnOpen()
{
    if (!input::menuOpen()) {
        g_neutralSent = false;
        return;
    }
    if (g_neutralSent || !g_gameCallback)
        return;
    g_neutralSent = true;
    for (int i = 0; i < g_lastCount; ++i) {
        ActionEvent ev = g_last[i];
        neutralize(ev);
        g_gameCallback(&ev);
    }
}

struct Counters {
    std::atomic<uint32_t> runFrame{0}, digital{0}, analog{0};
};
Counters g_si, g_sc; // SteamInput007, SteamController008
std::atomic<uint32_t> g_diState[4] = {}; // DIJOYSTATE(80) / DIJOYSTATE2(272) / keyboard(256) / other
std::atomic<uint32_t> g_diData{0};

void blockDigital(DigitalData& d)
{
    if (input::menuOpen())
        d.state = false; // released; bActive untouched so the game keeps the controller
}

void blockAnalog(AnalogData& a)
{
    if (input::menuOpen()) {
        a.x = 0.0f; // centred stick / released trigger
        a.y = 0.0f;
    }
}

// One wrapper class per interface so each has its own originals.
class SteamInputHook {
public:
    void runFrame(bool reserved);
    void enableEvents(EventCallback callback);
    DigitalData digital(uint64_t inputHandle, uint64_t actionHandle);
    AnalogData analog(uint64_t inputHandle, uint64_t actionHandle);
};
class SteamControllerHook {
public:
    void runFrame();
    DigitalData digital(uint64_t controller, uint64_t actionHandle);
    AnalogData analog(uint64_t controller, uint64_t actionHandle);
};

void (SteamInputHook::*g_siRunFrame)(bool) = nullptr;
void (SteamInputHook::*g_siEnableEvents)(EventCallback) = nullptr;
DigitalData (SteamInputHook::*g_siDigital)(uint64_t, uint64_t) = nullptr;
AnalogData (SteamInputHook::*g_siAnalog)(uint64_t, uint64_t) = nullptr;
void (SteamControllerHook::*g_scRunFrame)() = nullptr;
DigitalData (SteamControllerHook::*g_scDigital)(uint64_t, uint64_t) = nullptr;
AnalogData (SteamControllerHook::*g_scAnalog)(uint64_t, uint64_t) = nullptr;
static_assert(sizeof(g_siDigital) == sizeof(void*), "plain member function pointer expected");

using GetState_t = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPVOID);
using GetData_t = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
GetState_t g_diGetState = nullptr;
GetData_t g_diGetData = nullptr;

HRESULT STDMETHODCALLTYPE hookGetDeviceState(IDirectInputDevice8W* self, DWORD size, LPVOID data)
{
    int bucket = size == sizeof(DIJOYSTATE) ? 0 : size == sizeof(DIJOYSTATE2) ? 1 : size == 256 ? 2 : 3;
    ++g_diState[bucket];
    return g_diGetState(self, size, data);
}

HRESULT STDMETHODCALLTYPE hookGetDeviceData(IDirectInputDevice8W* self, DWORD size, LPDIDEVICEOBJECTDATA data,
                                            LPDWORD count, DWORD flags)
{
    ++g_diData;
    return g_diGetData(self, size, data, count, flags);
}

bool g_steamDone = false;
bool g_diDone = false;
int g_tries = 0;
ULONGLONG g_nextTry = 0;

template <typename T> void fromAddress(T& fn, void* p)
{
    memcpy(&fn, &p, sizeof(p));
}

template <typename T> void* toAddress(T fn)
{
    void* p;
    memcpy(&p, &fn, sizeof(p));
    return p;
}

bool inModule(const void* addr, const wchar_t* name)
{
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(addr), &mod))
        return false;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(mod, path, MAX_PATH);
    return wcsstr(_wcslwr(path), name) != nullptr;
}

// Replaces vtable slots (aligned 8-byte stores, so other threads see old or new) after checking
// every original lies in `module`.
bool swapSlots(void** vtbl, const int* slots, void* const* hooks, void** origs, int n, const wchar_t* module,
               const char* what)
{
    for (int i = 0; i < n; ++i) {
        if (!inModule(vtbl[slots[i]], module)) {
            LOG("%s: vtable slot %d is not in %ls, not hooking", what, slots[i], module);
            return false;
        }
    }
    int lo = slots[0], hi = slots[0];
    for (int i = 1; i < n; ++i) {
        lo = slots[i] < lo ? slots[i] : lo;
        hi = slots[i] > hi ? slots[i] : hi;
    }
    DWORD old;
    if (!VirtualProtect(&vtbl[lo], sizeof(void*) * (hi - lo + 1), PAGE_READWRITE, &old)) {
        LOG("%s: VirtualProtect on vtable failed: %lu", what, GetLastError());
        return false;
    }
    for (int i = 0; i < n; ++i)
        origs[i] = InterlockedExchangePointer(&vtbl[slots[i]], hooks[i]);
    VirtualProtect(&vtbl[lo], sizeof(void*) * (hi - lo + 1), old, &old);
    return true;
}

void** g_siVtable = nullptr;

void hookSteamInput(void* iface)
{
    void** vtbl = *static_cast<void***>(iface);
    g_siVtable = vtbl;
    const int slots[4] = {3, 8, 17, 21};
    void* hooks[4] = {toAddress(&SteamInputHook::runFrame), toAddress(&SteamInputHook::enableEvents),
                      toAddress(&SteamInputHook::digital), toAddress(&SteamInputHook::analog)};
    // Originals must be in place before the swap makes the wrappers reachable.
    fromAddress(g_siRunFrame, vtbl[3]);
    fromAddress(g_siEnableEvents, vtbl[8]);
    fromAddress(g_siDigital, vtbl[17]);
    fromAddress(g_siAnalog, vtbl[21]);
    void* origs[4];
    if (swapSlots(vtbl, slots, hooks, origs, 4, L"steamclient", "SteamInput007"))
        LOG("steam input: SteamInput007 hooked (interface %p); waiting for the game's event callback", iface);
}

void hookSteamController(void* iface)
{
    void** vtbl = *static_cast<void***>(iface);
    if (vtbl == g_siVtable) { // same table: our SteamController slots would hit SteamInput methods
        LOG("steam input: SteamController008 shares SteamInput's vtable, not hooking it");
        return;
    }
    const int slots[3] = {2, 12, 15};
    void* hooks[3] = {toAddress(&SteamControllerHook::runFrame), toAddress(&SteamControllerHook::digital),
                      toAddress(&SteamControllerHook::analog)};
    fromAddress(g_scRunFrame, vtbl[2]);
    fromAddress(g_scDigital, vtbl[12]);
    fromAddress(g_scAnalog, vtbl[15]);
    void* origs[3];
    if (swapSlots(vtbl, slots, hooks, origs, 3, L"steamclient", "SteamController008"))
        LOG("steam input: SteamController008 hooked (interface %p)", iface);
}

void hookDirectInput()
{
    g_diDone = true;
    IDirectInput8W* di = nullptr;
    if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                  reinterpret_cast<void**>(&di), nullptr)) ||
        !di) {
        LOG("dinput: DirectInput8Create failed");
        return;
    }
    // All dinput8 devices share one implementation; a keyboard device gives us its vtable.
    IDirectInputDevice8W* dev = nullptr;
    if (SUCCEEDED(di->CreateDevice(GUID_SysKeyboard, &dev, nullptr)) && dev) {
        void** vtbl = *reinterpret_cast<void***>(dev);
        g_diGetState = reinterpret_cast<GetState_t>(vtbl[9]);
        g_diGetData = reinterpret_cast<GetData_t>(vtbl[10]);
        const int slots[2] = {9, 10};
        void* hooks[2] = {reinterpret_cast<void*>(&hookGetDeviceState), reinterpret_cast<void*>(&hookGetDeviceData)};
        void* origs[2];
        if (swapSlots(vtbl, slots, hooks, origs, 2, L"dinput8", "dinput"))
            LOG("dinput: device GetDeviceState/GetDeviceData counted");
        dev->Release();
    }
    di->Release();
}

} // namespace

void SteamInputHook::runFrame(bool reserved)
{
    ++g_si.runFrame;
    // Without the game's callback we cannot rewrite events; then simply let Steam deliver none
    // while the menu is open (a stick held at that moment keeps its last value).
    if (input::menuOpen() && !g_gameCallback)
        return;
    (this->*g_siRunFrame)(reserved);
    sendNeutralOnOpen();
}

// The game registers its action-event handler here (Steam supports one). We keep its pointer and
// give Steam ours instead, which forwards every event.
void SteamInputHook::enableEvents(EventCallback callback)
{
    g_gameCallback = callback;
    LOG("steam input: game registered event callback %p, intercepting", reinterpret_cast<void*>(callback));
    (this->*g_siEnableEvents)(callback ? &onActionEvent : nullptr);
}

DigitalData SteamInputHook::digital(uint64_t inputHandle, uint64_t actionHandle)
{
    ++g_si.digital;
    DigitalData d = (this->*g_siDigital)(inputHandle, actionHandle);
    blockDigital(d);
    return d;
}

AnalogData SteamInputHook::analog(uint64_t inputHandle, uint64_t actionHandle)
{
    ++g_si.analog;
    AnalogData a = (this->*g_siAnalog)(inputHandle, actionHandle);
    blockAnalog(a);
    return a;
}

void SteamControllerHook::runFrame()
{
    ++g_sc.runFrame;
    (this->*g_scRunFrame)();
}

DigitalData SteamControllerHook::digital(uint64_t controller, uint64_t actionHandle)
{
    ++g_sc.digital;
    DigitalData d = (this->*g_scDigital)(controller, actionHandle);
    blockDigital(d);
    return d;
}

AnalogData SteamControllerHook::analog(uint64_t controller, uint64_t actionHandle)
{
    ++g_sc.analog;
    AnalogData a = (this->*g_scAnalog)(controller, actionHandle);
    blockAnalog(a);
    return a;
}

void tryInstall()
{
    if (!g_diDone)
        hookDirectInput();
    if (g_steamDone)
        return;
    ULONGLONG now = GetTickCount64();
    if (now < g_nextTry)
        return;
    g_nextTry = now + 2000;
    if (++g_tries > 60) { // two minutes without the Steam API: give up quietly
        g_steamDone = true;
        LOG("steam input: Steam API never became available");
        return;
    }

    HMODULE api = GetModuleHandleW(L"steam_api64.dll");
    if (!api)
        return;
    using GetUser_t = int(__cdecl*)();
    using FindIface_t = void*(__cdecl*)(int, const char*);
    auto getUser = reinterpret_cast<GetUser_t>(GetProcAddress(api, "SteamAPI_GetHSteamUser"));
    auto findIface = reinterpret_cast<FindIface_t>(GetProcAddress(api, "SteamInternal_FindOrCreateUserInterface"));
    if (!getUser || !findIface)
        return;
    int user = getUser();
    if (!user)
        return; // Steam API not initialised yet

    g_steamDone = true;
    if (void* si = findIface(user, "SteamInput007"))
        hookSteamInput(si);
    if (void* sc = findIface(user, "SteamController008"))
        hookSteamController(sc);
}

void logCounts()
{
    LOG("pad apis (10 s): SteamInput run %u dig %u ana %u events %u (game cb %s) | SteamController run %u dig %u "
        "ana %u | dinput state joy %u joy2 %u kbd %u other %u, data %u",
        g_si.runFrame.exchange(0), g_si.digital.exchange(0), g_si.analog.exchange(0), g_events.exchange(0),
        g_gameCallback ? "captured" : "not seen", g_sc.runFrame.exchange(0),
        g_sc.digital.exchange(0), g_sc.analog.exchange(0), g_diState[0].exchange(0), g_diState[1].exchange(0),
        g_diState[2].exchange(0), g_diState[3].exchange(0), g_diData.exchange(0));
}

} // namespace steaminput
