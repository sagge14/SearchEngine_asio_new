#pragma once

#include <optional>
#include <string_view>

namespace inverted_index {

enum class FullIndexStrategy {
    Legacy,
    Batch
};

[[nodiscard]] constexpr std::string_view toString(
    FullIndexStrategy strategy) noexcept
{
    switch (strategy) {
    case FullIndexStrategy::Legacy:
        return "legacy";
    case FullIndexStrategy::Batch:
        return "batch";
    }
    return "legacy";
}

[[nodiscard]] constexpr std::optional<FullIndexStrategy>
parseFullIndexStrategy(std::string_view value) noexcept
{
    if (value == "legacy")
        return FullIndexStrategy::Legacy;
    if (value == "batch")
        return FullIndexStrategy::Batch;
    return std::nullopt;
}

} // namespace inverted_index
