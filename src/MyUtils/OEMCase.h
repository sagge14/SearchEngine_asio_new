#pragma once
#include <string>

/// Единый быстрый класс для OEM866 регистровых преобразований.
/// Использует два статических массива table[256] вместо std::map.
/// Заменяет старые OEMtoUpper и OEMtoCase.
class OEMCase {
public:
    static unsigned char upperTable[256];
    static unsigned char lowerTable[256];
    static bool initialized;
    static char firstOEM;

    /// Загрузка таблицы из OEM866.INI
    /// Строка 1 = строчные, Строка 2 = прописные
    static void init(const std::string& path);

    // ── Быстрые посимвольные операции (lookup по таблице) ─────

    static inline char toUpper(char c) {
        return static_cast<char>(upperTable[static_cast<unsigned char>(c)]);
    }

    static inline char toLower(char c) {
        return static_cast<char>(lowerTable[static_cast<unsigned char>(c)]);
    }

    // ── Строковые операции ───────────────────────────────────

    static std::string toUpper(const std::string& s) {
        std::string out = s;
        for (auto& ch : out) ch = toUpper(ch);
        return out;
    }

    static std::string toLower(const std::string& s) {
        std::string out = s;
        for (auto& ch : out) ch = toLower(ch);
        return out;
    }

    // ── Обратная совместимость с OEMtoUpper API ──────────────

    static inline char getUpperCharOem(char c) { return toUpper(c); }
    static inline bool iS_not_a_Oem(char c)    { return c != firstOEM; }
};
