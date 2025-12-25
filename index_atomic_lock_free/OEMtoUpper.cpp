#include "OEMtoUpper.h"
#include <fstream>
#include <algorithm>
#include <cctype>


    unsigned char OEMtoUpper::table[256];
    bool OEMtoUpper::initialized = false;
    char OEMtoUpper::firstOEM = 0;

    void OEMtoUpper::init(const std::string& path)
    {
        if (initialized)
            return;

        // 1. Базовая таблица: точный аналог "else return std::toupper(c);"
        //    Сначала заполняем toupper'ом для всех 0..255
        for (int i = 0; i < 256; ++i) {
            // старый код вызывал toupper(c) с char,
            // корректный эквивалент — toupper((unsigned char)i)
            table[i] = static_cast<unsigned char>(
                    std::toupper(static_cast<unsigned char>(i))
            );
        }

        // 2. Читаем ini-файл
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            // как и раньше: если файл не открылся —
            // просто остаётся поведение через toupper
            initialized = true;
            return;
        }

        std::string s1, s2;
        std::getline(file, s1);
        std::getline(file, s2);
        file.close();

        // Старый код брал ровно первые 33 символа
        const size_t n = std::min<size_t>(33, std::min(s1.size(), s2.size()));

        // 3. Вычисляем firstOEM как mapChar.begin()->first,
        //    то есть МИНИМАЛЬНЫЙ ключ среди вставленных s1[i]
        if (n > 0) {
            char minKey = s1[0];
            for (size_t i = 1; i < n; ++i) {
                if (s1[i] < minKey)
                    minKey = s1[i];
            }
            firstOEM = minKey;
        } else {
            firstOEM = 0;
        }

        // 4. Поверх toupper накатываем OEM соответствия s1[i] -> s2[i],
        //    точный аналог mapChar.insert({s1[i], s2[i]})
        for (size_t i = 0; i < n; ++i) {
            unsigned char from = static_cast<unsigned char>(s1[i]);
            unsigned char to   = static_cast<unsigned char>(s2[i]);
            table[from] = to;
        }

        initialized = true;
    }


