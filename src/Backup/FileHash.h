#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct FileHashResult {
    bool ok = false;
    std::string sha256;
    std::uint64_t size = 0;
    std::string message;
};

FileHashResult sha256File(const std::filesystem::path& path);
std::string sha256String(const std::string& value);
