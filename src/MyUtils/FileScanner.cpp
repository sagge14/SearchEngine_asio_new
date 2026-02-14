#include "FileScanner.h"
#include "UtfUtil.h"
#include <fstream>
#include <mutex>
#include <cwctype>

namespace fs = std::filesystem;

namespace
{
    void logScanError(const std::wstring& path, const std::string& msg)
    {
        static std::mutex mtx;
        std::lock_guard<std::mutex> lock(mtx);

        std::ofstream out("scan_errors.log", std::ios::app);
        if (!out) return;

        out << utf::to_utf8(path) << " | " << msg << '\n';
    }

    bool matchExtension(const std::wstring& fileName,
                        const std::vector<std::string>& exts)
    {
        if (exts.empty())
            return true;

        for (const auto& extUtf8 : exts)
        {
            std::wstring ext = fs::u8path(extUtf8).wstring();

            if (ext.empty())
            {
                if (fileName.find(L'.') == std::wstring::npos)
                    return true;
            }
            else if (fileName.size() >= ext.size())
            {
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
}


std::list<std::wstring>
FileScanner::scanDirectory(const std::string& dir,
                           const std::vector<std::string>& extensions,
                           const std::vector<std::string>& excludeDirs)
{
    std::list<std::wstring> out;
    fs::path root = fs::u8path(dir);

    std::error_code ec;
    fs::recursive_directory_iterator it(
            root,
            fs::directory_options::skip_permission_denied,
            ec);

    if (ec)
    {
        logScanError(root.wstring(), ec.message());
        return out;
    }

    for (; it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec)
        {
            logScanError(it->path().wstring(), ec.message());
            ec.clear();
            continue;
        }

        const auto& de = *it;

        if (de.is_directory(ec))
        {
            if (ec) { ec.clear(); continue; }

            const std::wstring wDir = de.path().wstring();

            for (const auto& exclUtf8 : excludeDirs)
            {
                if (wDir.find(fs::u8path(exclUtf8).wstring()) != std::wstring::npos)
                {
                    it.disable_recursion_pending();
                    break;
                }
            }
            continue;
        }

        if (!de.is_regular_file(ec))
        {
            ec.clear();
            continue;
        }

        auto size = de.file_size(ec);
        if (ec || size < 10)
        {
            ec.clear();
            continue;
        }

        const std::wstring fname = de.path().filename().wstring();
        if (!matchExtension(fname, extensions))
            continue;

        out.push_back(de.path().wstring());
    }

    return out;
}


std::vector<std::wstring>
FileScanner::scanDirectories(const std::vector<std::string>& dirs,
                             const std::vector<std::string>& extensions,
                             const std::vector<std::string>& excludeDirs)
{
    std::vector<std::wstring> result;

    for (const auto& dir : dirs)
    {
        auto list = scanDirectory(dir, extensions, excludeDirs);

        result.insert(result.end(),
                      std::make_move_iterator(list.begin()),
                      std::make_move_iterator(list.end()));
    }

    return result;
}
