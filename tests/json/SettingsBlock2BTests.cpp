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
#include <string>

namespace {

namespace fs = std::filesystem;
namespace nh = nlohmann;

class TemporarySettings final {
public:
    TemporarySettings()
    {
        static std::atomic_uint64_t sequence{0};
        path_ = fs::temp_directory_path() /
            ("se-settings-block2b-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count() +
                static_cast<std::int64_t>(sequence.fetch_add(1))));
        fs::create_directories(path_);
        LogFile::setLogsDirectory(path_ / "logs");
        ConverterJSON::setInteractiveErrors(false);
    }

    ~TemporarySettings()
    {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    fs::path file() const { return path_ / "Settings.json"; }

private:
    fs::path path_;
};

nh::json completeConfig()
{
    return nh::json{
        {"year", "2026"},
        {"prm_base_dir", ""},
        {"prd_base_dir", ""},
        {"tlg_send_root", "D:\\"},
        {"razn_output_dir", "D:\\OPIS_ADMIN\\RAZN"},
        {"opis_base_dir", "D:\\OPIS_ADMIN"},
        {"f12_base_dir", "D:\\F12"},
        {"index_roots", nh::json::array({"D:\\DATA"})},
        {"excluded_subtrees", nh::json::array()},
        {"asio_port", 15001},
        {"thread_count", 4},
        {"max_response", 30},
        {"ind_time", 500},
        {"hide_console_window", false},
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

void writeJson(const fs::path& path, const nh::json& root)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << root.dump(2);
}

std::string readAll(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

TEST(SettingsBlock2BTest, LegacyAliasesMapWithoutResave)
{
    TemporarySettings temporary;
    auto config = completeConfig();
    config["extensions"] = nh::json::array({"txt", "", "TXT", ".atl"});
    config["exact_search"] = true;
    config["future_field"] = 42;
    const nh::json root{
        {"config", config},
        {"future_section", {{"keep", true}}}};
    writeJson(temporary.file(), root);
    const std::string before = readAll(temporary.file());

    const auto loaded = ConverterJSON::getSettings(temporary.file().string());
    ASSERT_EQ(loaded.indexedExtensions.size(), 2u);
    EXPECT_EQ(loaded.indexedExtensions[0], "txt");
    EXPECT_EQ(loaded.indexedExtensions[1], "atl");
    EXPECT_TRUE(loaded.includeExtensionlessFiles);
    EXPECT_EQ(loaded.queryWordMatch, search_server::QueryWordMatch::All);
    EXPECT_EQ(readAll(temporary.file()), before);
}

TEST(SettingsBlock2BTest, CanonicalValuesWinOverLegacyAliases)
{
    TemporarySettings temporary;
    auto config = completeConfig();
    config["indexed_extensions"] = nh::json::array({"shp"});
    config["include_extensionless_files"] = false;
    config["extensions"] = nh::json::array({"txt", ""});
    config["query_word_match"] = "any";
    config["exact_search"] = true;
    writeJson(temporary.file(), {{"config", config}});

    const auto loaded = ConverterJSON::getSettings(temporary.file().string());
    EXPECT_EQ(loaded.indexedExtensions,
              (std::vector<std::string>{"shp"}));
    EXPECT_FALSE(loaded.includeExtensionlessFiles);
    EXPECT_EQ(loaded.queryWordMatch, search_server::QueryWordMatch::Any);
}

TEST(SettingsBlock2BTest, ExplicitLegacyValuesMapToAnyAndOverrideEmptyAlias)
{
    TemporarySettings temporary;
    auto config = completeConfig();
    config["extensions"] = nh::json::array({"txt", ""});
    config["include_extensionless_files"] = false;
    config["exact_search"] = false;
    writeJson(temporary.file(), {{"config", config}});

    const auto loaded = ConverterJSON::getSettings(temporary.file().string());
    EXPECT_EQ(loaded.indexedExtensions,
              (std::vector<std::string>{"txt"}));
    EXPECT_FALSE(loaded.includeExtensionlessFiles);
    EXPECT_EQ(loaded.queryWordMatch, search_server::QueryWordMatch::Any);
}

TEST(SettingsBlock2BTest, InvalidCanonicalExtensionDoesNotFallBackToLegacy)
{
    TemporarySettings temporary;
    auto config = completeConfig();
    config["indexed_extensions"] = nh::json::array({".txt"});
    config["include_extensionless_files"] = false;
    config["extensions"] = nh::json::array({"txt"});
    config["query_word_match"] = "all";
    writeJson(temporary.file(), {{"config", config}});

    EXPECT_THROW(
        ConverterJSON::getSettings(temporary.file().string()),
        std::invalid_argument);
}

TEST(SettingsBlock2BTest, InvalidCanonicalQueryDoesNotFallBackToLegacy)
{
    TemporarySettings temporary;
    auto config = completeConfig();
    config["indexed_extensions"] = nh::json::array({"txt"});
    config["include_extensionless_files"] = false;
    config["query_word_match"] = "invalid";
    config["exact_search"] = true;
    writeJson(temporary.file(), {{"config", config}});

    EXPECT_THROW(
        ConverterJSON::getSettings(temporary.file().string()),
        std::invalid_argument);
}

TEST(SettingsBlock2BTest, MissingQueryModeUsesHistoricalAnyWithoutResave)
{
    TemporarySettings temporary;
    auto config = completeConfig();
    config["indexed_extensions"] = nh::json::array({"txt"});
    config["include_extensionless_files"] = false;
    writeJson(temporary.file(), {{"config", config}});
    const std::string before = readAll(temporary.file());

    const auto loaded = ConverterJSON::getSettings(temporary.file().string());
    EXPECT_EQ(loaded.queryWordMatch, search_server::QueryWordMatch::Any);
    EXPECT_EQ(readAll(temporary.file()), before);
}

TEST(SettingsBlock2BTest, WriterEmitsCanonicalFieldsOnly)
{
    TemporarySettings temporary;
    search_server::Settings settings;
    settings.year = "2026";
    settings.indexRoots = {"D:\\DATA"};
    settings.indexedExtensions = {"txt"};
    settings.includeExtensionlessFiles = true;
    settings.queryWordMatch = search_server::QueryWordMatch::All;

    ConverterJSON::setSettings(settings, temporary.file().string());
    const auto saved = nh::json::parse(readAll(temporary.file()));
    const auto& config = saved.at("config");
    EXPECT_TRUE(config.contains("indexed_extensions"));
    EXPECT_TRUE(config.contains("include_extensionless_files"));
    EXPECT_EQ(config.at("query_word_match"), "all");
    EXPECT_FALSE(config.contains("extensions"));
    EXPECT_FALSE(config.contains("exact_search"));
}

}  // namespace
