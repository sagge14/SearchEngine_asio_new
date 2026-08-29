#include "ZagEditorSettings.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace zag_editor {
namespace {

static inline std::string trim(std::string s) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

static inline std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static inline bool parseBool(const std::string& v, bool* out) {
    std::string s = toLower(trim(v));
    if (s == "1" || s == "true" || s == "yes" || s == "y" || s == "on") { *out = true; return true; }
    if (s == "0" || s == "false" || s == "no" || s == "n" || s == "off") { *out = false; return true; }
    return false;
}

static inline std::vector<std::filesystem::path> splitPaths(const std::string& v) {
    std::vector<std::filesystem::path> out;
    std::string s = v;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t next = s.find(';', pos);
        std::string part = (next == std::string::npos) ? s.substr(pos) : s.substr(pos, next - pos);
        part = trim(part);
        if (!part.empty()) out.emplace_back(std::filesystem::path(part));
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return out;
}

} // namespace

LoadedSettings loadSettingsIni(const std::filesystem::path& ini_path) {
    LoadedSettings res;

    std::ifstream in(ini_path);
    if (!in.is_open()) {
        res.loaded_from_file = false;
        return res;
    }
    res.loaded_from_file = true;

    std::string line;
    while (std::getline(in, line)) {
        std::string s = trim(line);
        if (s.empty()) continue;
        if (s[0] == '#' || s[0] == ';') continue;

        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;

        std::string key = toLower(trim(s.substr(0, eq)));
        std::string val = trim(s.substr(eq + 1));

        if (key == "dict_path") {
            if (!val.empty()) res.settings.dict_path = std::filesystem::path(val);
        } else if (key == "recursive") {
            bool b{};
            if (parseBool(val, &b)) res.settings.recursive = b;
        } else if (key == "backup") {
            bool b{};
            if (parseBool(val, &b)) res.settings.backup = b;
        } else if (key == "input_dirs") {
            auto v = splitPaths(val);
            res.settings.input_dirs.insert(res.settings.input_dirs.end(), v.begin(), v.end());
        }
    }

    return res;
}

} // namespace zag_editor

