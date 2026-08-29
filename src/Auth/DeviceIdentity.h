#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace auth {

inline constexpr std::string_view kDeviceTypeUsb = "usb";
inline constexpr std::string_view kDeviceTypeComputer = "computer";

[[nodiscard]] inline bool IsSupportedDeviceType(std::string_view device_type) noexcept
{
    return device_type == kDeviceTypeUsb || device_type == kDeviceTypeComputer;
}

[[nodiscard]] inline std::string TrimCopy(std::string value)
{
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(
        std::find_if(value.rbegin(), value.rend(), notSpace).base(),
        value.end());
    return value;
}

[[nodiscard]] inline std::string NormalizeUsbDeviceId(std::string value)
{
    value = TrimCopy(std::move(value));
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

// Canonical SMBIOS UUID: 8-4-4-4-12 uppercase hex. Accepts braces and optional
// hyphens. Rejects empty, all-zero, and all-F placeholders.
[[nodiscard]] inline std::optional<std::string> NormalizeComputerUuid(
    std::string value)
{
    value = TrimCopy(std::move(value));
    if (value.size() >= 2 && value.front() == '{' && value.back() == '}') {
        value = value.substr(1, value.size() - 2);
        value = TrimCopy(std::move(value));
    }

    std::string hex;
    hex.reserve(32);
    for (const unsigned char c : value) {
        if (c == '-') {
            continue;
        }
        if (!std::isxdigit(c)) {
            return std::nullopt;
        }
        hex.push_back(static_cast<char>(std::toupper(c)));
    }
    if (hex.size() != 32) {
        return std::nullopt;
    }
    if (hex == std::string(32, '0') || hex == std::string(32, 'F')) {
        return std::nullopt;
    }

    std::string canonical;
    canonical.reserve(36);
    canonical.append(hex, 0, 8);
    canonical.push_back('-');
    canonical.append(hex, 8, 4);
    canonical.push_back('-');
    canonical.append(hex, 12, 4);
    canonical.push_back('-');
    canonical.append(hex, 16, 4);
    canonical.push_back('-');
    canonical.append(hex, 20, 12);
    return canonical;
}

[[nodiscard]] inline bool IsUsableComputerUuid(std::string_view value)
{
    return NormalizeComputerUuid(std::string(value)).has_value();
}

} // namespace auth
