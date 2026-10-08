#pragma once

#include <optional>
#include <string>

namespace token_issuer {

// Win32_ComputerSystemProduct.UUID via WMI, already canonicalized.
// Returns nullopt when WMI fails or the UUID is unusable.
[[nodiscard]] std::optional<std::string> QueryComputerSystemProductUuid();

// Throws std::runtime_error when a usable SMBIOS UUID cannot be obtained.
[[nodiscard]] std::string RequireComputerDeviceId();

} // namespace token_issuer
