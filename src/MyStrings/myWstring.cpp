
#include "myWstring.h"
#include "myString.h"

myWstring::myWstring(const std::wstring& wstr) : std::wstring(utf8ToWstring(wstringToUtf8(wstr))) {}

myWstring::myWstring(const wchar_t* p) : std::wstring(p) {}

myWstring::myWstring(const char* str) : std::wstring(utf8ToWstring(std::string(str))) {}

// Перемещающий конструктор из std::string
myWstring::myWstring(std::string&& str) : std::wstring(utf8ToWstring(str)) {}

// Перемещающий конструктор из std::wstring
myWstring::myWstring(std::wstring&& wstr) : std::wstring(std::move(wstr)) {}

// Перемещающий оператор присваивания для std::string
myWstring& myWstring::operator=(std::string&& str) {
    *this = utf8ToWstring(str);
    return *this;
}

// Перемещающий оператор присваивания для std::wstring
myWstring& myWstring::operator=(std::wstring&& wstr) {
    std::wstring::operator=(std::move(wstr));
    return *this;
}

myWstring::myWstring(const myString& str) : std::wstring(utf8ToWstring(static_cast<std::string>(str))) {}

std::string myWstring::to_string() const { return wstringToUtf8(*this); }

myWstring& myWstring::operator=(const std::string& str) {
    *this = utf8ToWstring(str);
    return *this;
}

myWstring& myWstring::operator=(const std::wstring& wstr) {
    std::wstring::operator=(wstr);
    return *this;
}

myWstring& myWstring::operator=(const myString& str) {
    *this = utf8ToWstring(static_cast<std::string>(str));
    return *this;
}


myWstring::operator std::string() const { return to_string(); }
myWstring::operator std::wstring() const { return *this; }

std::ostream& operator<<(std::ostream& os, const myWstring& myStr) {
    os << myStr.to_string();
    return os;
}

std::istream& operator>>(std::istream& is, myWstring& myStr) {
    std::string temp;
    is >> temp;
    myStr = temp;
    return is;
}

void to_json(nlohmann::json& j, const myWstring& myStr) {
    j = myStr.to_string();
}

void from_json(const nlohmann::json& j, myWstring& myStr) {
    myStr = j.get<std::string>();
}

myWstring::myWstring(const std::string &str) : std::wstring(utf8ToWstring(str)) {}


