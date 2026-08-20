#include "JSON/ConverterJSON.h"
#include "MyUtils/LogFile.h"
#include "SearchServer/SearchServer.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
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

    std::string readAll(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    }

    nh::json optionalFields()
    {
        return nh::json{
            {"asio_port", 15001},
            {"thread_count", 4},
            {"max_response", 30},
            {"ind_time", 500},
            {"exact_search", false},
            {"tlg_send_root", "D:\\"},
            {"razn_output_dir", "D:\\OPIS_ADMIN\\РАЗНОСКА_ДЛЯ_ПРОСТАВЛЕНИЯ"},
            {"opis_base_dir", "D:\\OPIS_ADMIN"},
            {"f12_base_dir", "D:\\F12"},
            {"compact_threshold_percent", 5.0},
            {"scan_on_startup", true},
            {"max_parallel_readers", 0},
            {"file_indexing_timeout_sec", 60},
            {"enable_prm_short_content_autodetect", true},
            {"sqlite_mirror_flush_interval_sec", 2.0},
            {"sqlite_mirror_max_pending_ops", 500},
            {"sqlite_load_threads", 4},
            {"sqlite_precount_postings", false},
            {"full_index_strategy", "batch"},
            {"document_catalog_storage", "memory"},
            {"batch_reader_threads", 1},
            {"batch_indexer_threads", 0},
            {"batch_queue_memory_mb", 256},
        };
    }

    nh::json completeConfigWithLegacyAliases()
    {
        auto config = optionalFields();
        config["year"] = "2026";
        config["extensions"] = nh::json::array({"txt"});
        config["prm_base_dir"] = "";
        config["prd_base_dir"] = "";
        config["hide_mode"] = true;
        config["dirs"] = nh::json::array({"D:\\DATA"});
        config["exclude_dirs"] = nh::json::array({"D:\\DATA\\TEMP"});
        config["my_future_field"] = 123;
        return nh::json{
            {"config", config},
            {"custom_section", {{"abc", true}}},
        };
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

TEST(SettingsBlock2IndexRootsTest, EmptyCanonicalIndexRootsDoesNotFallBackToDirs)
{
    TemporaryDirectory temporary;
    const fs::path settingsPath = temporary.path() / "Settings.json";
    auto root = completeConfigWithLegacyAliases();
    root["config"]["index_roots"] = nh::json::array();
    root["config"]["dirs"] = nh::json::array({"D:\\OLD"});
    writeUtf8(settingsPath, root);

    const auto loaded = ConverterJSON::getSettings(settingsPath.string());
    EXPECT_TRUE(loaded.indexRoots.empty());
}

TEST(SettingsBlock2IndexRootsTest, WrongTypeCanonicalIndexRootsDoesNotFallBackToDirs)
{
    TemporaryDirectory temporary;
    const fs::path settingsPath = temporary.path() / "Settings.json";
    auto root = completeConfigWithLegacyAliases();
    root["config"]["index_roots"] = "bad";
    root["config"]["dirs"] = nh::json::array({"D:\\OLD"});
    writeUtf8(settingsPath, root);

    EXPECT_THROW(
        ConverterJSON::getSettings(settingsPath.string()),
        std::invalid_argument);
}

TEST(SettingsBlock2IndexRootsTest, LegacyDirsWithoutIndexRootsIsValid)
{
    TemporaryDirectory temporary;
    const fs::path settingsPath = temporary.path() / "Settings.json";
    auto root = completeConfigWithLegacyAliases();
    root["config"].erase("index_roots");
    root["config"]["dirs"] = nh::json::array({"D:\\OLD"});
    writeUtf8(settingsPath, root);

    const auto loaded = ConverterJSON::getSettings(settingsPath.string());
    ASSERT_EQ(loaded.indexRoots.size(), 1u);
    EXPECT_EQ(loaded.indexRoots.front(), "D:\\OLD");
}

TEST(SettingsBlock2IndexRootsTest, DirectReadLegacyAliasesDoesNotResaveUnknownFields)
{
    TemporaryDirectory temporary;
    const fs::path settingsPath = temporary.path() / "Settings.json";
    const auto root = completeConfigWithLegacyAliases();
    writeUtf8(settingsPath, root);
    const std::string before = readAll(settingsPath);

    const auto loaded = ConverterJSON::getSettings(settingsPath.string());
    EXPECT_TRUE(loaded.hideConsoleWindow);
    ASSERT_EQ(loaded.indexRoots.size(), 1u);
    EXPECT_EQ(loaded.indexRoots.front(), "D:\\DATA");
    ASSERT_EQ(loaded.excludedSubtrees.size(), 1u);
    EXPECT_EQ(loaded.excludedSubtrees.front(), "D:\\DATA\\TEMP");

    const std::string after = readAll(settingsPath);
    EXPECT_EQ(after, before);

    const auto saved = nh::json::parse(after);
    ASSERT_TRUE(saved.contains("config"));
    EXPECT_EQ(saved["config"].at("my_future_field").get<int>(), 123);
    ASSERT_TRUE(saved.contains("custom_section"));
    EXPECT_TRUE(saved["custom_section"].at("abc").get<bool>());
    EXPECT_TRUE(saved["config"].contains("hide_mode"));
    EXPECT_TRUE(saved["config"].contains("dirs"));
    EXPECT_TRUE(saved["config"].contains("exclude_dirs"));
    EXPECT_FALSE(saved["config"].contains("hide_console_window"));
    EXPECT_FALSE(saved["config"].contains("index_roots"));
    EXPECT_FALSE(saved["config"].contains("excluded_subtrees"));
}

TEST(SettingsBlock2IndexRootsTest, WriterEmitsCanonicalIndexRootsOnly)
{
    TemporaryDirectory temporary;
    const fs::path settingsPath = temporary.path() / "Settings.json";
    search_server::Settings settings;
    settings.year = "2026";
    settings.indexRoots = {"E:\\CUSTOM"};
    settings.extensions = {"txt"};
    settings.prm_base_dir = "";
    settings.prd_base_dir = "";

    ConverterJSON::setSettings(settings, settingsPath.string());

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
