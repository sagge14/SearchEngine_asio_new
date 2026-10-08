#include "ComputerTokenPath.hpp"

#include <cstdlib>

namespace token_issuer {

std::optional<std::filesystem::path> LocalAppDataRoot()
{
    wchar_t* local_app_data = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&local_app_data, &length, L"LOCALAPPDATA") != 0 ||
        local_app_data == nullptr)
    {
        return std::nullopt;
    }

    std::filesystem::path root(local_app_data);
    free(local_app_data);

    if (root.empty() || !root.is_absolute()) {
        return std::nullopt;
    }
    return root;
}

std::optional<std::filesystem::path> StandardComputerTokenDirectory()
{
    const auto local_app_data = LocalAppDataRoot();
    if (!local_app_data) {
        return std::nullopt;
    }
    return *local_app_data / L"SearchEngine";
}

std::optional<std::filesystem::path> StandardComputerTokenPath()
{
    const auto directory = StandardComputerTokenDirectory();
    if (!directory) {
        return std::nullopt;
    }
    return *directory / kTokenFileName;
}

bool EnsureComputerTokenDirectory(
    const std::filesystem::path& directory,
    std::string* error)
{
    if (directory.empty()) {
        if (error != nullptr) {
            *error = "token directory path is empty";
        }
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::create_directories(directory, ec)) {
        if (ec) {
            if (error != nullptr) {
                *error =
                    "cannot create token directory: " + directory.string() +
                    " (" + ec.message() + ")";
            }
            return false;
        }
        if (!std::filesystem::is_directory(directory, ec) || ec) {
            if (error != nullptr) {
                *error =
                    "token directory is not available: " + directory.string();
            }
            return false;
        }
    }
    return true;
}

} // namespace token_issuer
