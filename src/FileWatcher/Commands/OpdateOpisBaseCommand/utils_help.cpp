#include "utils_help.h"
#include <charconv>
#include <windows.h>
#include <string_view>
#include <cctype>
#include "OEMtoCase.h"
#include "utf8cpp/utf8/checked.h"
#include <iostream>
#include <utf8proc.h>
#include <regex>

// --- строковые утилиты ---
void replace_all(std::string& s, std::string_view from, std::string_view to)
{
    for (size_t pos{}; (pos = s.find(from, pos)) != std::string::npos; )
    {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string trim(const std::string& s)
{
    static const std::regex trim_re(R"(^\s+|\s+$)");
    return std::regex_replace(s, trim_re, "");
}

std::optional<int> to_int(std::string_view sv) noexcept
{
    int value{};
    auto [p, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
    return (ec == std::errc{}) ? std::optional<int>{value} : std::nullopt;
}

std::string to_lower_utf8(const std::string& input) {
    utf8proc_uint8_t* result = utf8proc_NFKC_Casefold(
            reinterpret_cast<const utf8proc_uint8_t*>(input.c_str())
    );

    std::string lowered(reinterpret_cast<char*>(result));
    free(result);
    return lowered;
}

#include <cwctype>
#include <clocale>
#include <locale>
#include <string>

std::string capitalize_utf8(const std::string& utf8)
{
    if (utf8.empty()) return utf8;

    std::string result;
    auto it = utf8.begin();
    auto end = utf8.end();

    // Первая буква — в верхний регистр
    uint32_t cp = utf8::next(it, end);
    wchar_t wch = static_cast<wchar_t>(cp);
    wch = std::towupper(wch);
    utf8::utf32to8(&wch, &wch + 1, std::back_inserter(result));

    // Остальные буквы — в нижний регистр
    while (it != end) {
        uint32_t c = utf8::next(it, end);
        wchar_t wch2 = static_cast<wchar_t>(c);
        wch2 = std::towlower(wch2);
        utf8::utf32to8(&wch2, &wch2 + 1, std::back_inserter(result));
    }

    return result;
}

std::string to_lower_utf82(const std::string& input) {
    std::string utf8;
    // Преобразуем в lower-case
    utf8proc_uint8_t* lowered = utf8proc_NFKC_Casefold(
            reinterpret_cast<const utf8proc_uint8_t*>(input.c_str())
    );
    std::cout <<" 1-  " << *utf8.begin() << std::endl;
    std::cout <<" 2-  " << *input.begin() << std::endl;
    if (lowered) {
        utf8 = std::string{reinterpret_cast<char*>(lowered)};
        free(lowered); // обязательно через free, не delete!
    } else {
        utf8 = input;
        free(lowered); // обязательно через free, не delete!
    }
    return utf8;
}

#include <utf8proc.h>
#include <string>
#include <cstring>          // memcpy
#include <cstdint>          // intptr_t / utf8proc_ssize_t

std::string capitalize_utf82(const std::string& s)
{
    if (s.empty()) return s;

    utf8proc_int32_t cp_first;
    utf8proc_ssize_t n = utf8proc_iterate(      // ← здесь
            reinterpret_cast<const utf8proc_uint8_t*>(s.data()),
            s.size(),
            &cp_first);

    if (n <= 0) return s;                       // не-валидный UTF-8

    std::string first(s.data(), static_cast<size_t>(n));

    const utf8proc_uint8_t* tail =
            reinterpret_cast<const utf8proc_uint8_t*>(s.data() + n);
    utf8proc_ssize_t tail_len =
            static_cast<utf8proc_ssize_t>(s.size() - n);

    utf8proc_uint8_t* lower =
            utf8proc_NFKC_Casefold(tail);

    std::string result;
    result.reserve(s.size());

    utf8proc_uint8_t upbuf[4];
    int up_n = utf8proc_encode_char(utf8proc_toupper(cp_first), upbuf);
    result.append(reinterpret_cast<char*>(upbuf), up_n);

    if (lower) {
        result += reinterpret_cast<char*>(lower);
        free(lower);
    } else {
        result.append(reinterpret_cast<const char*>(tail),
                      static_cast<size_t>(tail_len));
    }
    return result;
}



std::string oem866_to_utf8(const std::string& oem)
{
    if (oem.empty()) return {};

    // OEM-866 → UTF-16
    int wlen = MultiByteToWideChar(866, MB_ERR_INVALID_CHARS,
                                   oem.data(), static_cast<int>(oem.size()),
                                   nullptr, 0);

    if (wlen <= 0 || wlen > 10 * 1024 * 1024) { // максимум 10 млн символов (около 20 МБ)
        std::cerr << "[oem866_to_utf8] MultiByteToWideChar размер странный: " << wlen
                  << ", GetLastError(): " << GetLastError() << std::endl;
        return {};
    }

    std::wstring wide(wlen, L'\0');
    int res = MultiByteToWideChar(866, MB_ERR_INVALID_CHARS,
                                  oem.data(), static_cast<int>(oem.size()),
                                  wide.data(), wlen);
    if (res <= 0) {
        std::cerr << "[oem866_to_utf8] MultiByteToWideChar ошибка преобразования: "
                  << GetLastError() << std::endl;
        return {};
    }

    // UTF-16 → UTF-8
    int ulen = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                   wide.data(), wlen,
                                   nullptr, 0, nullptr, nullptr);
    if (ulen <= 0 || ulen > 10 * 1024 * 1024) {
        std::cerr << "[oem866_to_utf8] WideCharToMultiByte размер странный: " << ulen
                  << ", GetLastError(): " << GetLastError() << std::endl;
        return {};
    }

    std::string utf8(ulen, '\0');
    res = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                              wide.data(), wlen,
                              utf8.data(), ulen,
                              nullptr, nullptr);
    if (res <= 0) {
        std::cerr << "[oem866_to_utf8] WideCharToMultiByte ошибка преобразования: "
                  << GetLastError() << std::endl;
        return {};
    }

    return utf8;
}

std::string read_oem866_file_as_utf8(const std::wstring& wfilePath)
{
    std::ifstream file(std::filesystem::path(wfilePath), std::ios::binary);
    if (!file.is_open()) return {};

    std::string oem((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    if (oem.empty()) return {};

        return oem866_to_utf8(oem);
}






