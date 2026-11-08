#include "Auth/IssuerPublicKeyPath.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace
{
    void WriteDummyPem(const fs::path& path)
    {
        std::ofstream out(path, std::ios::binary);
        out << "dummy-public-pem\n";
    }
} // namespace

TEST(IssuerPublicKeyPathTest, PrefersSiblingIssuerPublicPem)
{
    const fs::path temp =
        fs::temp_directory_path() /
        ("issuer-key-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path data_dir = temp / "data";
    fs::create_directories(data_dir);

    const fs::path db = data_dir / "auth_clients.sqlite";
    std::ofstream(db).put('x');

    const fs::path beside = data_dir / "issuer-public.pem";
    WriteDummyPem(beside);

    EXPECT_EQ(auth::ResolveIssuerPublicPemPath(db), beside);

    fs::remove_all(temp);
}

TEST(IssuerPublicKeyPathTest, FallsBackToProgramDataKeystorePublicPem)
{
    const fs::path temp =
        fs::temp_directory_path() /
        ("issuer-key-fallback-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path fake_program_data = temp / "ProgramData";
    const fs::path keys_dir =
        fake_program_data / "SearchClientTokenIssuer" / "keys";
    fs::create_directories(keys_dir);

    const fs::path fallback = keys_dir / "public.pem";
    WriteDummyPem(fallback);

    const fs::path data_dir = temp / "service-data";
    fs::create_directories(data_dir);
    const fs::path db = data_dir / "auth_clients.sqlite";
    std::ofstream(db).put('x');

    _wputenv_s(L"ProgramData", fake_program_data.wstring().c_str());

    EXPECT_EQ(auth::ResolveIssuerPublicPemPath(db), fallback);

    _wputenv_s(L"ProgramData", L"");
    fs::remove_all(temp);
}

TEST(IssuerPublicKeyPathTest, ReturnsSiblingPathWhenNeitherExists)
{
    const fs::path temp =
        fs::temp_directory_path() /
        ("issuer-key-missing-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path data_dir = temp / "data";
    fs::create_directories(data_dir);

    const fs::path db = data_dir / "auth_clients.sqlite";
    std::ofstream(db).put('x');

    EXPECT_EQ(
        auth::ResolveIssuerPublicPemPath(db),
        data_dir / "issuer-public.pem");

    fs::remove_all(temp);
}
