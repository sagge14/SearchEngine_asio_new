#include "CryptoStub.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace token_issuer {
namespace {

// Minimal base64 (stub storage only; not cryptographic).
constexpr char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(std::string_view input)
{
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 2 < input.size()) {
        const unsigned triple =
            (static_cast<unsigned>(static_cast<unsigned char>(input[i])) << 16) |
            (static_cast<unsigned>(static_cast<unsigned char>(input[i + 1])) << 8) |
            static_cast<unsigned>(static_cast<unsigned char>(input[i + 2]));
        out.push_back(kB64[(triple >> 18) & 63]);
        out.push_back(kB64[(triple >> 12) & 63]);
        out.push_back(kB64[(triple >> 6) & 63]);
        out.push_back(kB64[triple & 63]);
        i += 3;
    }
    if (i < input.size()) {
        unsigned triple =
            static_cast<unsigned>(static_cast<unsigned char>(input[i])) << 16;
        out.push_back(kB64[(triple >> 18) & 63]);
        if (i + 1 < input.size()) {
            triple |= static_cast<unsigned>(
                          static_cast<unsigned char>(input[i + 1]))
                << 8;
            out.push_back(kB64[(triple >> 12) & 63]);
            out.push_back(kB64[(triple >> 6) & 63]);
            out.push_back('=');
        } else {
            out.push_back(kB64[(triple >> 12) & 63]);
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

int B64Value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

std::string Base64Decode(std::string_view input)
{
    std::string out;
    int val = 0;
    int valb = -8;
    for (const char c : input) {
        if (c == '=') {
            break;
        }
        const int d = B64Value(c);
        if (d < 0) {
            continue;
        }
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

void WriteTextFile(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot write: " + path.string());
    }
    output << text;
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read: " + path.string());
    }
    return std::string(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
}

} // namespace

KeystorePaths ResolveKeystorePaths(const std::filesystem::path& root)
{
    KeystorePaths paths;
    paths.root = root;
    paths.public_key = root / "public.stub.pem";
    paths.private_enc = root / "private.stub.enc";
    paths.meta = root / "keystore.meta.json";
    return paths;
}

bool KeystoreExists(const KeystorePaths& paths)
{
    return std::filesystem::is_regular_file(paths.private_enc) &&
        std::filesystem::is_regular_file(paths.public_key);
}

std::filesystem::path DefaultKeystoreRoot()
{
    wchar_t* program_data = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&program_data, &length, L"ProgramData") == 0 &&
        program_data != nullptr)
    {
        std::filesystem::path root(program_data);
        free(program_data);
        return root / "SearchClientTokenIssuer" / "keys";
    }
    return std::filesystem::path("keys");
}

void ProtectPrivateKey(
    const KeystorePaths& paths,
    std::string_view password,
    std::string_view private_plaintext)
{
    if (password.empty()) {
        throw std::runtime_error("password must not be empty");
    }
    // stub-v1: base64(password + '\n' + private). Not real encryption.
    std::string payload;
    payload.reserve(password.size() + 1 + private_plaintext.size());
    payload.append(password);
    payload.push_back('\n');
    payload.append(private_plaintext);

    const std::string blob = std::string("stub-v1:") + Base64Encode(payload);
    WriteTextFile(paths.private_enc, blob);

    const auto raw_path = paths.root / "private.stub.raw";
    std::error_code ec;
    std::filesystem::remove(raw_path, ec);
}

void GenerateKeyPair(const KeystorePaths& paths, std::string_view password)
{
    std::error_code ec;
    std::filesystem::create_directories(paths.root, ec);
    if (ec) {
        throw std::runtime_error(
            "cannot create keystore directory: " + paths.root.string());
    }

    constexpr std::string_view kPublic = "STUB-PUBLIC-KEY-v1\n";
    constexpr std::string_view kPrivate = "STUB-PRIVATE-KEY-v1\n";
    WriteTextFile(paths.public_key, kPublic);
    ProtectPrivateKey(paths, password, kPrivate);

    nlohmann::json meta = {
        {"version", 1},
        {"alg", "stub-v1"},
        {"note", "crypto is stub; production RSA later"},
    };
    WriteTextFile(paths.meta, meta.dump(2));
}

std::string UnlockPrivateKey(
    const KeystorePaths& paths,
    std::string_view password)
{
    const std::string blob = ReadTextFile(paths.private_enc);
    constexpr std::string_view kPrefix = "stub-v1:";
    if (blob.size() < kPrefix.size() ||
        blob.compare(0, kPrefix.size(), kPrefix) != 0)
    {
        throw std::runtime_error("unsupported private key blob (expected stub-v1)");
    }

    const std::string decoded = Base64Decode(
        std::string_view(blob).substr(kPrefix.size()));
    const auto sep = decoded.find('\n');
    if (sep == std::string::npos) {
        throw std::runtime_error("corrupt stub private key blob");
    }
    const std::string stored_password = decoded.substr(0, sep);
    if (stored_password != password) {
        throw std::runtime_error("incorrect keystore password");
    }
    return decoded.substr(sep + 1);
}

StubSignature SignTokenPayload(std::string_view /*canonical_payload_utf8*/)
{
    // Stage 1: keep wire-compatible alg=none; do not emit a real signature.
    StubSignature signature;
    signature.alg = "none";
    signature.encoding = "base64";
    signature.value.clear();
    return signature;
}

bool VerifyTokenSignature(
    std::string_view alg,
    std::string_view /*signature_value*/,
    std::string_view /*canonical_payload_utf8*/)
{
    return alg.empty() || alg == "none";
}

} // namespace token_issuer
