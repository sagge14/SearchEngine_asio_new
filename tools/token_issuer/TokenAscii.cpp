#include "TokenAscii.hpp"

#include <algorithm>
#include <cctype>

namespace token_issuer {

std::string TrimCopy(std::string value)
{
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(
        std::find_if(value.rbegin(), value.rend(), notSpace).base(),
        value.end());
    return value;
}

bool IsAsciiTokenField(std::string_view value)
{
    for (const unsigned char c : value) {
        if (c < 0x20 || c > 0x7E) {
            return false;
        }
        switch (c) {
        case '"':
        case '\\':
            return false;
        default:
            break;
        }
        const bool ok =
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == ' ' || c == '.' || c == ',' || c == '-' || c == '_' ||
            c == '(' || c == ')' || c == '/' || c == '+' || c == '#' ||
            c == '@' || c == ':';
        if (!ok) {
            return false;
        }
    }
    return true;
}

} // namespace token_issuer
