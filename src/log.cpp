#include "log.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

namespace logx {

static std::mutex g_mutex;
static FILE* g_file = nullptr;
static std::deque<std::string> g_ring;
static std::string g_dir;
static constexpr size_t kRingSize = 200;

void init()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file)
        return;

    char base[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base))) {
        g_dir = std::string(base) + "\\EnshroudedMenu\\";
        CreateDirectoryA(g_dir.c_str(), nullptr);
        g_file = _fsopen((g_dir + "menu.log").c_str(), "w", _SH_DENYWR);
    }
}

const std::string& dataDir()
{
    return g_dir;
}

void write(const char* fmt, ...)
{
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1100];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s", t.wHour, t.wMinute, t.wSecond,
             t.wMilliseconds, msg);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fprintf(g_file, "%s\n", line);
        fflush(g_file);
    }
    g_ring.emplace_back(line);
    if (g_ring.size() > kRingSize)
        g_ring.pop_front();
}

std::vector<std::string> tail(size_t max_lines)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    size_t start = g_ring.size() > max_lines ? g_ring.size() - max_lines : 0;
    return std::vector<std::string>(g_ring.begin() + start, g_ring.end());
}

} // namespace logx
