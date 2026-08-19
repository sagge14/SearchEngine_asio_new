#include "JSON/ConverterJSON.h"
#include "MyUtils/LogFile.h"
#include "SearchServer/SearchServer.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace
{
    namespace fs = std::filesystem;
    namespace nh = nlohmann;

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
                ("searchengine-settings-roots-" + std::to_string(uniqueValue));
            fs::create_directories(path_);
            LogFile::setLogsDirectory(path_ / "logs");
            ConverterJSON::setInteractiveErrors(false);
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

    std::string writeUtf8(const fs::path& path, const nh::json& root)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << root.dump(2);
        return path.string();
    }

    nh::json minimalConfig()
    {
        return nh::json{
            {"config",
             {
                 {"year", "2026"},
                 {"index_roots", nh::json::array({"D:\\JANUARY"})},
                 {"extensions", nh::json::array({"txt"})},
                 {"prm_base_dir", ""},
                 {"prd_base_dir", ""}
             }}
        };
    }
}

TEST(ProductionRootsSettingsTest, CustomValuesRoundtrip)
{
    TemporaryDirectory temporary;
    const fs::path settingsPath = temporary.path() / "Settings.json";
    search_server::Settings original;
    original.year = "2026";
    original.indexRoots = {"D:\\JANUARY"};
    original.extensions = {"txt"};
    original.prm_base_dir = "";
    original.prd_base_dir = "";
    original.tlg_send_root = "E:\\tlg-root";
    original.razn_output_dir = "E:\\OPIS ADMIN\\razn";
    original.opis_base_dir = "F:\\OPIS_ADMIN";
    original.f12_base_dir = "G:\\F12-custom";

    ConverterJSON::setSettings(original, settingsPath.string());
    const auto loaded = ConverterJSON::getSettings(settingsPath.string());
    EXPECT_EQ(loaded.tlg_send_root, original.tlg_send_root);
    EXPECT_EQ(loaded.razn_output_dir, original.razn_output_dir);
    EXPECT_EQ(loaded.opis_base_dir, original.opis_base_dir);
    EXPECT_EQ(loaded.f12_base_dir, original.f12_base_dir);
}

TEST(ProductionRootsSettingsTest, MissingFieldsUseProductionDefaultsAndResave)
{
    TemporaryDirectory temporary;
    const fs::path settingsPath = temporary.path() / "Settings.json";
    writeUtf8(settingsPath, minimalConfig());

    const auto loaded = ConverterJSON::getSettings(settingsPath.string());
    EXPECT_EQ(loaded.tlg_send_root, "D:\\");
    EXPECT_EQ(
        loaded.razn_output_dir,
        "D:\\OPIS_ADMIN\\РАЗНОСКА_ДЛЯ_ПРОСТАВЛЕНИЯ");
    EXPECT_EQ(loaded.opis_base_dir, "D:\\OPIS_ADMIN");
    EXPECT_EQ(loaded.f12_base_dir, "D:\\F12");

    std::ifstream input(settingsPath, std::ios::binary);
    const auto saved = nh::json::parse(input);
    ASSERT_TRUE(saved.contains("config"));
    EXPECT_EQ(saved["config"].at("tlg_send_root").get<std::string>(), "D:\\");
    EXPECT_EQ(
        saved["config"].at("razn_output_dir").get<std::string>(),
        "D:\\OPIS_ADMIN\\РАЗНОСКА_ДЛЯ_ПРОСТАВЛЕНИЯ");
    EXPECT_EQ(
        saved["config"].at("opis_base_dir").get<std::string>(),
        "D:\\OPIS_ADMIN");
    EXPECT_EQ(saved["config"].at("f12_base_dir").get<std::string>(), "D:\\F12");
}

TEST(ProductionRootsSettingsTest, WrongJsonTypeIsConfigError)
{
    TemporaryDirectory temporary;
    for (const char* name :
         {"tlg_send_root", "razn_output_dir", "opis_base_dir", "f12_base_dir"})
    {
        auto root = minimalConfig();
        root["config"][name] = 12;
        const fs::path settingsPath =
            temporary.path() / (std::string(name) + ".json");
        writeUtf8(settingsPath, root);
        EXPECT_THROW(
            ConverterJSON::getSettings(settingsPath.string()),
            std::invalid_argument)
            << name;
    }
}
