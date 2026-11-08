#include "ZagKpodiChanger.h"

#include <fstream>
#include <vector>
#include <algorithm>
#include <cctype>
#include <system_error>
#include <stdexcept>

namespace zag_editor {

ZagKpodiChanger::ZagKpodiChanger(std::filesystem::path dict_path)
    : dict_path_(std::move(dict_path))
{
    loadMapOrThrow(dict_path_);
}

bool ZagKpodiChanger::starts_with(const std::string& s, const char* pref) {
    const size_t n = std::char_traits<char>::length(pref);
    return s.size() >= n && std::equal(pref, pref + n, s.begin());
}

std::string ZagKpodiChanger::trim(const std::string& s) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    size_t i = 0, j = s.size();
    while (i < j && is_space(static_cast<unsigned char>(s[i]))) ++i;
    while (j > i && is_space(static_cast<unsigned char>(s[j - 1]))) --j;
    return s.substr(i, j - i);
}

void ZagKpodiChanger::stripTrailingCr(std::string& s) {
    if (!s.empty() && s.back() == '\r') s.pop_back();
}

std::string ZagKpodiChanger::parse_s_key(const std::string& line) {
    if (!starts_with(line, "s=")) return {};
    size_t pos = 2; // after "s="
    size_t end = pos;
    while (end < line.size()) {
        unsigned char ch = static_cast<unsigned char>(line[end]);
        if (ch == '/' || ch == ' ' || ch == '\t') break;
        ++end;
    }
    return trim(line.substr(pos, end - pos));
}

void ZagKpodiChanger::loadMapOrThrow(const std::filesystem::path& dict_path) {
    std::ifstream in(dict_path);
    if (!in.is_open()) {
        throw std::runtime_error("ZagEditor: can't open dict file (EXPORT.INI).");
    }

    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line); ) {
        stripTrailingCr(line);
        lines.push_back(std::move(line));
    }

    size_t inserted = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& cur = lines[i];
        if (!starts_with(cur, "s=")) continue;

        std::string value = parse_s_key(cur);
        if (value.empty()) continue;
        if (i == 0) continue;

        const std::string& prev = lines[i - 1];
        if (starts_with(prev, "s=")) continue;

        std::string key = trim(prev);
        if (key.empty()) continue;

        auto [it, ok] = mapKpodi_.emplace(std::move(key), std::move(value));
        if (ok) ++inserted;
    }

    if (inserted == 0) {
        throw std::runtime_error("ZagEditor: dict loaded, but no entries parsed.");
    }
}

bool ZagKpodiChanger::processFile(const std::filesystem::path& zag_path) const {
    return processFile(zag_path, nullptr);
}

bool ZagKpodiChanger::processFile(const std::filesystem::path& zag_path, std::ostream* verbose_log) const {
    std::ifstream in(zag_path, std::ios::binary);
    if (!in.is_open()) return false;

    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line); ) {
        stripTrailingCr(line);
        lines.push_back(std::move(line));
    }
    in.close(); // важно для Windows: иначе remove/rename может не пройти из-за открытого handle

    bool changed = false;
    for (auto& line : lines) {
        if (starts_with(line, "From=")) {
            std::string key = trim(line.substr(5));
            auto it = mapKpodi_.find(key);
            if (it != mapKpodi_.end()) {
                line = std::string("From=") + it->second;
                changed = true;
                if (verbose_log) {
                    (*verbose_log) << "[ZagEditor] From key matched: '" << key
                                   << "' -> '" << it->second << "'\n";
                }
            } else if (verbose_log) {
                (*verbose_log) << "[ZagEditor] From key NOT found in dict: '" << key << "'\n";
            }
            break;
        }
    }

    if (!changed) return false;

    std::filesystem::path tmp = zag_path;
    tmp += ".tmp";

    std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
    if (!out.is_open()) return false;

    for (size_t i = 0; i < lines.size(); ++i) {
        out << lines[i];
        if (i + 1 < lines.size()) out << "\r\n";
    }
    out.close();

    std::error_code ec;
    std::filesystem::remove(zag_path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::filesystem::rename(tmp, zag_path, ec);
    if (ec) {
        // попробуем вернуть tmp обратно, чтобы не потерять изменения
        std::error_code ec2;
        std::filesystem::rename(tmp, zag_path, ec2);
        return false;
    }
    return true;
}

} // namespace zag_editor

