#include "Auth/IssuerPublicKeyPath.h"

#include <cstdlib>

namespace auth
{
namespace
{
    std::filesystem::path ProgramDataRoot()
    {
        wchar_t* program_data = nullptr;
        std::size_t length = 0;
        if (_wdupenv_s(&program_data, &length, L"ProgramData") == 0 &&
            program_data != nullptr)
        {
            std::filesystem::path root(program_data);
            free(program_data);
            return root;
        }
        return {};
    }
} // namespace

std::filesystem::path DefaultTokenIssuerPublicPemPath()
{
    const auto program_data = ProgramDataRoot();
    if (program_data.empty()) {
        return std::filesystem::path("SearchClientTokenIssuer") /
            "keys" / "public.pem";
    }
    return program_data / "SearchClientTokenIssuer" / "keys" / "public.pem";
}

std::filesystem::path ResolveIssuerPublicPemPath(
    const std::filesystem::path& auth_clients_db_path)
{
    const auto beside =
        auth_clients_db_path.parent_path() / "issuer-public.pem";
    if (std::filesystem::is_regular_file(beside)) {
        return beside;
    }

    const auto fallback = DefaultTokenIssuerPublicPemPath();
    if (std::filesystem::is_regular_file(fallback)) {
        return fallback;
    }

    return beside;
}

} // namespace auth
