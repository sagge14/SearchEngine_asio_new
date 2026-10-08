#pragma once

#include <string>
#include <optional>

// Быстрые утилиты
void replace_all(std::string& s, std::string_view from, std::string_view to = "");
std::string trim(const std::string& s);
std::optional<int> to_int(std::string_view sv) noexcept;

// UTF-8 text processing (utf8cpp / utf8proc)
std::string capitalize_utf8(const std::string& utf8);
std::string to_lower_utf8(const std::string& input);
