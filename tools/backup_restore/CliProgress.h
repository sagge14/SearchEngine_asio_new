#pragma once

#include "Backup/Restore/RestoreInterfaces.h"

#include <iostream>
#include <string>

class CliProgress final : public IRestoreProgress {
public:
    void onPhase(const std::string& name) override
    {
        std::cout << "[phase] " << name << '\n';
    }

    void onFile(
        size_t index,
        size_t total,
        const std::string& path) override
    {
        std::cout
            << "  [" << index << '/' << total << "] "
            << path << '\n';
    }

    void onWarning(const std::string& message) override
    {
        std::cerr << "WARNING: " << message << '\n';
    }

    bool isCancelled() const override
    {
        return false;
    }
};
