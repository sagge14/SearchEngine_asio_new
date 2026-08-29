#include "Commands/CommandResult.h"
#include "Commands/GetFile/GetFileCmd.h"
#include "Commands/GetTelegaWay/GetTelegaWayCmd.h"
#include "Commands/GetTelegaWay/TelegaWay.h"
#include "MyUtils/Utf8Path.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using command_execution::ErrorCode;

    class TemporaryDirectory final
    {
    public:
        TemporaryDirectory()
        {
            static std::atomic_uint64_t sequence{0};
            const auto uniqueValue =
                std::chrono::steady_clock::now().time_since_epoch().count() +
                static_cast<std::int64_t>(
                    sequence.fetch_add(1, std::memory_order_relaxed));
            path_ = fs::temp_directory_path() /
                ("searchengine-production-roots-" + std::to_string(uniqueValue));
            fs::create_directories(path_);
        }

        ~TemporaryDirectory()
        {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
        }

        [[nodiscard]] const fs::path& path() const noexcept { return path_; }

    private:
        fs::path path_;
    };

    std::vector<std::uint8_t> bytesOf(const std::string& text)
    {
        return {text.begin(), text.end()};
    }
}

TEST(ProductionFilesystemRootsTest, JoinHelperBuildsF12AndOpisContracts)
{
    const std::string f12 = "E:\\custom-f12";
    const std::string opis = "F:\\custom-opis";
    EXPECT_EQ(
        encoding::utf8_path_join(f12, "2099.db"),
        encoding::path_to_utf8(encoding::utf8_to_path(f12) / L"2099.db"));
    EXPECT_EQ(
        encoding::utf8_path_join(f12, "base.db"),
        encoding::path_to_utf8(encoding::utf8_to_path(f12) / L"base.db"));
    EXPECT_EQ(
        encoding::utf8_path_join(opis, "2099.db"),
        encoding::path_to_utf8(encoding::utf8_to_path(opis) / L"2099.db"));
    EXPECT_EQ(
        encoding::utf8_path_join(opis, "2099.DB"),
        encoding::path_to_utf8(encoding::utf8_to_path(opis) / L"2099.DB"));
}

TEST(ProductionFilesystemRootsTest, GetOpisBaseUsesCustomYearDbPath)
{
    TemporaryDirectory temporary;
    const std::string customDir =
        encoding::path_to_utf8(temporary.path() / L"opis-custom");
    fs::create_directories(encoding::utf8_to_path(customDir));
    const auto filePath = encoding::utf8_to_path(
        encoding::utf8_path_join(customDir, "2099.db"));
    {
        std::ofstream output(filePath, std::ios::binary | std::ios::trunc);
        output << "opis-payload";
    }

    const auto result = GetFileCmd::downloadFileResultByPath(filePath);
    ASSERT_TRUE(result.succeeded()) << result.diagnostic;
    EXPECT_EQ(
        std::string(result.payload.begin(), result.payload.end()),
        "opis-payload");
}

TEST(ProductionFilesystemRootsTest, CustomF12MissingDbIsDataSourceUnavailable)
{
    TemporaryDirectory temporary;
    const std::string customDir =
        encoding::path_to_utf8(temporary.path() / L"f12-custom");
    const auto yearDb = encoding::utf8_path_join(customDir, "2099.db");
    const auto baseDb = encoding::utf8_path_join(customDir, "base.db");

    const auto previousWay = TelegaWay::base_way_dir;
    const auto previousBase = TelegaWay::base_f12_dir;
    const auto previousYear = TelegaWay::work_year;
    TelegaWay::base_way_dir = yearDb;
    TelegaWay::base_f12_dir = baseDb;
    TelegaWay::work_year = "2099";

    GetTelegaWayVhCmd command;
    const auto result = command.executeResult(bytesOf("100"));

    TelegaWay::base_way_dir = previousWay;
    TelegaWay::base_f12_dir = previousBase;
    TelegaWay::work_year = previousYear;

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.error, ErrorCode::DataSourceUnavailable);
    EXPECT_FALSE(fs::exists(encoding::utf8_to_path(yearDb)));
    EXPECT_FALSE(fs::exists(encoding::utf8_to_path(baseDb)));
}
