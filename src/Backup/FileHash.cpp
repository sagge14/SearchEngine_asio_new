#include "FileHash.h"

#include "MyUtils/Encoding.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace {

std::uint32_t rotateRight32(std::uint32_t value, unsigned bits)
{
    const unsigned shift = bits & 31u;
    if (shift == 0u) {
        return value;
    }
    return (value >> shift) | (value << (32u - shift));
}

constexpr std::array<std::uint32_t, 64> round_constants{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

class Sha256 {
public:
    void update(const unsigned char* data, size_t size)
    {
        total_bytes_ += size;
        while (size > 0) {
            const size_t available = block_.size() - block_size_;
            const size_t count = size < available ? size : available;
            std::copy_n(data, count, block_.begin() + block_size_);
            data += count;
            size -= count;
            block_size_ += count;
            if (block_size_ == block_.size()) {
                transform(block_.data());
                block_size_ = 0;
            }
        }
    }

    std::string finish()
    {
        const std::uint64_t bit_count = total_bytes_ * 8u;
        block_[block_size_++] = 0x80u;

        if (block_size_ > 56) {
            std::fill(block_.begin() + block_size_, block_.end(), 0u);
            transform(block_.data());
            block_size_ = 0;
        }
        std::fill(block_.begin() + block_size_, block_.begin() + 56, 0u);
        for (size_t index = 0; index < 8; ++index) {
            block_[63 - index] = static_cast<unsigned char>(
                bit_count >> (index * 8)
            );
        }
        transform(block_.data());

        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (const std::uint32_t value : state_) {
            output << std::setw(8) << value;
        }
        return output.str();
    }

private:
    void transform(const unsigned char* block)
    {
        std::array<std::uint32_t, 64> words{};
        for (size_t index = 0; index < 16; ++index) {
            const size_t offset = index * 4;
            words[index] =
                (static_cast<std::uint32_t>(block[offset]) << 24) |
                (static_cast<std::uint32_t>(block[offset + 1]) << 16) |
                (static_cast<std::uint32_t>(block[offset + 2]) << 8) |
                static_cast<std::uint32_t>(block[offset + 3]);
        }
        for (size_t index = 16; index < words.size(); ++index) {
            const std::uint32_t s0 =
                rotateRight32(words[index - 15], 7) ^
                rotateRight32(words[index - 15], 18) ^
                (words[index - 15] >> 3);
            const std::uint32_t s1 =
                rotateRight32(words[index - 2], 17) ^
                rotateRight32(words[index - 2], 19) ^
                (words[index - 2] >> 10);
            words[index] =
                words[index - 16] + s0 + words[index - 7] + s1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];

        for (size_t index = 0; index < words.size(); ++index) {
            const std::uint32_t sum1 =
                rotateRight32(e, 6) ^ rotateRight32(e, 11) ^ rotateRight32(e, 25);
            const std::uint32_t choose = (e & f) ^ ((~e) & g);
            const std::uint32_t temporary1 =
                h + sum1 + choose + round_constants[index] + words[index];
            const std::uint32_t sum0 =
                rotateRight32(a, 2) ^ rotateRight32(a, 13) ^ rotateRight32(a, 22);
            const std::uint32_t majority =
                (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = sum0 + majority;

            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{
        0x6a09e667u,
        0xbb67ae85u,
        0x3c6ef372u,
        0xa54ff53au,
        0x510e527fu,
        0x9b05688cu,
        0x1f83d9abu,
        0x5be0cd19u
    };
    std::array<unsigned char, 64> block_{};
    size_t block_size_ = 0;
    std::uint64_t total_bytes_ = 0;
};

std::string pathToUtf8(const std::filesystem::path& path)
{
    return encoding::wstring_to_utf8(path.wstring());
}

} // namespace

FileHashResult sha256File(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return FileHashResult{
            false,
            {},
            0,
            "cannot open file \"" + pathToUtf8(path) + '"'
        };
    }

    Sha256 hash;
    std::uint64_t size = 0;
    std::array<unsigned char, 64 * 1024> buffer{};
    while (input) {
        input.read(
            reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size())
        );
        const std::streamsize count = input.gcount();
        if (count > 0) {
            hash.update(buffer.data(), static_cast<size_t>(count));
            size += static_cast<std::uint64_t>(count);
        }
    }
    if (input.bad()) {
        return FileHashResult{
            false,
            {},
            size,
            "I/O error while reading \"" + pathToUtf8(path) + '"'
        };
    }
    return FileHashResult{true, hash.finish(), size, {}};
}

std::string sha256String(const std::string& value)
{
    Sha256 hash;
    hash.update(
        reinterpret_cast<const unsigned char*>(value.data()),
        value.size()
    );
    return hash.finish();
}
