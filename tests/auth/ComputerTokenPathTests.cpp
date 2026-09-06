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
class ScopedLocalAppDataEnv
{
public:
    explicit ScopedLocalAppDataEnv(std::optional<std::wstring> new_value)
    {
        wchar_t* current = nullptr;
        std::size_t length = 0;
        if (_wdupenv_s(&current, &length, L"LOCALAPPDATA") == 0 &&
            current != nullptr)
        {
            had_value_ = true;
            previous_value_ = current;
            free(current);
        }

        if (new_value) {
            _wputenv_s(L"LOCALAPPDATA", new_value->c_str());
        } else {
            _wputenv_s(L"LOCALAPPDATA", L"");
        }
    }

    ~ScopedLocalAppDataEnv()
    {
        if (had_value_) {
            _wputenv_s(L"LOCALAPPDATA", previous_value_.c_str());
        } else {
            _wputenv_s(L"LOCALAPPDATA", L"");
        }
    }

    ScopedLocalAppDataEnv(const ScopedLocalAppDataEnv&) = delete;
    ScopedLocalAppDataEnv& operator=(const ScopedLocalAppDataEnv&) = delete;

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

TEST(ComputerTokenPathTest, StandardPathUsesLocalAppDataWhenAvailable)
{
    const fs::path temp = UniqueTempRoot("computer-token-path-a");
    fs::create_directories(temp);

    ScopedLocalAppDataEnv env(temp.wstring());
    const auto path = token_issuer::StandardComputerTokenPath();

    ASSERT_TRUE(path.has_value());
    EXPECT_EQ(
        *path,
        temp / L"SearchEngine" / token_issuer::kTokenFileName);

    fs::remove_all(temp);
}

TEST(ComputerTokenPathTest, StandardPathUnavailableWithoutLocalAppData)
{
    ScopedLocalAppDataEnv env(std::nullopt);

    EXPECT_FALSE(token_issuer::LocalAppDataRoot().has_value());
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

TEST(ComputerTokenPathTest, RejectsRelativeUserProfile)
{
    ScopedLocalAppDataEnv env(L"relative-profile");
    EXPECT_FALSE(token_issuer::StandardComputerTokenPath().has_value());
}

TEST(ComputerTokenPathTest, DifferentUserProfilesHaveSeparatePaths)
{
    const auto first = UniqueTempRoot("user-a") / L"\u041F";
    const auto second = UniqueTempRoot("user-b");
    ScopedLocalAppDataEnv env(first.wstring());
    const auto first_path = token_issuer::StandardComputerTokenPath();
    ASSERT_TRUE(first_path.has_value());
    EXPECT_EQ(*first_path, first / L"SearchEngine" / token_issuer::kTokenFileName);
    {
        ScopedLocalAppDataEnv other(second.wstring());
        const auto second_path = token_issuer::StandardComputerTokenPath();
        ASSERT_TRUE(second_path.has_value());
        EXPECT_NE(first_path, second_path);
        EXPECT_EQ(*second_path, second / L"SearchEngine" / token_issuer::kTokenFileName);
    }
    EXPECT_EQ(first_path, token_issuer::StandardComputerTokenPath());
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
