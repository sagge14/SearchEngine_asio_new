#pragma once

#include <string>
#include <string_view>

namespace token_issuer {

// JSON-safe printable ASCII for token string fields (no Cyrillic, no escapes).
// Allowed: A-Z a-z 0-9 space . , - _ ( ) / + # @ :
[[nodiscard]] bool IsAsciiTokenField(std::string_view value);
[[nodiscard]] std::string TrimCopy(std::string value);

} // namespace token_issuer
