#include "JSON/ConverterJSON.h"
#include "MyUtils/LogFile.h"
#include "SearchServer/SearchServer.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
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
                ("se-settings-block2-" + std::to_string(uniqueValue));
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

    void writeUtf8(const fs::path& path, const nh::json& root)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << root.dump(2);
    }

    nh::json minimalConfig()
    {
        return nh::json{
            {"config",
             {
                 {"year", "2026"},
                 {"extensions", nh::json::array({"txt"})},
                 {"prm_base_dir", ""},
                 {"prd_base_dir", ""},
                 {"asio_port", 15001},
                 {"thread_count", 4},
             }}
        };
    }
}

TEST(SettingsBlock2IndexRootsTest, LegacyDirsMapsToIndexRoots)
{
    TemporaryDirectory temporary;
    const fs::path settingsPath = temporary.path() / "Settings.json";
    auto root = minimalConfig();
    root["config"]["dirs"] = nh::json::array({"E:\\CUSTOM"});
    writeUtf8(settingsPath, root);

    const auto loaded = ConverterJSON::getSettings(settingsPath.string());
    ASSERT_EQ(loaded.indexRoots.size(), 1u);
    EXPECT_EQ(loaded.indexRoots.front(), "E:\\CUSTOM");
}

TEST(SettingsBlock2IndexRootsTest, CanonicalIndexRootsWinsOverLegacyDirs)
{
    TemporaryDirectory temporary;
    const fs::path settingsPath = temporary.path() / "Settings.json";
    auto root = minimalConfig();
    root["config"]["dirs"] = nh::json::array({"D:\\OLD"});
    root["config"]["index_roots"] = nh::json::array({"D:\\NEW"});
    writeUtf8(settingsPath, root);

    const auto loaded = ConverterJSON::getSettings(settingsPath.string());
    ASSERT_EQ(loaded.indexRoots.size(), 1u);
    EXPECT_EQ(loaded.indexRoots.front(), "D:\\NEW");
}

TEST(SettingsBlock2IndexRootsTest, WriterEmitsCanonicalIndexRootsOnly)
{
    TemporaryDirectory temporary;
    const fs::path settingsPath = temporary.path() / "Settings.json";
    auto root = minimalConfig();
    root["config"]["dirs"] = nh::json::array({"E:\\CUSTOM"});
    writeUtf8(settingsPath, root);

    (void)ConverterJSON::getSettings(settingsPath.string());

    std::ifstream input(settingsPath, std::ios::binary);
    const auto saved = nh::json::parse(input);
    ASSERT_TRUE(saved.contains("config"));
    EXPECT_TRUE(saved["config"].contains("index_roots"));
    EXPECT_FALSE(saved["config"].contains("dirs"));
    EXPECT_EQ(
        saved["config"].at("index_roots").at(0).get<std::string>(),
        "E:\\CUSTOM");
}

TEST(SettingsBlock2IndexRootsTest, LegacyExcludeDirsMapsToExcludedSubtrees)
{
    TemporaryDirectory temporary;
    const fs::path settingsPath = temporary.path() / "Settings.json";
    auto root = minimalConfig();
    root["config"]["index_roots"] = nh::json::array({"D:\\DATA"});
    root["config"]["exclude_dirs"] = nh::json::array({"D:\\DATA\\TEMP"});
    writeUtf8(settingsPath, root);

    const auto loaded = ConverterJSON::getSettings(settingsPath.string());
    ASSERT_EQ(loaded.excludedSubtrees.size(), 1u);
    EXPECT_EQ(loaded.excludedSubtrees.front(), "D:\\DATA\\TEMP");
}

TEST(SettingsBlock2IndexRootsTest, WriterEmitsExcludedSubtreesOnly)
{
    TemporaryDirectory temporary;
    const fs::path settingsPath = temporary.path() / "Settings.json";
    search_server::Settings settings;
    settings.year = "2026";
    settings.indexRoots = {"D:\\DATA"};
    settings.extensions = {"txt"};
    settings.prm_base_dir = "";
    settings.prd_base_dir = "";
    settings.excludedSubtrees = {"D:\\DATA\\TEMP"};

    ConverterJSON::setSettings(settings, settingsPath.string());

    std::ifstream input(settingsPath, std::ios::binary);
    const auto saved = nh::json::parse(input);
    ASSERT_TRUE(saved.contains("config"));
    EXPECT_TRUE(saved["config"].contains("excluded_subtrees"));
    EXPECT_FALSE(saved["config"].contains("exclude_dirs"));
}

TEST(SettingsBlock2IndexRootsTest, CanonicalExcludedSubtreesWinsOverLegacyExcludeDirs)
{
    TemporaryDirectory temporary;
    const fs::path settingsPath = temporary.path() / "Settings.json";
    auto root = minimalConfig();
    root["config"]["index_roots"] = nh::json::array({"D:\\DATA"});
    root["config"]["exclude_dirs"] = nh::json::array({"D:\\OLD\\TEMP"});
    root["config"]["excluded_subtrees"] = nh::json::array({"D:\\NEW\\TEMP"});
    writeUtf8(settingsPath, root);

    const auto loaded = ConverterJSON::getSettings(settingsPath.string());
    ASSERT_EQ(loaded.excludedSubtrees.size(), 1u);
    EXPECT_EQ(loaded.excludedSubtrees.front(), "D:\\NEW\\TEMP");
}
