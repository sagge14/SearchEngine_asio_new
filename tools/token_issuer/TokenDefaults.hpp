#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace token_issuer {

nlohmann::json BuiltInTokenDefaults();

// Load defaults: explicit path, beside EXE, data/ beside EXE, or built-in.
// Throws on unreadable/invalid JSON or non-ASCII string fields in the template.
nlohmann::json LoadTokenDefaults(
    const std::filesystem::path& exe_dir,
    const std::filesystem::path& explicit_path);

std::string GenerateClientId();

} // namespace token_issuer
