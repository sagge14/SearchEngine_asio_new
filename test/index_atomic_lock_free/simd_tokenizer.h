// simd_tokenizer.h
#pragma once

#include <string>
#include <string_view>
#include <functional>

namespace simd_tokenizer
{
    /// Callback вызывается для каждого готового токена.
    /// ВАЖНО: string_view живёт только внутри вызова callback.
    using TokenCallback = std::function<void(std::string_view)>;

    /**
     * Разбивает буфер OEM866 на токены.
     *
     *  - Разделители: все "невидимые" символы: пробел, таб, \n, \r, \f, \v, '\0'
     *  - По краям токена отрезается вся пунктуация (std::ispunct, по C-локали)
     *  - Токены, разорванные на границе буферов, корректно склеиваются через carry
     *
     *  data        - указатель на буфер (часть файла)
     *  len         - длина буфера
     *  isLastChunk - true, если это последний кусок файла (на нём будет сброшен хвост)
     *  carry       - строка-хвост, которую ты создаёшь один раз на файл и передаёшь между вызовами
     *  cb          - callback на каждый токен
     */
    void tokenize_oem866_buffer(const char* data,
                                std::size_t len,
                                bool isLastChunk,
                                std::string& carry,
                                const TokenCallback& cb);
}
