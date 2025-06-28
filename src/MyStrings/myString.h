
#ifndef MYSTRING_H
#define MYSTRING_H

#include <string>
#include <iostream>
#include <nlohmann/json.hpp>
#include <boost/serialization/access.hpp>
#include <boost/serialization/base_object.hpp>
#include "myConverter.h"

struct myWstring; // Forward declaration

struct myString : public std::string, private myConverter {
    // Конструктор по умолчанию
    myString() = default;

    explicit myString(const std::wstring& wstr);
    explicit myString(const char* str);    // Конструктор из const char*
    explicit myString(const std::string& str);
    explicit myString(const myWstring& wstr);

    // Перемещающие конструкторы
    explicit myString(std::string&& str);
    explicit myString(std::wstring&& wstr);

// Перемещающие операторы присваивания
    myString& operator=(std::string&& str);
    myString& operator=(std::wstring&& wstr);

    std::wstring to_wstring() const;

    myString& operator=(const std::wstring& wstr);
    myString& operator=(const std::string& str);
    myString& operator=(const myWstring& wstr);

    explicit operator std::string() const;
    explicit operator std::wstring() const;

    // Для Boost Serialization
    template <class Archive>
    void serialize(Archive& ar, const unsigned int version) {
        ar & boost::serialization::base_object<std::string>(*this);
    }

    friend std::ostream& operator<<(std::ostream& os, const myString& myStr);
    friend std::istream& operator>>(std::istream& is, myString& myStr);
};

#endif // MYSTRING_H
