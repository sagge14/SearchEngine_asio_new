#pragma once

#include <string>

namespace encoding {

// ── OEM866 / Win-1251 → UTF-8 (Windows API) ─────────────────

/// OEM866 (кодовая страница 866) → UTF-8
std::string oem866_to_utf8(const std::string& oem);

/// Windows-1251 → UTF-8
std::string win1251_to_utf8(const std::string& str);

/// Прочитать файл в OEM866, вернуть содержимое как UTF-8
std::string read_oem866_file_as_utf8(const std::wstring& filePath);

// ── wstring ↔ UTF-8 (через utf8cpp, замена deprecated codecvt) ──

/// wstring (UTF-16 на Windows) → UTF-8 string
std::string wstring_to_utf8(const std::wstring& ws);

/// UTF-8 string → wstring (UTF-16 на Windows)
std::wstring utf8_to_wstring(const std::string& s);

/// Системное сообщение об ошибке (OEM-866 или Windows-1251) → UTF-8
/// Пробует определить кодировку автоматически
std::string system_error_to_utf8(const std::string& msg);

} // namespace encoding
