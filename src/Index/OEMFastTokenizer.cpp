#include "OEMFastTokenizer.h"
#include <cctype>
#include <cstring>

// ------------------------------------------------------------
// 1. Таблица wordChar — ускоряет разбор слов на 2–3 раза
// ------------------------------------------------------------
bool OEMFastTokenizer::isWordChar[256];

void OEMFastTokenizer::initTables()
{
    for (bool & i : isWordChar)
        i = false;

    // Цифры
    for (int c = '0'; c <= '9'; ++c) isWordChar[c] = true;

    // Английские буквы
    for (int c = 'A'; c <= 'Z'; ++c) isWordChar[c] = true;
    for (int c = 'a'; c <= 'z'; ++c) isWordChar[c] = true;

    // Русские OEM866 символы (128..255)
    for (int c = 128; c < 256; ++c)
        isWordChar[c] = true;
}


// ------------------------------------------------------------
// 2. Быстрая нормализация токена:
//    - UPPER через твою OEM-таблицу
//    - TRIM аналогичен твоему boost::trim_if
// ------------------------------------------------------------
void OEMFastTokenizer::normalizeToken(std::string& s)
{
    if (s.empty())
        return;

    auto* p = (unsigned char*)s.data();
    size_t n = s.size();

    // Upper-case OEM
    for (size_t i = 0; i < n; ++i)
        p[i] = OEMtoUpper::getUpperCharOem(p[i]);

    // Trim left
    size_t start = 0;
    while (start < n)
    {
        unsigned char c = p[start];
        if (!(OEMtoUpper::iS_not_a_Oem(c) && ispunct(c)))
            break;
        start++;
    }

    // Trim right
    size_t end = n;
    while (end > start)
    {
        unsigned char c = p[end - 1];
        if (!(OEMtoUpper::iS_not_a_Oem(c) && ispunct(c)))
            break;
        end--;
    }

    if (start != 0 || end != n)
        s = s.substr(start, end - start);
}


// ------------------------------------------------------------
// 3. Основной быстрый OEM tokenizer, без аллокаций
//
//    data   – указатель на буфер OEM866
//    len    – длина буфера
//    carry  – хвост незавершённого токена (если слово разделено между чанками)
//    callback(std::string_view token) – вызывается для каждого токена
// ------------------------------------------------------------
void OEMFastTokenizer::tokenizeBuffer(
        const char* data,
        size_t len,
        std::string& carry,
        const std::function<void(std::string_view)>& callback)
{
    const auto* p = (const unsigned char*)data;

    size_t start = 0;
    bool inWord = false;

    for (size_t i = 0; i < len; ++i)
    {
        unsigned char c = p[i];

        if (isWordChar[c])
        {
            if (!inWord)
            {
                inWord = true;
                start = i;
            }
        }
        else
        {
            if (inWord)
            {
                // токен в пределах одного чанка
                if (carry.empty())
                {
                    callback(std::string_view((const char*)p + start, i - start));
                }
                else
                {
                    // продолжаем carry
                    carry.append((const char*)p + start, i - start);
                    callback(std::string_view(carry));
                    carry.clear();
                }

                inWord = false;
            }
        }
    }

    // Хвост — переносим в carry
    if (inWord)
    {
        carry.append((const char*)p + start, len - start);
    }
}
