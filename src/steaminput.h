#pragma once

#include <cstdint>

namespace steaminput {

// Hooks the Steam Input / Steam Controller action getters (released buttons and centred sticks
// while the menu is open) and counts DirectInput device reads. Retries until the Steam API is up.
// Call once per frame.
void tryInstall();

// Logs how often the game called each pad API since the last call (diagnostics).
void logCounts();

} // namespace steaminput
