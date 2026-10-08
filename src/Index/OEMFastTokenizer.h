#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <functional>

#include "MyUtils/OEMCase.h"

namespace OEMFastTokenizer
{
    // Таблица допустимых символов (буквы/цифры OEM866)
    extern bool isWordChar[256];

    void initTables();

    // Основной метод быстрой токенизации OEM866-буфера
    void tokenizeBuffer(
            const char* data,
            size_t len,
            std::string& carry,
            const std::function<void(std::string_view)>& callback);

    // Быстрая нормализация токена: UPPER + TRIM
    void normalizeToken(std::string& s);
}
