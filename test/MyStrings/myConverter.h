
#ifndef MYCONVERTER_H
#define MYCONVERTER_H

#include "MyUtils/Encoding.h"

struct myConverter {
    static std::string wstringToUtf8(const std::wstring& wstr) {
        return encoding::wstring_to_utf8(wstr);
    }

    static std::wstring utf8ToWstring(const std::string& str) {
        return encoding::utf8_to_wstring(str);
    }
};

#endif // MYCONVERTER_H
