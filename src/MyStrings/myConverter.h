
#ifndef MYCONVERTER_H
#define MYCONVERTER_H

#include <string>
#include <codecvt>
#include <locale>

struct myConverter {
    static std::string wstringToUtf8(const std::wstring& wstr) {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
        return conv.to_bytes(wstr);
    }

    static std::wstring utf8ToWstring(const std::string& str) {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
        return conv.from_bytes(str);
    }
};

#endif // MYCONVERTER_H
