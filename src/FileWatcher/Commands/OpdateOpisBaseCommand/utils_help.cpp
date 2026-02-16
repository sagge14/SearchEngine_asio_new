#include "utils_help.h"
#include "MyUtils/Encoding.h"
#include <charconv>
#include <string_view>
#include <cctype>
#include "MyUtils/OEMtoCase.h"
#include "Utils/utf8cpp/utf8/checked.h"
#include <iostream>
#include "Utils/utf8cpp/utf8proc.h"
#include <regex>

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




