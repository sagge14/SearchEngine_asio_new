#include "ComputerTokenPath.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace fs = std::filesystem;

namespace
{
class ScopedProgramDataEnv
{
public:
    explicit ScopedProgramDataEnv(std::optional<std::wstring> new_value)
    {
        wchar_t* current = nullptr;
        std::size_t length = 0;
        if (_wdupenv_s(&current, &length, L"ProgramData") == 0 &&
            current != nullptr)
        {
            had_value_ = true;
            previous_value_ = current;
            free(current);
        }

        if (new_value) {
            _wputenv_s(L"ProgramData", new_value->c_str());
        } else {
            _wputenv_s(L"ProgramData", L"");
        }
    }

    ~ScopedProgramDataEnv()
    {
        if (had_value_) {
            _wputenv_s(L"ProgramData", previous_value_.c_str());
        } else {
            _wputenv_s(L"ProgramData", L"");
        }
    }

    ScopedProgramDataEnv(const ScopedProgramDataEnv&) = delete;
    ScopedProgramDataEnv& operator=(const ScopedProgramDataEnv&) = delete;

private:
    bool had_value_ = false;
    std::wstring previous_value_;
};

fs::path UniqueTempRoot(const char* label)
{
    return fs::temp_directory_path() /
        (std::string(label) + "-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
}
} // namespace

TEST(ComputerTokenPathTest, StandardPathUsesProgramDataWhenAvailable)
{
    const fs::path temp = UniqueTempRoot("computer-token-path-a");
    fs::create_directories(temp);

    ScopedProgramDataEnv env(temp.wstring());
    const auto path = token_issuer::StandardComputerTokenPath();

    ASSERT_TRUE(path.has_value());
    EXPECT_EQ(
        *path,
        temp / L"SearchEngine" / token_issuer::kTokenFileName);

    fs::remove_all(temp);
}

TEST(ComputerTokenPathTest, StandardPathUnavailableWithoutProgramData)
{
    ScopedProgramDataEnv env(std::nullopt);

    EXPECT_FALSE(token_issuer::ProgramDataRoot().has_value());
    EXPECT_FALSE(token_issuer::StandardComputerTokenDirectory().has_value());
    EXPECT_FALSE(token_issuer::StandardComputerTokenPath().has_value());
}

TEST(ComputerTokenPathTest, EnsureDirectoryFailsWhenParentIsFile)
{
    const fs::path temp = UniqueTempRoot("computer-token-path-c");
    fs::create_directories(temp);

    const fs::path blocker = temp / "blocker";
    {
        std::ofstream out(blocker);
        out << "not-a-directory";
    }

    std::string error;
    const fs::path target = blocker / "SearchEngine";
    EXPECT_FALSE(
        token_issuer::EnsureComputerTokenDirectory(target, &error));
    EXPECT_FALSE(error.empty());

    fs::remove_all(temp);
}

TEST(ComputerTokenPathTest, EnsureDirectoryCreatesWritablePath)
{
    const fs::path temp = UniqueTempRoot("computer-token-path-d");
    fs::create_directories(temp);

    const fs::path directory = temp / "tokens" / "SearchEngine";
    std::string error;
    ASSERT_TRUE(token_issuer::EnsureComputerTokenDirectory(directory, &error));
    EXPECT_TRUE(fs::is_directory(directory));

    const fs::path token_path = directory / token_issuer::kTokenFileName;
    {
        std::ofstream out(token_path);
        out << "{}";
    }
    EXPECT_TRUE(fs::is_regular_file(token_path));
    EXPECT_EQ(token_path.filename().wstring(), L"searchclient-auth-token.json");

    fs::remove_all(temp);
}
