#pragma once

#include "TokenDocument.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace token_issuer {

[[nodiscard]] std::optional<std::filesystem::path> LocalAppDataRoot();

[[nodiscard]] std::optional<std::filesystem::path> StandardComputerTokenDirectory();

[[nodiscard]] std::optional<std::filesystem::path> StandardComputerTokenPath();

[[nodiscard]] bool EnsureComputerTokenDirectory(
    const std::filesystem::path& directory,
    std::string* error = nullptr);

} // namespace token_issuer
