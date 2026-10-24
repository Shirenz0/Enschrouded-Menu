#pragma once

struct ImGuiIO;

namespace gamepad {

// Hooks XInputGetState in xinput1_4.dll (trampoline) so the game reads an idle pad while the
// menu is open. Loads the DLL if the game has not yet. Safe to call more than once.
void install();

// Once per frame: reads the first connected pad, toggles the menu on L3+R3, closes on B.
void poll();

// While the menu is open: D-pad / left stick / A / B / LB / RB as ImGui gamepad keys.
void feedImGui(ImGuiIO& io);

// True when the last input that reached the menu came from the pad (menu hides the mouse cursor).
bool active();
void mouseUsed();

} // namespace gamepad
