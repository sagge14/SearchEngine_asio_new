//
// Created by Sg on 01.10.2023.
//

#include "Interface.h"
#include "SearchServer/SearchServer.h"
std::list<std::wstring> Interface::getAllFilesFromDir(const std::string& dir, const std::list<std::string>& exts) {

    /**
    Функция получения всех файлов из дирректории @param dir и ее подпапках, исключая имена папок */

    namespace fs = std::filesystem;
    using convert_t = std::codecvt_utf8<wchar_t>;
    std::wstring_convert<convert_t, wchar_t> strconverter;

    auto recursiveGetFileNamesByExtension = [&exts](const std::wstring &path)
    {
        std::list<std::wstring> _docPaths;
        auto opt1 = std::filesystem::directory_options::skip_permission_denied;
        auto opt2 = std::filesystem::directory_options::follow_directory_symlink;
        auto iter = fs::recursive_directory_iterator(path,opt1 | opt2);
        auto end_iter = fs::end(iter);
        auto ec = std::error_code();

        for(;iter!=end_iter;iter.increment(ec))
        {
            if(ec)
            {
                continue;
            }
            auto p = *iter;
            auto fName = p.path().string();

            auto trueExt = [fName] (const std::list<std::string>& exts)
            {

                for(const auto& ext:exts) {

                    if(ext == "")
                    {
                        if(fName.find('.') == std::string::npos)
                            return true;
                    }
                    else
                    {
                        auto b = std::equal(fName.end() - ext.length(), fName.end(), ext.begin(),
                                            [](char ch1, char ch2) { return tolower(ch1) == tolower(ch2); });
                        if(b)
                            return true;
                    }
                }
                return false;
            };

            if(p.is_regular_file() && fs::file_size(p) > 10  && (!exts.size() || trueExt(exts)))
                _docPaths.push_back(p.path().wstring());}

        return _docPaths;
    };

    return std::move(recursiveGetFileNamesByExtension(strconverter.from_bytes(dir)));
}


#include <vector>
#include <codecvt>
#include <locale>
#include <filesystem>
#include <string>
#include <list>

std::list<std::wstring>
Interface::getAllFilesFromDir2(const std::string& dir,
                               const std::vector<std::string>& exts,
                               const std::vector<std::string>& excludeDirs)
{
    namespace fs = std::filesystem;
    using convert_t = std::codecvt_utf8<wchar_t>;
    std::wstring_convert<convert_t, wchar_t> toWide;

    const fs::path root = toWide.from_bytes(dir);
    std::list<std::wstring> out;

    /* ---------- опции обхода ---------- */
    auto opts = fs::directory_options::skip_permission_denied
                | fs::directory_options::follow_directory_symlink;

    std::error_code ec;
    fs::recursive_directory_iterator it(root, opts, ec), end;

    /* ---------- обход ---------- */
    for (; it != end; it.increment(ec))
    {
        /* если во время increment возникла ошибка – пропускаем элемент */
        if (ec) { ec.clear(); continue; }

        const fs::directory_entry& de = *it;

        /* --- 1. исключённые каталоги -------------------------------- */
        if (de.is_directory(ec))
        {
            if (ec) { ec.clear(); continue; }

            const std::wstring wDir = de.path().wstring();

            bool excluded = std::any_of(excludeDirs.begin(), excludeDirs.end(),
                                        [&wDir](const std::string& excl)
                                        {
                                            using convert_t = std::codecvt_utf8<wchar_t>;
                                            std::wstring_convert<convert_t, wchar_t> conv;
                                            return wDir.find(conv.from_bytes(excl)) == 0;
                                        });

            if (excluded)
            {
                it.disable_recursion_pending();   // не заходить глубже
                continue;
            }
            continue;                             // папка нас не интересует
        }

        /* --- 2. только regular_file --------------------------------- */
        if (!de.is_regular_file(ec)) { ec.clear(); continue; }

        /* --- 3. размер > 10 байт ------------------------------------ */
        auto fsize = de.file_size(ec);
        if (ec || fsize < 10) { ec.clear(); continue; }

        /* --- 4. проверка расширения --------------------------------- */
        const std::string fileName = de.path().string();
        auto matchExt = [&fileName](const std::string& ext) -> bool
        {
            if (ext.empty())
                return fileName.find('.') == std::string::npos;     // «без точки»

            if (ext.size() > fileName.size()) return false;

            return std::equal(fileName.end() - ext.size(),
                              fileName.end(),
                              ext.begin(),
                              [](char a, char b)
                              { return std::tolower(a) == std::tolower(b); });
        };

        bool okExt = exts.empty() ||
                     std::any_of(exts.begin(), exts.end(), matchExt);

        if (okExt)
            out.push_back(de.path().wstring());
    }

    return out;      // NRVO, явный move не нужен
}



bool folderExists(const std::wstring& path)
{
    DWORD attrib = GetFileAttributesW(path.c_str());
    return (attrib != INVALID_FILE_ATTRIBUTES) && (attrib & FILE_ATTRIBUTE_DIRECTORY);
}


std::list<std::wstring> Interface::getAllFilesFromDirs(const std::vector<std::string>& dirs,
                                                       const std::vector<std::string>& ext,
                                                       const std::vector<std::string>& excludeDirs) {
    std::list<std::wstring> result{};

    for (const auto& dirStr : dirs)
    {
        std::wstring wDir = std::filesystem::u8path(dirStr).wstring();

        if (!folderExists(wDir)) {
            search_server::SearchServer::addToLog("Missing dir: " + dirStr);
            continue;
        }

        auto list = getAllFilesFromDir2(dirStr, ext, excludeDirs); // предполагаем, что эта функция уже возвращает list<wstring>
        list.sort();
        result.sort();
        result.merge(list);
    }

    return result;
}

