#pragma once

#include "Commands/TelegramFiles/TelegramFileResolver.h"

#include <cstdint>
#include <vector>

class PdtvFileResolver
{
public:
    [[nodiscard]] static ResolveTelegramFileResult resolve(
        const std::vector<std::uint8_t>& request);
};
