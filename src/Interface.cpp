//
// Created by Sg on 01.10.2023.
//
#include <vector>
#include <codecvt>
#include <locale>
#include <filesystem>
#include <string>
#include <list>
#include <cwctype>
#include "Interface.h"
#include "SearchServer/SearchServer.h"

std::string wstring_to_utf85(const std::wstring& ws)
{
    std::string result;
    utf8::utf16to8(ws.begin(), ws.end(), std::back_inserter(result));
    return result;
}

std::string utf8_to_wstring(const std::wstring& ws)
{
    std::string result;
    utf8::utf8to16(ws.begin(), ws.end(), std::back_inserter(result));
    return result;
}


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




void logScanError(const std::wstring& path, const std::string& msg)
{
    static std::mutex mtx;
    std::lock_guard<std::mutex> lock(mtx);

    std::ofstream out("scan_errors.log", std::ios::app);
    if (!out) return;

    out << my::utf8(path) << " | " << msg << '\n';
}

bool matchExtension(const std::wstring& fileName,
                    const std::vector<std::string>& exts)
{
    if (exts.empty())
        return true;

    for (const auto& extUtf8 : exts)
    {
        std::wstring ext = std::filesystem::u8path(extUtf8).wstring();

        if (ext.empty())
        {
            if (fileName.find(L'.') == std::wstring::npos)
                return true;
        }
        else
        {
            if (fileName.size() < ext.size())
                continue;

            if (std::equal(fileName.end() - ext.size(),
                           fileName.end(),
                           ext.begin(),
                           [](wchar_t a, wchar_t b)
                           {
                               return std::towlower(a) == std::towlower(b);
                           }))
                return true;
        }
    }
    return false;
}


std::list<std::wstring>
Interface::getAllFilesFromDir2(const std::string& dir,
                               const std::vector<std::string>& exts,
                               const std::vector<std::string>& excludeDirs)
{
    namespace fs = std::filesystem;

    std::list<std::wstring> out;
    fs::path root = fs::u8path(dir);

    auto opts = fs::directory_options::skip_permission_denied;

    std::error_code ec;
    fs::recursive_directory_iterator it(root, opts, ec), end;

    if (ec)
    {
        logScanError(root.wstring(), ec.message());
        return out;
    }

    /* ---------- обход ---------- */
    for (; it != end; it.increment(ec))
    {
        if (ec)
        {
            logScanError(it->path().wstring(), ec.message());
            ec.clear();
            continue;
        }

        const fs::directory_entry& de = *it;

        /* --- каталоги / исключения --- */
        if (de.is_directory(ec))
        {
            if (ec) { ec.clear(); continue; }

            const std::wstring wDir = de.path().wstring();

            bool excluded = false;
            for (const auto& exclUtf8 : excludeDirs)
            {
                fs::path exclPath = fs::u8path(exclUtf8);
                if (wDir.find(exclPath.wstring()) != std::wstring::npos)
                {
                    excluded = true;
                    break;
                }
            }

            if (excluded)
            {
                it.disable_recursion_pending();
            }
            continue;
        }

        /* --- только файлы --- */
        if (!de.is_regular_file(ec))
        {
            ec.clear();
            continue;
        }

        /* --- размер --- */
        auto size = de.file_size(ec);
        if (ec || size < 10)
        {
            ec.clear();
            continue;
        }

        /* --- расширение --- */
        const std::wstring fname = de.path().filename().wstring();
        if (!matchExtension(fname, exts))
            continue;

        out.push_back(de.path().wstring());
    }

    return out;
}


bool folderExists(const std::wstring& path)
{
    DWORD attrib = GetFileAttributesW(path.c_str());
    return (attrib != INVALID_FILE_ATTRIBUTES) && (attrib & FILE_ATTRIBUTE_DIRECTORY);
}


std::vector<std::wstring>
Interface::getAllFilesFromDirs(const std::vector<std::string>& dirs,
                               const std::vector<std::string>& ext,
                               const std::vector<std::string>& excludeDirs)
{
    std::vector<std::wstring> result;
    namespace fs = std::filesystem;

    for (const auto& dirStr : dirs)
    {
        fs::path p = fs::u8path(dirStr);

        if (!folderExists(p.wstring()))
        {
            search_server::addToLog("Missing dir: " + dirStr);
            continue;
        }

        auto list = getAllFilesFromDir2(dirStr, ext, excludeDirs);

        result.insert(result.end(),
                      std::make_move_iterator(list.begin()),
                      std::make_move_iterator(list.end()));

    }

    return result;
}


