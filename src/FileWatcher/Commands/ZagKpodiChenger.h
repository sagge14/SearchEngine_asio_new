#pragma once
#include "IFileEventCommand.h"
#include "MyUtils/LogFile.h"
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <algorithm>




class ZagKpodiChengerCommand : public IFileEventCommand {
public:
    ZagKpodiChengerCommand()
    {

        loadMapFromFile(L"D:\\BASES_PRD\\EXPORT.INI");
        //printMap();
    }

    // Загружает карту из файла по описанным правилам
    bool loadMapFromFile(const std::wstring& path_to_dict) {
        namespace fs = std::filesystem;
        std::wifstream in((fs::path(path_to_dict)));
        if (!in.is_open()) return false;

        std::vector<std::wstring> lines;
        for (std::wstring line; std::getline(in, line); )
            lines.push_back(std::move(line));
        in.close();

        size_t inserted = 0;
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::wstring& cur = lines[i];
            if (!starts_with(cur, L"s=")) continue;

            // извлекаем слово после s= до '/' или пробела/табуляции
            std::wstring value = parse_s_key(cur);
            if (value.empty()) continue;

            if (i == 0) continue; // нет строки выше

            const std::wstring& prev = lines[i - 1];
            if (starts_with(prev, L"s=")) continue; // верхняя тоже s= — пропускаем

            std::wstring key = trim(prev);
            if (value.empty()) continue;

            // Вставляем. Если ключ уже есть — не перезаписываем (можно поменять на operator[] для overwrite)
            auto [it, ok] = mapKpodi_.emplace(std::move(key), std::move(value));
            if (ok) ++inserted;
        }

        return inserted > 0;
    }

    void execute(const std::wstring& path) override {
        namespace fs = std::filesystem;
        fs::path p(path);

        if (isInDirWithWrongExt(p)) return;

        std::wifstream in(p);
        if (!in.is_open()) return;

        std::vector<std::wstring> lines;
        for (std::wstring line; std::getline(in, line); )
            lines.push_back(std::move(line));
        in.close();

        bool changed = false;

        for (auto& line : lines) {
            if (starts_with(line, L"From=")) {
                std::wstring key = trim(line.substr(5));
                auto it = mapKpodi_.find(key);
                if (it != mapKpodi_.end()) {
                    line = L"From=" + it->second;
                    changed = true;
                }
                break; // только первая From=
            }
        }

        if (!changed) return;

        fs::path tmp = p; tmp += L".tmp";
        std::wofstream out(tmp, std::ios::trunc);
        if (!out.is_open()) return;

        for (size_t i = 0; i < lines.size(); ++i) {
            out << lines[i];
            if (i + 1 < lines.size()) out << L'\n';
        }
        out.close();

        std::error_code ec;
        fs::remove(p, ec);
        fs::rename(tmp, p, ec);
    }

private:
    std::map<std::wstring, std::wstring> mapKpodi_;

    static bool starts_with(const std::wstring& s, const std::wstring& pref) {
        return s.size() >= pref.size() && s.compare(0, pref.size(), pref) == 0;
    }

    static std::wstring trim(const std::wstring& s) {
        size_t i = 0, j = s.size();
        while (i < j && iswspace(s[i])) ++i;
        while (j > i && iswspace(s[j - 1])) --j;
        return s.substr(i, j - i);
    }

    // s=WORD/anything  -> "WORD"
    // также обрываем по пробелу или табу, если встретятся раньше '/'
    static std::wstring parse_s_key(const std::wstring& line) {
        if (!starts_with(line, L"s=")) return {};
        size_t pos = 2; // после "s="
        // граница ключа: '/', пробел, таб или конец строки
        size_t end = pos;
        while (end < line.size()) {
            wchar_t ch = line[end];
            if (ch == L'/' || ch == L' ' || ch == L'\t') break;
            ++end;
        }
        std::wstring key = line.substr(pos, end - pos);
        return trim(key);
    }

    static bool isInDirWithWrongExt(const std::filesystem::path& p) {
        auto parent = p.parent_path().filename().wstring();
        if (parent.size() > 2 && parent.rfind(L"in", 0) == 0) {
            bool all_digits = std::all_of(parent.begin() + 2, parent.end(),
                                          [](wchar_t ch){ return iswdigit(ch); });
            if (all_digits) {
                return !(p.has_extension() && p.extension() == L".zag");
            }
        }
        return false;
    }

    void printMap() const {
        for (const auto& [key, value] : mapKpodi_) {
            LogFile::getWatcher().write(L"[" + key + L"] = " + value);
        }
    }
};
