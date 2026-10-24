// Mod menu look and content. Right-hand glass panel, drawn mostly with the draw list so it does
// not look like stock ImGui. Tabs are built from the cheats registry (cheats.cpp); SOON rows are listed only.

#define IMGUI_DEFINE_MATH_OPERATORS
#include "menu.h"

#include "cheats.h"
#include "gamepad.h"
#include "log.h"
#include "mods.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <windows.h>

namespace menu {
namespace {

struct Fonts {
    ImFont* body = nullptr;
    ImFont* bold = nullptr;
    ImFont* small = nullptr;
    ImFont* tiny = nullptr;
    ImFont* title = nullptr;
};

const char* const kTabs[] = {"Player", "Move", "World", "Build", "Camera", "Mods"};
constexpr int kTabCount = int(sizeof(kTabs) / sizeof(kTabs[0]));

Fonts g_font;
int g_tab = 0;
float g_s = 1.0f; // UI scale
float g_anim = 0.0f;
ULONGLONG g_lastDraw = 0;

const ImVec4 kAmber(0.96f, 0.62f, 0.22f, 1.0f);
const ImVec4 kTeal(0.38f, 0.84f, 0.78f, 1.0f);
const ImVec4 kSwitchOff(0.24f, 0.26f, 0.32f, 0.95f);
constexpr ImU32 kTextOn = IM_COL32(255, 255, 255, 245);
constexpr ImU32 kTextOff = IM_COL32(215, 220, 230, 190);
constexpr ImU32 kTextDim = IM_COL32(200, 205, 215, 120);
constexpr ImU32 kHairline = IM_COL32(255, 255, 255, 26);

ImU32 col(ImVec4 c, float alpha = 1.0f)
{
    c.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}

ImFont* loadFont(const char* file, float size, ImFont* fallback)
{
    std::string path = std::string("C:\\Windows\\Fonts\\") + file;
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return fallback;
    ImFont* f = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), size);
    return f ? f : fallback;
}

// ASCII text with extra letter spacing; returns drawn width.
float spacedText(ImDrawList* dl, ImFont* f, ImVec2 pos, ImU32 c, const char* text, float spacing)
{
    float x = pos.x;
    for (const char* p = text; *p; ++p) {
        dl->AddText(f, f->FontSize, ImVec2(x, pos.y), c, p, p + 1);
        x += f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, p, p + 1).x + spacing;
    }
    return x - pos.x - spacing;
}

void keycap(ImDrawList* dl, ImVec2& cursor, const char* key)
{
    ImFont* f = g_font.small;
    ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, key);
    ImVec2 pad(5 * g_s, 2 * g_s);
    ImVec2 b = cursor + ts + pad * 2;
    dl->AddRectFilled(cursor, b, IM_COL32(255, 255, 255, 18), 4 * g_s);
    dl->AddRect(cursor, b, IM_COL32(255, 255, 255, 60), 4 * g_s);
    dl->AddText(f, f->FontSize, cursor + pad, IM_COL32(235, 238, 245, 230), key);
    cursor.x = b.x + 4 * g_s;
}

void header()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    float ww = ImGui::GetWindowWidth();
    float rounding = ImGui::GetStyle().WindowRounding;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;

    // Warm glow behind the title, fading to the right.
    float glowH = 96 * g_s;
    int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(wp + ImVec2(1, 1), wp + ImVec2(ww - 1, glowH), IM_COL32(255, 255, 255, 34), rounding,
                      ImDrawFlags_RoundCornersTop);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, wp, wp + ImVec2(ww, 0),
                                                  col(kAmber), IM_COL32(10, 12, 18, 255));

    spacedText(dl, g_font.small, p, col(kTeal, 0.9f), "ENSHROUDED", 2.2f * g_s);
    float y = p.y + g_font.small->FontSize + 2 * g_s;

    ImFont* tf = g_font.title;
    dl->AddText(tf, tf->FontSize, ImVec2(p.x, y), col(kAmber), "E-");
    float cw = tf->CalcTextSizeA(tf->FontSize, FLT_MAX, 0.0f, "E-").x;
    dl->AddText(tf, tf->FontSize, ImVec2(p.x + cw, y), kTextOn, "Menu");

    // Version pill, right aligned on the title line.
    ImFont* sf = g_font.small;
    const char* pill = "v0.5";
    ImVec2 ps = sf->CalcTextSizeA(sf->FontSize, FLT_MAX, 0.0f, pill);
    ImVec2 pad(7 * g_s, 3 * g_s);
    ImVec2 pa(p.x + w - ps.x - pad.x * 2, y + (tf->FontSize - ps.y) * 0.5f - pad.y);
    dl->AddRect(pa, pa + ps + pad * 2, col(kTeal, 0.55f), 20 * g_s, 0, 1.2f * g_s);
    dl->AddText(sf, sf->FontSize, pa + pad, col(kTeal, 0.95f), pill);

    y += tf->FontSize + 2 * g_s;
    char sub[64];
    int n = cheats::activeCount();
    snprintf(sub, sizeof(sub), n == 1 ? "%d cheat active" : "%d cheats active", n);
    dl->AddText(sf, sf->FontSize, ImVec2(p.x, y), n ? col(kAmber, 0.85f) : kTextDim, sub);
    y += sf->FontSize + 10 * g_s;

    // Accent line amber -> teal -> transparent.
    float lh = 2 * g_s;
    float mid = p.x + w * 0.55f;
    dl->AddRectFilledMultiColor(ImVec2(p.x, y), ImVec2(mid, y + lh), col(kAmber), col(kTeal), col(kTeal), col(kAmber));
    dl->AddRectFilledMultiColor(ImVec2(mid, y), ImVec2(p.x + w, y + lh), col(kTeal), col(kTeal, 0.0f),
                                col(kTeal, 0.0f), col(kTeal));
    y += lh + 6 * g_s;

    ImGui::Dummy(ImVec2(w, y - p.y));
}

void section(const char* name)
{
    ImGui::Dummy(ImVec2(0, 8 * g_s));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImFont* f = g_font.small;
    float tw = spacedText(dl, f, p, col(kTeal, 0.85f), name, 1.8f * g_s);
    float ly = p.y + f->FontSize * 0.55f;
    dl->AddLine(ImVec2(p.x + tw + 10 * g_s, ly), ImVec2(p.x + w, ly), kHairline, 1.0f);
    ImGui::Dummy(ImVec2(w, f->FontSize + 4 * g_s));
}

// Eases *t toward target; the value lives in ImGui's per-window storage under the item id.
float animate(ImGuiID id, float target, float speed = 16.0f)
{
    float* t = ImGui::GetStateStorage()->GetFloatRef(id, target);
    *t += (target - *t) * ImMin(1.0f, ImGui::GetIO().DeltaTime * speed);
    return *t;
}

void rowBackground(ImDrawList* dl, ImVec2 a, ImVec2 b)
{
    if (ImGui::IsItemHovered())
        dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, ImGui::IsItemActive() ? 28 : 16), 6 * g_s);
}

// Controller navigation over the focusable rows of the current tab. ImGui's built-in gamepad nav
// did not move between these custom-drawn rows, so the menu keeps its own focus index.
struct PadNav {
    int focus = 0;         // index among focusable items in the current tab
    int counter = 0;       // items registered this frame
    int lastCount = 0;     // items registered last frame
    bool activate = false; // A pressed this frame
    int step = 0;          // -1 / +1 from left / right this frame
    float stepMul = 1.0f;  // larger steps while left / right is held
};
PadNav g_nav;

bool padPressed(ImGuiKey dpad, ImGuiKey stick)
{
    return ImGui::IsKeyPressed(dpad, true) || ImGui::IsKeyPressed(stick, true);
}

void navBeginFrame()
{
    g_nav.lastCount = g_nav.counter;
    g_nav.counter = 0;
    if (g_nav.lastCount > 0) {
        if (padPressed(ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown))
            g_nav.focus = (g_nav.focus + 1) % g_nav.lastCount;
        if (padPressed(ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp))
            g_nav.focus = (g_nav.focus + g_nav.lastCount - 1) % g_nav.lastCount;
        g_nav.focus = ImMin(g_nav.focus, g_nav.lastCount - 1);
    }
    g_nav.activate = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
    g_nav.step = int(padPressed(ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight)) -
                 int(padPressed(ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft));
    float held = ImMax(ImMax(ImGui::GetKeyData(ImGuiKey_GamepadDpadRight)->DownDuration,
                             ImGui::GetKeyData(ImGuiKey_GamepadDpadLeft)->DownDuration),
                       ImMax(ImGui::GetKeyData(ImGuiKey_GamepadLStickRight)->DownDuration,
                             ImGui::GetKeyData(ImGuiKey_GamepadLStickLeft)->DownDuration));
    g_nav.stepMul = held > 2.0f ? 25.0f : (held > 0.8f ? 5.0f : 1.0f);
}

// Registers a focusable item spanning a..b; true when it has controller focus (outlined in amber).
bool navItem(ImVec2 a, ImVec2 b)
{
    bool focused = g_nav.counter++ == g_nav.focus && gamepad::active();
    if (focused)
        ImGui::GetWindowDrawList()->AddRect(a, b, col(kAmber, 0.9f), 6 * g_s, 0, 1.5f * g_s);
    return focused;
}

// Small rounded tag at the right end of a disabled row ("SOON", "N/A").
void rowTag(ImDrawList* dl, ImVec2 rowMin, float rowW, float rowH, const char* tag)
{
    ImFont* f = g_font.small;
    ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, tag);
    ImVec2 pad(6 * g_s, 2 * g_s);
    ImVec2 a(rowMin.x + rowW - ts.x - pad.x * 2 - 8 * g_s, rowMin.y + (rowH - ts.y) * 0.5f - pad.y);
    dl->AddRect(a, a + ts + pad * 2, IM_COL32(255, 255, 255, 50), 20 * g_s);
    dl->AddText(f, f->FontSize, a + pad, kTextDim, tag);
}

// Returns true when an enabled row is clicked; the caller decides the new state.
// Disabled rows are dimmed, show `tag` instead of the switch and ignore clicks.
bool toggleRow(const char* label, bool value, bool enabled = true, const char* tag = "SOON")
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float h = 34 * g_s;

    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h));
    ImGui::EndDisabled();
    float t = animate(ImGui::GetItemID(), value ? 1.0f : 0.0f);
    ImFont* f = g_font.body;
    ImVec2 textPos(p.x + 12 * g_s, p.y + (h - f->FontSize) * 0.5f);

    if (!enabled) {
        dl->AddText(f, f->FontSize, textPos, kTextDim, label);
        rowTag(dl, p, w, h, tag);
        return false;
    }

    rowBackground(dl, p, p + ImVec2(w, h));
    if (navItem(p, p + ImVec2(w, h)) && g_nav.activate)
        clicked = true;
    if (t > 0.01f)
        dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.28f), ImVec2(p.x + 3 * g_s, p.y + h * 0.72f), col(kAmber, t),
                          2 * g_s);
    dl->AddText(f, f->FontSize, textPos, value ? kTextOn : kTextOff, label);

    float sh = 18 * g_s, sw = 34 * g_s;
    ImVec2 a(p.x + w - sw - 8 * g_s, p.y + (h - sh) * 0.5f);
    dl->AddRectFilled(a, a + ImVec2(sw, sh), col(ImLerp(kSwitchOff, kAmber, t)), sh * 0.5f);
    float r = sh * 0.5f - 2.5f * g_s;
    dl->AddCircleFilled(ImVec2(a.x + sh * 0.5f + (sw - sh) * t, a.y + sh * 0.5f), r, IM_COL32(255, 255, 255, 245));
    return clicked;
}

// Returns true while the value changes. `active` lights the label and value up in amber.
bool sliderRow(const char* label, float* value, float lo, float hi, float step, const char* fmt, bool active,
               bool enabled = true)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float h = 48 * g_s;
    float x0 = p.x + 12 * g_s, x1 = p.x + w - 8 * g_s;

    if (!enabled) {
        ImGui::BeginDisabled();
        ImGui::InvisibleButton(label, ImVec2(w, 34 * g_s));
        ImGui::EndDisabled();
        ImFont* f = g_font.body;
        dl->AddText(f, f->FontSize, ImVec2(x0, p.y + (34 * g_s - f->FontSize) * 0.5f), kTextDim, label);
        rowTag(dl, p, w, 34 * g_s, "N/A");
        return false;
    }

    ImGui::InvisibleButton(label, ImVec2(w, h));
    bool changed = false;
    if (navItem(p, p + ImVec2(w, h)) && g_nav.step) { // controller: left/right, faster while held
        float v = ImClamp(*value + g_nav.step * step * g_nav.stepMul, lo, hi);
        changed = v != *value;
        *value = v;
    }
    if (ImGui::IsItemActive()) {
        float f = ImClamp((ImGui::GetIO().MousePos.x - x0) / (x1 - x0), 0.0f, 1.0f);
        float v = ImClamp(lo + roundf(f * (hi - lo) / step) * step, lo, hi);
        changed = v != *value;
        *value = v;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        LOG("%s: %g", label, *value);
    bool hovered = ImGui::IsItemHovered() || ImGui::IsItemActive();
    rowBackground(dl, p, p + ImVec2(w, h));

    float on = active ? 1.0f : 0.0f;
    ImFont* f = g_font.body;
    float ty = p.y + 6 * g_s;
    // Labels may carry an ImGui "##id" suffix; draw only the visible part.
    dl->AddText(f, f->FontSize, ImVec2(x0, ty), on ? kTextOn : kTextOff, label, ImGui::FindRenderedTextEnd(label));
    char buf[32];
    snprintf(buf, sizeof(buf), fmt, *value);
    ImFont* bf = g_font.bold;
    float vw = bf->CalcTextSizeA(bf->FontSize, FLT_MAX, 0.0f, buf).x;
    dl->AddText(bf, bf->FontSize, ImVec2(x1 - vw, ty), on ? col(kAmber) : kTextOff, buf);

    float frac = (*value - lo) / (hi - lo);
    float ty2 = p.y + h - 12 * g_s;
    float th = 4 * g_s;
    float kx = x0 + (x1 - x0) * frac;
    dl->AddRectFilled(ImVec2(x0, ty2), ImVec2(x1, ty2 + th), col(kSwitchOff), th);
    dl->AddRectFilledMultiColor(ImVec2(x0, ty2), ImVec2(kx, ty2 + th), col(kAmber, 0.6f), col(kAmber), col(kAmber),
                                col(kAmber, 0.6f));
    float kr = animate(ImGui::GetItemID(), hovered ? 8.0f : 6.5f) * g_s;
    dl->AddCircleFilled(ImVec2(kx, ty2 + th * 0.5f), kr, IM_COL32(255, 255, 255, 250));
    if (hovered)
        dl->AddCircle(ImVec2(kx, ty2 + th * 0.5f), kr + 2 * g_s, col(kAmber, 0.6f), 0, 1.5f * g_s);
    return changed;
}

// One registry entry as a toggle, slider, or toggle with a value slider underneath.
void cheatRow(int i)
{
    const cheats::Info& ci = cheats::info(i);
    bool avail = cheats::available(i);
    const char* tag = ci.soon ? "SOON" : "N/A";
    float v = cheats::value(i);

    switch (ci.kind) {
    case cheats::Kind::Toggle:
        if (toggleRow(ci.label, cheats::enabled(i), avail, tag))
            cheats::setEnabled(i, !cheats::enabled(i));
        break;
    case cheats::Kind::Slider:
        if (sliderRow(ci.label, &v, ci.min, ci.max, ci.step, ci.fmt, cheats::enabled(i), avail))
            cheats::setValue(i, v);
        break;
    case cheats::Kind::ToggleSlider:
        if (toggleRow(ci.label, cheats::enabled(i), avail, tag))
            cheats::setEnabled(i, !cheats::enabled(i));
        if (avail && cheats::enabled(i)) {
            char id[96];
            snprintf(id, sizeof(id), "   Amount##%s", ci.label);
            if (sliderRow(id, &v, ci.min, ci.max, ci.step, ci.fmt, true))
                cheats::setValue(i, v);
        }
        break;
    }
}

// Pill tabs under the header; the active one is amber with an underline.
void tabBar()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImFont* f = g_font.small;
    float h = f->FontSize + 12 * g_s;
    float tw = w / kTabCount;

    int before = g_tab;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false))
        g_tab = (g_tab + kTabCount - 1) % kTabCount;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false))
        g_tab = (g_tab + 1) % kTabCount;

    for (int t = 0; t < kTabCount; ++t) {
        ImVec2 a(p.x + t * tw, p.y);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(t);
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        if (ImGui::InvisibleButton("##tab", ImVec2(tw, h)))
            g_tab = t;
        ImGui::PopItemFlag();
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();

        float sel = animate(ImGui::GetID(kTabs[t]), g_tab == t ? 1.0f : 0.0f);
        if (hov && g_tab != t)
            dl->AddRectFilled(a, a + ImVec2(tw, h), IM_COL32(255, 255, 255, 14), 6 * g_s);
        ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, kTabs[t]);
        ImU32 tc = g_tab == t ? col(kAmber) : (hov ? kTextOn : kTextOff);
        dl->AddText(f, f->FontSize, ImVec2(a.x + (tw - ts.x) * 0.5f, a.y + (h - ts.y) * 0.5f - 1 * g_s), tc, kTabs[t]);
        if (sel > 0.01f) {
            float uw = ts.x * sel;
            float cx = a.x + tw * 0.5f;
            dl->AddRectFilled(ImVec2(cx - uw * 0.5f, a.y + h - 2 * g_s), ImVec2(cx + uw * 0.5f, a.y + h),
                              col(kAmber, sel), 1 * g_s);
        }
    }
    dl->AddLine(ImVec2(p.x, p.y + h), ImVec2(p.x + w, p.y + h), kHairline, 1.0f);
    if (g_tab != before)
        g_nav.focus = 0;
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, h + 2 * g_s));
}

void skillTreeToggle();

// Compact rounded button; amber outline on hover, dimmed when disabled.
bool pillButton(const char* label, float width, bool enabled = true)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImFont* f = g_font.small;
    float h = f->FontSize + 12 * g_s;
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::InvisibleButton(label, ImVec2(width, h));
    ImGui::EndDisabled();
    if (enabled && navItem(p, p + ImVec2(width, h)) && g_nav.activate)
        clicked = true;
    bool hov = enabled && ImGui::IsItemHovered();
    bool held = enabled && ImGui::IsItemActive();
    ImU32 fill = held ? col(kAmber, 0.35f) : (hov ? col(kAmber, 0.18f) : IM_COL32(255, 255, 255, 14));
    dl->AddRectFilled(p, p + ImVec2(width, h), fill, h * 0.5f);
    dl->AddRect(p, p + ImVec2(width, h), hov ? col(kAmber, 0.8f) : IM_COL32(255, 255, 255, 40), h * 0.5f);
    const char* end = ImGui::FindRenderedTextEnd(label);
    ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, label, end);
    dl->AddText(f, f->FontSize, p + ImVec2((width - ts.x) * 0.5f, (h - ts.y) * 0.5f), enabled ? kTextOn : kTextDim,
                label, end);
    return enabled && clicked;
}

// Position read-out plus save / teleport / up / down.
void teleportPanel()
{
    section("TELEPORT");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImFont* f = g_font.body;
    int pos[3];
    bool have = cheats::position(pos);
    char buf[96];
    if (have)
        snprintf(buf, sizeof(buf), "X %d   Y %d   Z %d", pos[0], pos[1], pos[2]);
    else
        snprintf(buf, sizeof(buf), "Waiting for player position...");
    dl->AddText(f, f->FontSize, ImVec2(p.x + 12 * g_s, p.y + 4 * g_s), have ? kTextOn : kTextDim, buf);
    ImGui::Dummy(ImVec2(w, f->FontSize + 12 * g_s));

    float gap = 6 * g_s;
    float bw = (w - 3 * gap) / 4.0f;
    ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos() + ImVec2(0, 0));
    if (pillButton("Save##tp", bw, have))
        cheats::savePosition();
    ImGui::SameLine(0, gap);
    if (pillButton("Teleport##tp", bw, have && cheats::hasSavedPosition()))
        cheats::teleportToSaved();
    ImGui::SameLine(0, gap);
    if (pillButton("Up 5##tp", bw, have))
        cheats::nudgeUp(5);
    ImGui::SameLine(0, gap);
    if (pillButton("Down 5##tp", bw, have))
        cheats::nudgeUp(-5);
    ImGui::Dummy(ImVec2(0, 2 * g_s));
}

// All registry rows of the current tab, with a section heading whenever it changes.
void tabContent()
{
    if (g_tab == kTabCount - 1) { // Mods
        section("MODS");
        skillTreeToggle();
        return;
    }
    const char* lastSection = nullptr;
    for (int i = 0; i < cheats::count(); ++i) {
        const cheats::Info& ci = cheats::info(i);
        if (strcmp(ci.tab, kTabs[g_tab]) != 0)
            continue;
        if (!lastSection || strcmp(lastSection, ci.section) != 0) {
            section(ci.section);
            lastSection = ci.section;
        }
        cheatRow(i);
    }
    if (strcmp(kTabs[g_tab], "Move") == 0)
        teleportPanel();
}

// Very small note tucked under the previous row; amber when `highlight`.
void rowHint(const char* text, bool highlight)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImFont* f = g_font.tiny;
    float lift = 7 * g_s; // sit close to the label above
    dl->AddText(f, f->FontSize, ImVec2(p.x + 12 * g_s, p.y - lift), highlight ? col(kAmber, 0.9f) : kTextDim, text);
    ImGui::Dummy(ImVec2(0, f->FontSize - lift + 4 * g_s));
}

void skillTreeToggle()
{
    bool on = mods::skillTreeEnabled();
    if (toggleRow("Skill Tree Mod (Beta)", on, mods::skillTreeAvailable(), "N/A"))
        on = mods::setSkillTree(!on);
    if (mods::skillTreeAvailable())
        rowHint("Restart game to apply", on != mods::skillTreeEnabledAtStart());
}

void footer()
{
    ImGui::Dummy(ImVec2(0, 10 * g_s));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    dl->AddLine(p, ImVec2(p.x + w, p.y), kHairline, 1.0f);

    ImFont* f = g_font.small;
    ImVec2 c(p.x, p.y + 9 * g_s);
    keycap(dl, c, "F2");
    keycap(dl, c, "L3+R3");
    dl->AddText(f, f->FontSize, ImVec2(c.x + 2 * g_s, c.y + 2 * g_s), kTextDim, "close");

    char fps[32];
    snprintf(fps, sizeof(fps), "%.0f FPS", ImGui::GetIO().Framerate);
    float fw = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, fps).x;
    dl->AddText(f, f->FontSize, ImVec2(p.x + w - fw, c.y + 2 * g_s), kTextDim, fps);
    ImGui::Dummy(ImVec2(w, 9 * g_s + f->FontSize + 6 * g_s + 2 * g_s));
}

void drawToast(float rightEdge, float top)
{
    ImGui::SetNextWindowPos(ImVec2(rightEdge, top), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * g_s, 9 * g_s));
    if (ImGui::Begin("##toast", nullptr, flags)) {
        ImGui::PushFont(g_font.bold);
        ImGui::TextColored(kAmber, "E-");
        ImGui::SameLine(0, 0);
        ImGui::TextUnformatted("Menu");
        ImGui::PopFont();
        ImGui::SameLine(0, 14 * g_s);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 c = ImGui::GetCursorScreenPos();
        c.y += (g_font.bold->FontSize - g_font.small->FontSize) * 0.5f - 2 * g_s;
        keycap(dl, c, "F2");
        keycap(dl, c, "L3+R3");
        ImGui::Dummy(ImVec2(c.x - ImGui::GetCursorScreenPos().x, g_font.bold->FontSize));
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void drawMenu()
{
    cheats::init(); // pattern scan + caves, once

    // Fade + slide in whenever the menu (re)opens.
    ULONGLONG now = GetTickCount64();
    if (now - g_lastDraw > 150) {
        g_anim = 0.0f;
        ImGui::SetNextWindowFocus(); // so the pad navigates the menu right away
    }
    g_lastDraw = now;
    g_anim += (1.0f - g_anim) * ImMin(1.0f, ImGui::GetIO().DeltaTime * 12.0f);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float width = 380 * g_s;
    float margin = 28 * g_s;
    float slide = (1.0f - g_anim) * 40 * g_s;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - margin + slide, vp->WorkPos.y + vp->WorkSize.y * 0.12f),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, FLT_MAX));

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_anim);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##cheatmenu", nullptr, flags)) {
        navBeginFrame();
        header();
        tabBar();
        tabContent();
        footer();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace

void initStyle(float scale)
{
    g_s = scale;
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.SizePixels = 13.0f * scale;
    ImFont* fallback = io.Fonts->AddFontDefault(&cfg);
    g_font.body = loadFont("segoeui.ttf", 17.0f * scale, fallback);
    g_font.bold = loadFont("seguisb.ttf", 17.0f * scale, g_font.body);
    g_font.small = loadFont("segoeuib.ttf", 12.5f * scale, g_font.body);
    g_font.tiny = loadFont("segoeui.ttf", 11.0f * scale, g_font.small);
    g_font.title = loadFont("seguibl.ttf", 34.0f * scale, g_font.bold);
    io.FontDefault = g_font.body;

    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 12 * scale;
    st.WindowBorderSize = 1.0f;
    st.WindowPadding = ImVec2(18 * scale, 16 * scale);
    st.ItemSpacing = ImVec2(8 * scale, 2 * scale);
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.035f, 0.04f, 0.06f, 0.62f);
    st.Colors[ImGuiCol_Border] = ImVec4(kAmber.x, kAmber.y, kAmber.z, 0.22f);
    st.Colors[ImGuiCol_Text] = ImVec4(0.92f, 0.93f, 0.96f, 1.0f);
    st.Colors[ImGuiCol_NavCursor] = ImVec4(kAmber.x, kAmber.y, kAmber.z, 0.9f);
    st.FrameRounding = 6 * scale; // also rounds the controller focus outline
}

void draw(const FrameInfo& fi)
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (fi.watermark && !fi.open)
        drawToast(vp->WorkPos.x + vp->WorkSize.x - 28 * g_s, vp->WorkPos.y + vp->WorkSize.y * 0.12f);
    if (fi.open)
        drawMenu();
}

} // namespace menu
