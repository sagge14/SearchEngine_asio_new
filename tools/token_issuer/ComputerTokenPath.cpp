#include "ComputerTokenPath.hpp"

#include <cstdlib>

namespace token_issuer {

std::optional<std::filesystem::path> ProgramDataRoot()
{
    wchar_t* program_data = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&program_data, &length, L"ProgramData") != 0 ||
        program_data == nullptr)
    {
        return std::nullopt;
    }

    std::filesystem::path root(program_data);
    free(program_data);

    if (root.empty()) {
        return std::nullopt;
    }
    return root;
}

std::optional<std::filesystem::path> StandardComputerTokenDirectory()
{
    const auto program_data = ProgramDataRoot();
    if (!program_data) {
        return std::nullopt;
    }
    return *program_data / L"SearchEngine";
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
