#pragma once
#include <string>



    class OEMtoUpper {
    public:
        static unsigned char table[256];
        static bool initialized;
        static char firstOEM;  // тот самый "a_oem" из mapChar.begin()->first

        // Инициализация из OEM866.INI
        static void init(const std::string& path);

        // Полный аналог старого getUpperCharOem
        static inline char getUpperCharOem(char c) {
            return static_cast<char>(table[static_cast<unsigned char>(c)]);
        }

        // Полный аналог старого iS_not_a_Oem (c != a_oem)
        static inline bool iS_not_a_Oem(char c) {
            return c != firstOEM;
        }
    };


