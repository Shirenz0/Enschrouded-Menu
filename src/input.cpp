// Hotkey, mouse feed for ImGui, and keeping the game blind to input while the menu is open.
//
// Enshrouded reads mouse/keyboard through raw input (GetRawInputData) and warps/clips the cursor.
// While the menu is open we neutralise raw input in place (key and button releases still pass,
// so nothing stays "held" after closing), swallow legacy key/mouse messages, and ignore the
// game's SetCursorPos/ClipCursor calls.

#include "input.h"

#include "gamepad.h"
#include "log.h"

#include "imgui.h"

#include <atomic>
#include <cstring>

namespace input {
namespace {

std::atomic<bool> g_open{false};
std::atomic<int> g_wheel{0};
std::atomic<HWND> g_hwnd{nullptr};
WNDPROC g_origWndProc = nullptr;
bool g_f2WasDown = false;

using GetRawInputData_t = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using SetCursorPos_t = BOOL(WINAPI*)(int, int);
using ClipCursor_t = BOOL(WINAPI*)(const RECT*);

GetRawInputData_t g_realGetRawInputData = &::GetRawInputData;
SetCursorPos_t g_realSetCursorPos = &::SetCursorPos;
ClipCursor_t g_realClipCursor = &::ClipCursor;

constexpr USHORT kMouseReleaseFlags = RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_UP |
                                      RI_MOUSE_MIDDLE_BUTTON_UP | RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP;

std::atomic<uint32_t> g_hidReads{0}; // raw input HID reports the game fetched (gamepads, diagnostics)

UINT WINAPI hookGetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT headerSize)
{
    UINT res = g_realGetRawInputData(h, cmd, data, size, headerSize);
    if (cmd != RID_INPUT || !data || res == UINT(-1) || res < sizeof(RAWINPUTHEADER))
        return res;
    if (static_cast<RAWINPUT*>(data)->header.dwType == RIM_TYPEHID) {
        ++g_hidReads;
        if (g_open)
            return UINT(-1); // controller report: the game sees a failed read and skips it
        return res;
    }
    if (!g_open)
        return res;

    auto* ri = static_cast<RAWINPUT*>(data);
    if (ri->header.dwType == RIM_TYPEMOUSE && res >= sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE)) {
        RAWMOUSE& m = ri->data.mouse;
        m.lLastX = 0;
        m.lLastY = 0;
        m.usButtonFlags &= kMouseReleaseFlags;
        m.usButtonData = 0;
    } else if (ri->header.dwType == RIM_TYPEKEYBOARD && res >= sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD)) {
        RAWKEYBOARD& k = ri->data.keyboard;
        if (!(k.Flags & RI_KEY_BREAK)) {
            k.VKey = 0xFF; // "no key"
            k.MakeCode = 0;
            k.Flags = RI_KEY_BREAK;
            k.Message = WM_KEYUP;
        }
    }
    return res;
}

BOOL WINAPI hookSetCursorPos(int x, int y)
{
    if (g_open)
        return TRUE;
    return g_realSetCursorPos(x, y);
}

BOOL WINAPI hookClipCursor(const RECT* rect)
{
    if (g_open)
        return g_realClipCursor(nullptr);
    return g_realClipCursor(rect);
}

// Replaces one import of `module` in place; returns the previous pointer or nullptr.
void* patchImport(HMODULE module, const char* dll, const char* function, void* hook)
{
    auto* base = reinterpret_cast<BYTE*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return nullptr;

    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc) {
        if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), dll) != 0)
            continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(byName->Name, function) != 0)
                continue;
            DWORD old;
            VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            void* prev = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            return prev;
        }
    }
    return nullptr;
}

template <typename T> void hookImport(const char* function, T hook, T& real)
{
    void* prev = patchImport(GetModuleHandleW(nullptr), "user32.dll", function, reinterpret_cast<void*>(hook));
    if (prev && prev != reinterpret_cast<void*>(hook))
        real = reinterpret_cast<T>(prev);
    LOG("IAT hook %s: %s", function, prev ? "ok" : "not imported");
}

bool isKeyOrMouseInput(UINT msg)
{
    switch (msg) {
    case WM_KEYDOWN:
    case WM_CHAR:
    case WM_DEADCHAR:
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK:
    case WM_MOUSEHWHEEL:
        return true;
    default:
        return false;
    }
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_open) {
        if (msg == WM_MOUSEWHEEL) {
            g_wheel += GET_WHEEL_DELTA_WPARAM(wp);
            return 0;
        }
        if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
            SetCursor(nullptr); // ImGui draws its own cursor
            return TRUE;
        }
        // Key-up / button-up still reach the game so nothing stays pressed.
        if (isKeyOrMouseInput(msg))
            return 0;
    }
    return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
}

void setOpen(bool open)
{
    g_open = open;
    if (open)
        g_realClipCursor(nullptr);
    LOG("menu %s", open ? "opened" : "closed");
}

} // namespace

void installHooks()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    hookImport("GetRawInputData", &hookGetRawInputData, g_realGetRawInputData);
    hookImport("SetCursorPos", &hookSetCursorPos, g_realSetCursorPos);
    hookImport("ClipCursor", &hookClipCursor, g_realClipCursor);
}

void attachWindow(HWND hwnd)
{
    if (!hwnd || g_hwnd.load() == hwnd)
        return;
    if (g_hwnd.load()) {
        LOG("second surface window %p ignored, staying on %p", hwnd, g_hwnd.load());
        return;
    }
    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&wndProc)));
    if (!g_origWndProc) {
        LOG("subclassing window %p failed: %lu", hwnd, GetLastError());
        return;
    }
    g_hwnd = hwnd;
    LOG("attached to game window %p", hwnd);
}

void pollHotkey()
{
    HWND hwnd = g_hwnd.load();
    bool focused = !hwnd || GetForegroundWindow() == hwnd;
    bool f2Down = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    if (focused && f2Down && !g_f2WasDown)
        setOpen(!g_open);
    g_f2WasDown = f2Down;
}

uint32_t takeHidReads()
{
    return g_hidReads.exchange(0);
}

bool menuOpen()
{
    return g_open;
}

void toggleMenu()
{
    setOpen(!g_open);
}

void feedImGui(ImGuiIO& io, const VkExtent2D& extent)
{
    io.MouseDrawCursor = g_open && !gamepad::active();
    if (!g_open)
        return;

    HWND hwnd = g_hwnd.load();
    POINT p;
    if (hwnd && GetCursorPos(&p) && ScreenToClient(hwnd, &p)) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        float sx = rc.right > 0 ? float(extent.width) / float(rc.right) : 1.0f;
        float sy = rc.bottom > 0 ? float(extent.height) / float(rc.bottom) : 1.0f;
        static POINT last = {};
        if (p.x != last.x || p.y != last.y)
            gamepad::mouseUsed();
        last = p;
        io.AddMousePosEvent(p.x * sx, p.y * sy);
    }
    io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(2, (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
    if (int wheel = g_wheel.exchange(0))
        io.AddMouseWheelEvent(0.0f, wheel / float(WHEEL_DELTA));
}

} // namespace input
