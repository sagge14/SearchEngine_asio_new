// simd_tokenizer.cpp
#include "simd_tokenizer.h"

#include <cstddef>
#include <cstdint>
#include <cctype>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace simd_tokenizer
{
    // --- Общие вспомогательные функции --------------------------------------

    // "Невидимый" символ-разделитель: пробел/таб/переводы строк/управляющие
    static inline bool is_delim_char_scalar(unsigned char c)
    {
        switch (c)
        {
            case ' ':   // space
            case '\t':  // tab
            case '\n':  // LF
            case '\r':  // CR
            case '\f':  // form feed
            case '\v':  // vertical tab
            case '\0':  // safety
                return true;
            default:
                return false;
        }
    }

    // Обрезаем пунктуацию по краям: вся std::ispunct по C-локали
    static inline std::string_view trim_punct(std::string_view sv)
    {
        std::size_t b = 0;
        std::size_t e = sv.size();

        while (b < e && std::ispunct(static_cast<unsigned char>(sv[b])))
            ++b;
        while (e > b && std::ispunct(static_cast<unsigned char>(sv[e - 1])))
            --e;

        return sv.substr(b, e - b);
    }

    // Вызываем callback, если токен после тримминга не пустой
    template<typename View>
    static inline void emit_token_if_not_empty(View view, const TokenCallback& cb)
    {
        std::string_view cleaned = trim_punct(
                std::string_view(view.data(), view.size())
        );
        if (!cleaned.empty())
            cb(cleaned);
    }

    // --- Скалярная реализация (fallback и хвост обработки) -------------------

    static void tokenize_scalar(const char* data,
                                std::size_t len,
                                bool isLastChunk,
                                std::string& carry,
                                const TokenCallback& cb)
    {
        bool in_token = !carry.empty();
        std::size_t token_start = 0; // индекс в data для текущего токена (если он внутри буфера)

        for (std::size_t i = 0; i < len; ++i)
        {
            unsigned char c = static_cast<unsigned char>(data[i]);
            bool delim = is_delim_char_scalar(c);

            if (delim)
            {
                if (in_token)
                {
                    // Токен заканчивается на i (не включая)
                    if (!carry.empty())
                    {
                        // Токен начался в предыдущем буфере
                        carry.append(data + token_start, i - token_start);
                        emit_token_if_not_empty(
                                std::string_view(carry.data(), carry.size()),
                                cb
                        );
                        carry.clear();
                    }
                    else
                    {
                        // Токен полностью внутри текущего буфера
                        emit_token_if_not_empty(
                                std::string_view(data + token_start, i - token_start),
                                cb
                        );
                    }
                    in_token = false;
                }
            }
            else
            {
                if (!in_token)
                {
                    // Начало нового токена в буфере
                    token_start = i;
                    in_token = true;
                }
            }
        }

        // Конец буфера
        if (in_token)
        {
            // Токен продолжается в следующий буфер
            if (!carry.empty())
            {
                // Продолжаем существующий хвост
                carry.append(data + token_start, len - token_start);
            }
            else
            {
                // Новый хвост, начавшийся в этом буфере
                carry.assign(data + token_start, len - token_start);
            }
        }

        // Если это последний кусок — сбрасываем хвост как готовый токен
        if (isLastChunk && !carry.empty())
        {
            emit_token_if_not_empty(
                    std::string_view(carry.data(), carry.size()),
                    cb
            );
            carry.clear();
        }
    }

    // --- AVX2 реализация -----------------------------------------------------

#if defined(__AVX2__)

    static void tokenize_avx2(const char* data,
                              std::size_t len,
                              bool isLastChunk,
                              std::string& carry,
                              const TokenCallback& cb)
    {
        bool in_token = !carry.empty();
        std::size_t token_start = 0;

        // Константы для сравнения
        const __m256i c_space = _mm256_set1_epi8(' ');
        const __m256i c_tab   = _mm256_set1_epi8('\t');
        const __m256i c_nl    = _mm256_set1_epi8('\n');
        const __m256i c_cr    = _mm256_set1_epi8('\r');
        const __m256i c_ff    = _mm256_set1_epi8('\f');
        const __m256i c_vt    = _mm256_set1_epi8('\v');
        const __m256i c_zero  = _mm256_set1_epi8('\0');

        std::size_t i = 0;

        // Обрабатываем блоками по 32 байта
        while (i + 32 <= len)
        {
            __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));

            __m256i eq_space = _mm256_cmpeq_epi8(chunk, c_space);
            __m256i eq_tab   = _mm256_cmpeq_epi8(chunk, c_tab);
            __m256i eq_nl    = _mm256_cmpeq_epi8(chunk, c_nl);
            __m256i eq_cr    = _mm256_cmpeq_epi8(chunk, c_cr);
            __m256i eq_ff    = _mm256_cmpeq_epi8(chunk, c_ff);
            __m256i eq_vt    = _mm256_cmpeq_epi8(chunk, c_vt);
            __m256i eq_zero  = _mm256_cmpeq_epi8(chunk, c_zero);

            __m256i delims1 = _mm256_or_si256(eq_space, eq_tab);
            __m256i delims2 = _mm256_or_si256(eq_nl, eq_cr);
            __m256i delims3 = _mm256_or_si256(eq_ff, eq_vt);
            __m256i delims4 = _mm256_or_si256(eq_zero, delims1);

            __m256i delims12 = _mm256_or_si256(delims2, delims3);
            __m256i is_delim = _mm256_or_si256(delims4, delims12);

            uint32_t mask = static_cast<uint32_t>(_mm256_movemask_epi8(is_delim));

            for (int b = 0; b < 32; ++b)
            {
                std::size_t pos = i + static_cast<std::size_t>(b);
                bool delim = (mask >> b) & 1;

                if (delim)
                {
                    if (in_token)
                    {
                        if (!carry.empty())
                        {
                            carry.append(data + token_start, pos - token_start);
                            emit_token_if_not_empty(
                                std::string_view(carry.data(), carry.size()),
                                cb
                            );
                            carry.clear();
                        }
                        else
                        {
                            emit_token_if_not_empty(
                                std::string_view(data + token_start, pos - token_start),
                                cb
                            );
                        }
                        in_token = false;
                    }
                }
                else
                {
                    if (!in_token)
                    {
                        token_start = pos;
                        in_token = true;
                    }
                }
            }

            i += 32;
        }

        // "хвост" буфера — скалярно, но с той же логикой
        for (; i < len; ++i)
        {
            unsigned char c = static_cast<unsigned char>(data[i]);
            bool delim = is_delim_char_scalar(c);

            if (delim)
            {
                if (in_token)
                {
                    if (!carry.empty())
                    {
                        carry.append(data + token_start, i - token_start);
                        emit_token_if_not_empty(
                            std::string_view(carry.data(), carry.size()),
                            cb
                        );
                        carry.clear();
                    }
                    else
                    {
                        emit_token_if_not_empty(
                            std::string_view(data + token_start, i - token_start),
                            cb
                        );
                    }
                    in_token = false;
                }
            }
            else
            {
                if (!in_token)
                {
                    token_start = i;
                    in_token = true;
                }
            }
        }

        if (in_token)
        {
            if (!carry.empty())
            {
                carry.append(data + token_start, len - token_start);
            }
            else
            {
                carry.assign(data + token_start, len - token_start);
            }
        }

        if (isLastChunk && !carry.empty())
        {
            emit_token_if_not_empty(
                std::string_view(carry.data(), carry.size()),
                cb
            );
            carry.clear();
        }
    }

#endif // __AVX2__

    // --- Публичная функция ---------------------------------------------------

    void tokenize_oem866_buffer(const char* data,
                                std::size_t len,
                                bool isLastChunk,
                                std::string& carry,
                                const TokenCallback& cb)
    {
#if defined(__AVX2__)
        tokenize_avx2(data, len, isLastChunk, carry, cb);
#else
        tokenize_scalar(data, len, isLastChunk, carry, cb);
#endif
    }

} // namespace simd_tokenizer
