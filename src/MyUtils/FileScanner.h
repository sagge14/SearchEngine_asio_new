#pragma once
#include <filesystem>
#include <vector>
#include <list>
#include <string>

class FileScanner
{
public:
    static std::list<std::wstring>
    scanDirectory(const std::string& dir,
                  const std::vector<std::string>& extensions = {},
                  const std::vector<std::string>& excludedSubtrees = {});

    static std::vector<std::wstring>
    scanDirectories(const std::vector<std::string>& indexRoots,
                    const std::vector<std::string>& extensions = {},
                    const std::vector<std::string>& excludedSubtrees = {});
};
