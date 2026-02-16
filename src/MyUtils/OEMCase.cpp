#include "OEMCase.h"
#include <fstream>
#include <algorithm>
#include <cctype>

unsigned char OEMCase::upperTable[256];
unsigned char OEMCase::lowerTable[256];
bool OEMCase::initialized = false;
char OEMCase::firstOEM = 0;

void OEMCase::init(const std::string& path)
{
    if (initialized)
        return;

    // 1. Базовые таблицы: стандартный toupper/tolower для ASCII
    for (int i = 0; i < 256; ++i) {
        upperTable[i] = static_cast<unsigned char>(
                std::toupper(static_cast<unsigned char>(i)));
        lowerTable[i] = static_cast<unsigned char>(
                std::tolower(static_cast<unsigned char>(i)));
    }

    // 2. Читаем OEM866.INI
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        initialized = true;
        return;
    }

    std::string lower_line, upper_line;
    std::getline(file, lower_line);
    std::getline(file, upper_line);
    file.close();

    // Убираем CR/LF
    auto trimCrlf = [](std::string& s) {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n'))
            s.pop_back();
    };
    trimCrlf(lower_line);
    trimCrlf(upper_line);

    const size_t n = std::min<size_t>(33, std::min(lower_line.size(), upper_line.size()));

    // 3. Вычисляем firstOEM — минимальный ключ среди строчных
    if (n > 0) {
        char minKey = lower_line[0];
        for (size_t i = 1; i < n; ++i)
            if (lower_line[i] < minKey)
                minKey = lower_line[i];
        firstOEM = minKey;
    }

    // 4. Накатываем OEM соответствия поверх toupper/tolower
    for (size_t i = 0; i < n; ++i) {
        auto low = static_cast<unsigned char>(lower_line[i]);
        auto up  = static_cast<unsigned char>(upper_line[i]);
        upperTable[low] = up;   // строчная → прописная
        lowerTable[up]  = low;  // прописная → строчная
    }

    initialized = true;
}
