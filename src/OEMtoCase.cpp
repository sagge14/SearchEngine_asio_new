//
// Created by Sg on 10.07.2025.
//

#include "OEMtoCase.h"

#include <stdexcept>
#include <filesystem>
#include <minwindef.h>
#include <libloaderapi.h>

std::string OEMtoCase::getExecutableDir() {

#ifdef _WIN32
    char buffer[MAX_PATH];
    GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    std::filesystem::path exePath(buffer);
    return exePath.parent_path().string();
#else
    // Для Linux (пример, не всегда работает везде)
    char buffer[4096];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len != -1) {
        buffer[len] = '\0';
        std::filesystem::path exePath(buffer);
        return exePath.parent_path().string();
    }
    throw std::runtime_error("Can't get executable path");
#endif
}