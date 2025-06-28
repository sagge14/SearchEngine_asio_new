
#ifndef MYWSTRING_H
#define MYWSTRING_H

#include <string>
#include <iostream>
#include <nlohmann/json.hpp>
#include <boost/serialization/access.hpp>
#include "myConverter.h"

struct myString; // Forward declaration

struct myWstring : public std::wstring, private myConverter {
    explicit myWstring(const std::string& str);
    explicit myWstring(const wchar_t* p);
    explicit myWstring(const std::wstring& wstr);
    explicit myWstring(const myString& str);
    myWstring() = default;


    explicit myWstring(const char* str);

    [[nodiscard]] std::string to_string() const;

    myWstring& operator=(const std::string& str);
    myWstring& operator=(const std::wstring& wstr);
    myWstring& operator=(const myString& str);

    explicit myWstring(std::string&& str);
    explicit myWstring(std::wstring&& wstr);

// Перемещающие операторы присваивания
    myWstring& operator=(std::string&& str);
    myWstring& operator=(std::wstring&& wstr);

    explicit operator std::string() const;
    explicit operator std::wstring() const;

    friend std::ostream& operator<<(std::ostream& os, const myWstring& myStr);
    friend std::istream& operator>>(std::istream& is, myWstring& myStr);

    friend void to_json(nlohmann::json& j, const myWstring& myStr);
    friend void from_json(const nlohmann::json& j, myWstring& myStr);

    template <class Archive>
    void serialize(Archive& ar, const unsigned int version) {
        std::string utf8Str = to_string();
        ar & utf8Str;
        if (Archive::is_loading::value) {
            *this = utf8ToWstring(utf8Str);
        }
    }
};

#endif // MYWSTRING_H
