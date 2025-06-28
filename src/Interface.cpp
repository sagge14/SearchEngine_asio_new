//
// Created by Sg on 01.10.2023.
//

#include "Interface.h"
#include "SearchServer.h"
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

std::list<std::wstring> Interface::getAllFilesFromDir2(const std::string& dir, const std::list<std::string>& exts, const std::vector<std::string>& excludeDirs) {
    /**
    Функция получения всех файлов из дирректории @param dir и ее подпапках, исключая имена папок и заданные исключенные директории */

    namespace fs = std::filesystem;
    using convert_t = std::codecvt_utf8<wchar_t>;
    std::wstring_convert<convert_t, wchar_t> strconverter;

    auto recursiveGetFileNamesByExtension = [&exts, &excludeDirs](const std::wstring &path)
    {
        std::list<std::wstring> _docPaths;
        auto opt1 = std::filesystem::directory_options::skip_permission_denied;
        auto opt2 = std::filesystem::directory_options::follow_directory_symlink;
        auto iter = fs::recursive_directory_iterator(path, opt1 | opt2);
        auto end_iter = fs::end(iter);
        auto ec = std::error_code();

        for (; iter != end_iter; iter.increment(ec))
        {
            if (ec)
            {
                continue;
            }
            auto p = *iter;
            auto dirPath = p.path().wstring();
            auto fName = p.path().string();

            // Проверяем, если текущая директория находится в списке исключений
            bool isExcluded = std::any_of(excludeDirs.begin(), excludeDirs.end(),
                                          [&dirPath](const std::string& excludeDir) {
                                              namespace fs = std::filesystem;
                                              using convert_t = std::codecvt_utf8<wchar_t>;
                                              std::wstring_convert<convert_t, wchar_t> strconverter;
                                              return dirPath.find(strconverter.from_bytes(excludeDir)) == 0;
                                          });

            if (isExcluded)
            {
                iter.disable_recursion_pending();
                continue;
            }

            auto trueExt = [fName](const std::list<std::string>& exts)
            {
                for (const auto& ext : exts) {

                    if (ext == "")
                    {
                        if (fName.find('.') == std::string::npos)
                            return true;
                    }
                    else
                    {
                        auto b = std::equal(fName.end() - ext.length(), fName.end(), ext.begin(),
                                            [](char ch1, char ch2) { return tolower(ch1) == tolower(ch2); });
                        if (b)
                            return true;
                    }
                }
                return false;
            };

            if (p.is_regular_file() && fs::file_size(p) > 10 && (!exts.size() || trueExt(exts)))
                _docPaths.push_back(p.path().wstring());
        }

        return _docPaths;
    };

    return std::move(recursiveGetFileNamesByExtension(strconverter.from_bytes(dir)));
}


bool folderExists(const std::wstring& path)
{
    DWORD attrib = GetFileAttributesW(path.c_str());
    return (attrib != INVALID_FILE_ATTRIBUTES) && (attrib & FILE_ATTRIBUTE_DIRECTORY);
}


std::list<std::wstring> Interface::getAllFilesFromDirs(const std::list<std::string> dirs,
                                                       const std::list<std::string> ext,
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

