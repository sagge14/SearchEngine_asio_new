#pragma once

#include <string>
#include <optional>

// Быстрые утилиты
void replace_all(std::string& s, std::string_view from, std::string_view to = "");
std::string trim(const std::string& s);
std::optional<int> to_int(std::string_view sv) noexcept;

std::string read_oem866_file_as_utf8(const std::wstring& filePath);
// Кодировка OEM866 → UTF-8 (Windows)
std::string oem866_to_utf8(const std::string& oem);

// OEM-case (регистр по таблице)
class OEMtoCase;
std::string to_upper(std::string_view sv);
std::string to_lower_keep_first(std::string s);
std::string capitalize_oem(std::string s);
std::string capitalize_utf8(const std::string& utf8);
std::string utf8_to_oem866(const std::string& utf8);
std::string sanitize_utf8(const std::string& input);
std::string to_lower_utf8(const std::string& input);
std::string to_lower_utf82(const std::string& input);