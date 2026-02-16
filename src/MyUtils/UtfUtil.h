#pragma once
#include <string>
#include <cassert>

#ifdef assert
#undef assert
#endif

#include "Utils/utf8cpp/utf8/checked.h"


namespace utf
{
    // wstring → UTF-8
    inline std::string to_utf8(const std::wstring& ws)
    {
        std::string result;
        utf8::utf16to8(ws.begin(), ws.end(), std::back_inserter(result));
        return result;
    }

    // UTF-8 → wstring
    inline std::wstring from_utf8(const std::string& s)
    {
        std::wstring result;
        utf8::utf8to16(s.begin(), s.end(), std::back_inserter(result));
        return result;
    }
}
