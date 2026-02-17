#include "Encoding.h"

#include <fstream>
#include <filesystem>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef assert
#undef assert
#endif
#include "Utils/utf8cpp/utf8/checked.h"

namespace encoding {

// ── OEM866 → UTF-8 ──────────────────────────────────────────

std::string oem866_to_utf8(const std::string& oem)
{
#ifdef _WIN32
    if (oem.empty()) return {};

    int wlen = MultiByteToWideChar(866, MB_ERR_INVALID_CHARS,
                                   oem.data(), static_cast<int>(oem.size()),
                                   nullptr, 0);
    if (wlen <= 0 || wlen > 10 * 1024 * 1024) {
        std::cerr << "[oem866_to_utf8] MultiByteToWideChar unexpected size: " << wlen
                  << ", GetLastError(): " << GetLastError() << std::endl;
        return {};
    }

    std::wstring wide(wlen, L'\0');
    int res = MultiByteToWideChar(866, MB_ERR_INVALID_CHARS,
                                  oem.data(), static_cast<int>(oem.size()),
                                  wide.data(), wlen);
    if (res <= 0) {
        std::cerr << "[oem866_to_utf8] MultiByteToWideChar conversion error: "
                  << GetLastError() << std::endl;
        return {};
    }

    int ulen = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                   wide.data(), wlen,
                                   nullptr, 0, nullptr, nullptr);
    if (ulen <= 0 || ulen > 10 * 1024 * 1024) {
        std::cerr << "[oem866_to_utf8] WideCharToMultiByte unexpected size: " << ulen
                  << ", GetLastError(): " << GetLastError() << std::endl;
        return {};
    }

    std::string utf8(ulen, '\0');
    res = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                              wide.data(), wlen,
                              utf8.data(), ulen,
                              nullptr, nullptr);
    if (res <= 0) {
        std::cerr << "[oem866_to_utf8] WideCharToMultiByte conversion error: "
                  << GetLastError() << std::endl;
        return {};
    }

    return utf8;
#else
    return oem;
#endif
}

// ── Win-1251 → UTF-8 ────────────────────────────────────────

std::string win1251_to_utf8(const std::string& str)
{
#ifdef _WIN32
    if (str.empty()) return {};

    int wlen = MultiByteToWideChar(1251, MB_ERR_INVALID_CHARS,
                                   str.data(), static_cast<int>(str.size()),
                                   nullptr, 0);
    if (wlen <= 0) {
        std::cerr << "[win1251_to_utf8] MultiByteToWideChar error: "
                  << GetLastError() << std::endl;
        return {};
    }

    std::wstring wstr(wlen, L'\0');
    int res = MultiByteToWideChar(1251, MB_ERR_INVALID_CHARS,
                                  str.data(), static_cast<int>(str.size()),
                                  wstr.data(), wlen);
    if (res <= 0) {
        std::cerr << "[win1251_to_utf8] MultiByteToWideChar conversion error: "
                  << GetLastError() << std::endl;
        return {};
    }

    int utf8len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                      wstr.data(), wlen,
                                      nullptr, 0, nullptr, nullptr);
    if (utf8len <= 0) {
        std::cerr << "[win1251_to_utf8] WideCharToMultiByte error: "
                  << GetLastError() << std::endl;
        return {};
    }

    std::string utf8(utf8len, '\0');
    res = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                              wstr.data(), wlen,
                              utf8.data(), utf8len,
                              nullptr, nullptr);
    if (res <= 0) {
        std::cerr << "[win1251_to_utf8] WideCharToMultiByte conversion error: "
                  << GetLastError() << std::endl;
        return {};
    }

    return utf8;
#else
    return str;
#endif
}

// ── Read OEM866 file → UTF-8 ────────────────────────────────

std::string read_oem866_file_as_utf8(const std::wstring& filePath)
{
    std::ifstream file(std::filesystem::path(filePath), std::ios::binary);
    if (!file.is_open()) return {};

    std::string oem((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    if (oem.empty()) return {};

    return oem866_to_utf8(oem);
}

// ── wstring ↔ UTF-8 (utf8cpp) ───────────────────────────────

std::string wstring_to_utf8(const std::wstring& ws)
{
    std::string result;
    result.reserve(ws.size() * 2);
    utf8::utf16to8(ws.begin(), ws.end(), std::back_inserter(result));
    return result;
}

std::wstring utf8_to_wstring(const std::string& s)
{
    std::wstring result;
    result.reserve(s.size());
    utf8::utf8to16(s.begin(), s.end(), std::back_inserter(result));
    return result;
}

// ── Системное сообщение об ошибке → UTF-8 ──────────────────────

std::string system_error_to_utf8(const std::string& msg)
{
#ifdef _WIN32
    if (msg.empty()) return {};
    
    // На Windows std::error_code::message() обычно возвращает строку
    // в OEM-866 (кодовая страница консоли) или в текущей системной кодовой странице.
    // Пробуем преобразовать через текущую кодовую страницу системы (GetACP)
    UINT cp = GetACP();  // Active Code Page (обычно 1251 для русской Windows)
    
    int wlen = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS,
                                   msg.data(), static_cast<int>(msg.size()),
                                   nullptr, 0);
    if (wlen <= 0) {
        // Если не получилось через системную кодовую страницу,
        // пробуем OEM-866 (кодовая страница консоли)
        return oem866_to_utf8(msg);
    }
    
    std::wstring wide(wlen, L'\0');
    int res = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS,
                                  msg.data(), static_cast<int>(msg.size()),
                                  wide.data(), wlen);
    if (res <= 0) {
        // Fallback на OEM-866
        return oem866_to_utf8(msg);
    }
    
    // Преобразуем wide string в UTF-8
    int utf8len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                      wide.data(), wlen,
                                      nullptr, 0, nullptr, nullptr);
    if (utf8len <= 0) {
        return oem866_to_utf8(msg);  // Fallback
    }
    
    std::string utf8(utf8len, '\0');
    res = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                              wide.data(), wlen,
                              utf8.data(), utf8len,
                              nullptr, nullptr);
    if (res <= 0) {
        return oem866_to_utf8(msg);  // Fallback
    }
    
    return utf8;
#else
    return msg;  // На Linux обычно уже UTF-8
#endif
}

} // namespace encoding
