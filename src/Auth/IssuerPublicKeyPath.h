#pragma once

#include <filesystem>

namespace auth
{
    // Primary: <auth-db-dir>/issuer-public.pem (exported by --export-public).
    // Fallback: %ProgramData%/SearchClientTokenIssuer/keys/public.pem when the
    // sibling file is absent but the issuer keystore public key exists.
    [[nodiscard]] std::filesystem::path ResolveIssuerPublicPemPath(
        const std::filesystem::path& auth_clients_db_path);

    [[nodiscard]] std::filesystem::path DefaultTokenIssuerPublicPemPath();
}
