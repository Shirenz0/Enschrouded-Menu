#pragma once

namespace mods {

// EML "Free Skills & Upgrades" (mods\free-skills\config.txt next to enshrouded.exe).
// EML applies it at game start, so a change here only takes effect after a restart.
bool skillTreeAvailable();

// Current value of `enabled=` in config.txt.
bool skillTreeEnabled();

// Value of `enabled=` when the game started, i.e. what is active right now.
bool skillTreeEnabledAtStart();

// Rewrites the `enabled=` line; returns the value now in the file.
bool setSkillTree(bool on);

} // namespace mods
