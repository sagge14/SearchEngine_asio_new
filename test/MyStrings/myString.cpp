
#include "myString.h"
#include "myWstring.h"

myString::myString(const std::wstring& wstr) : std::string(wstringToUtf8(wstr)) {}
myString::myString(const char* p) : std::string(p) {}
myString::myString(const std::string& str) : std::string(str) {}
myString::myString(const myWstring& wstr) : std::string(wstringToUtf8(static_cast<std::wstring>(wstr))) {}

std::wstring myString::to_wstring() const { return utf8ToWstring(*this); }

myString& myString::operator=(const std::wstring& wstr) {
    *this = wstringToUtf8(wstr);
    return *this;
}

myString& myString::operator=(const std::string& str) {
    std::string::operator=(str);
    return *this;
}

myString& myString::operator=(const myWstring& wstr) {
    *this = wstringToUtf8(static_cast<std::wstring>(wstr));
    return *this;
}

// Перемещающий конструктор из std::string
myString::myString(std::string&& str) : std::string(std::move(str)) {}

// Перемещающий конструктор из std::wstring
myString::myString(std::wstring&& wstr) : std::string(wstringToUtf8(wstr)) {}

// Перемещающий оператор присваивания для std::string
myString& myString::operator=(std::string&& str) {
    std::string::operator=(std::move(str));
    return *this;
}

// Перемещающий оператор присваивания для std::wstring
myString& myString::operator=(std::wstring&& wstr) {
    *this = wstringToUtf8(wstr);
    return *this;
}

myString::operator std::string() const { return *this; }
myString::operator std::wstring() const { return to_wstring(); }

std::ostream& operator<<(std::ostream& os, const myString& myStr) {
    os << static_cast<std::string>(myStr);
    return os;
}

std::istream& operator>>(std::istream& is, myString& myStr) {
    std::string temp;
    is >> temp;
    myStr = myString(temp);
    return is;
}

void to_json(nlohmann::json& j, const myString& myStr) {
    j = static_cast<std::string>(myStr);
}

void from_json(const nlohmann::json& j, myString& myStr) {
    myStr = j.get<std::string>();
}
