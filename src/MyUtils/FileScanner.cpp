#include "FileScanner.h"
#include "Encoding.h"
#include "FileExtensionContract.h"
#include "LogFile.h"
#include "PathExclusion.h"
#include <mutex>

namespace fs = std::filesystem;

namespace
{
    void logScanError(const std::wstring& path, const std::string& msg)
    {
        // Преобразуем путь и сообщение об ошибке в UTF-8
        std::string utf8Path = encoding::wstring_to_utf8(path);
        std::string utf8Msg = encoding::system_error_to_utf8(msg);
        LogFile::getScan().write(utf8Path + " | " + utf8Msg);
    }

}


std::list<std::wstring>
FileScanner::scanDirectory(const std::string& dir,
                           const file_extension_contract::Selection& fileTypes,
                           const std::vector<std::string>& excludedSubtrees)
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
        const fs::path currentPath = de.path();

        if (de.is_directory(ec))
        {
            if (ec) { ec.clear(); continue; }

            if (path_exclusion::isPathExcluded(currentPath, excludedSubtrees))
            {
                it.disable_recursion_pending();
            }
            continue;
        }

        if (!de.is_regular_file(ec))
        {
            ec.clear();
            continue;
        }

        if (path_exclusion::isPathExcluded(currentPath, excludedSubtrees))
        {
            continue;
        }

        auto size = de.file_size(ec);
        if (ec || size < 10)
        {
            ec.clear();
            continue;
        }

        if (!file_extension_contract::matchesPath(
                currentPath.wstring(), fileTypes))
            continue;

        out.push_back(currentPath.wstring());
    }

    return out;
}


std::vector<std::wstring>
FileScanner::scanDirectories(const std::vector<std::string>& indexRoots,
                             const file_extension_contract::Selection& fileTypes,
                             const std::vector<std::string>& excludedSubtrees)
{
    std::vector<std::wstring> result;

    for (const auto& dir : indexRoots)
    {
        auto list = scanDirectory(dir, fileTypes, excludedSubtrees);

        result.insert(result.end(),
                      std::make_move_iterator(list.begin()),
                      std::make_move_iterator(list.end()));
    }

    return result;
}
