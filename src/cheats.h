#pragma once

namespace cheats {

enum class Kind {
    Toggle,       // on/off
    Slider,       // value; equal to `def` means off
    ToggleSlider, // on/off plus a value used while on
};

struct Info {
    const char* tab;     // menu tab ("Player", "Move", "World", "Build", "Camera")
    const char* section; // heading inside the tab
    const char* label;
    Kind kind = Kind::Toggle;
    float min = 0, max = 0, def = 0; // sliders
    float step = 0.1f;
    const char* fmt = "%.1f";
    bool soon = false; // listed but not implemented yet
};

// Scans all patterns and prepares the code caves. Nothing is patched until a cheat is enabled.
void init();

int count();
const Info& info(int i);

// False when a pattern was not found or the bytes at a site differ (game updated).
bool available(int i);
bool enabled(int i);
float value(int i);

// Toggles and ToggleSliders. Returns the state the cheat actually ended up in.
bool setEnabled(int i, bool on);
// Sliders and ToggleSliders.
void setValue(int i, float v);

int activeCount();

// Player position from the movement component (integer part of the 32.32 fixed-point x/y/z).
// False until the game has run the capture code for the player.
bool position(int pos[3]);
bool hasSavedPosition();
void savePosition();
bool teleportToSaved();
void nudgeUp(int units);

// Logs probe counters/values of active hooks every 10 s. Call once per frame.
void debugTick();

} // namespace cheats
