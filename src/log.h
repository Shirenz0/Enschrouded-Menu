#pragma once

#include <string>
#include <vector>

namespace logx {

// Opens %LOCALAPPDATA%\EnshroudedMenu\menu.log (truncated per process start).
void init();
void write(const char* fmt, ...);

// Last lines for the in-game log panel (thread safe copy).
std::vector<std::string> tail(size_t max_lines);

// Directory that holds menu.log and imgui.ini (with trailing backslash).
const std::string& dataDir();

} // namespace logx

#define LOG(...) ::logx::write(__VA_ARGS__)
