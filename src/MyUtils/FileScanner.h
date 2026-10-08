#pragma once
#include <filesystem>
#include <vector>
#include <list>
#include <string>
#include "FileExtensionContract.h"

class FileScanner
{
public:
    static std::list<std::wstring>
    scanDirectory(const std::string& dir,
                  const file_extension_contract::Selection& fileTypes,
                  const std::vector<std::string>& excludedSubtrees = {});

    static std::vector<std::wstring>
    scanDirectories(const std::vector<std::string>& indexRoots,
                    const file_extension_contract::Selection& fileTypes,
                    const std::vector<std::string>& excludedSubtrees = {});
};
