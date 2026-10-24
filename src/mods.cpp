#include "mods.h"

#include "log.h"

#include <windows.h>

#include <fstream>
#include <sstream>
#include <string>

namespace mods {
namespace {

bool g_loaded = false;
bool g_available = false;
bool g_enabled = false;
bool g_enabledAtStart = false;
std::string g_path;

std::string trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r");
    size_t e = s.find_last_not_of(" \t\r");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

bool isEnabledLine(const std::string& line, bool* value)
{
    std::string t = trim(line);
    if (t.rfind("enabled", 0) != 0)
        return false;
    size_t eq = t.find('=');
    if (eq == std::string::npos || trim(t.substr(0, eq)) != "enabled")
        return false;
    std::string v = trim(t.substr(eq + 1));
    v = v.substr(0, v.find_first_of(" \t#"));
    for (char& c : v)
        c = char(tolower(c));
    *value = v == "1" || v == "true" || v == "yes" || v == "on";
    return true;
}

void load()
{
    if (g_loaded)
        return;
    g_loaded = true;

    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir(exe);
    dir = dir.substr(0, dir.find_last_of('\\'));
    g_path = dir + "\\mods\\free-skills\\config.txt";

    std::ifstream in(g_path, std::ios::binary);
    if (!in) {
        LOG("skill tree mod: %s not found", g_path.c_str());
        return;
    }
    g_available = true;
    g_enabled = true; // mod default when the line is missing
    std::string line;
    while (std::getline(in, line)) {
        bool v;
        if (isEnabledLine(line, &v)) {
            g_enabled = v;
            break;
        }
    }
    g_enabledAtStart = g_enabled;
    LOG("skill tree mod: enabled=%s (%s)", g_enabled ? "true" : "false", g_path.c_str());
}

} // namespace

bool skillTreeAvailable()
{
    load();
    return g_available;
}

bool skillTreeEnabled()
{
    load();
    return g_enabled;
}

bool skillTreeEnabledAtStart()
{
    load();
    return g_enabledAtStart;
}

bool setSkillTree(bool on)
{
    load();
    if (!g_available)
        return false;

    std::ifstream in(g_path, std::ios::binary);
    std::stringstream buf;
    buf << in.rdbuf();
    in.close();
    std::string content = buf.str();
    const char* eol = content.find("\r\n") != std::string::npos ? "\r\n" : "\n";

    // Replace the first enabled= line, keep everything else byte for byte.
    std::string out;
    bool replaced = false;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t end = content.find('\n', pos);
        size_t next = end == std::string::npos ? content.size() : end + 1;
        std::string line = content.substr(pos, next - pos);
        bool v;
        if (!replaced && isEnabledLine(line, &v)) {
            out += std::string("enabled=") + (on ? "true" : "false") + eol;
            replaced = true;
        } else {
            out += line;
        }
        pos = next;
    }
    if (!replaced)
        out = std::string("enabled=") + (on ? "true" : "false") + eol + out;

    std::string tmp = g_path + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o << out;
        if (!o) {
            LOG("skill tree mod: writing %s failed", tmp.c_str());
            return g_enabled;
        }
    }
    if (!MoveFileExA(tmp.c_str(), g_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        LOG("skill tree mod: replacing config failed: %lu", GetLastError());
        DeleteFileA(tmp.c_str());
        return g_enabled;
    }
    g_enabled = on;
    LOG("skill tree mod: enabled=%s written, active after restart", on ? "true" : "false");
    return g_enabled;
}

} // namespace mods
