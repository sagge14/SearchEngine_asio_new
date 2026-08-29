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
#include <type_traits>

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
                ("se-settings-block1-" + std::to_string(uniqueValue));
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
                 {"dirs", nh::json::array({"D:\\JANUARY"})},
                 {"extensions", nh::json::array({"txt"})},
                 {"prm_base_dir", ""},
                 {"prd_base_dir", ""},
                 {"asio_port", 15001},
                 {"thread_count", 4},
             }}
        };
    }
}

TEST(SettingsBlock1LegacyTest, LegacyHideModeMapsToHideConsoleWindow)
{
    TemporaryDirectory temporary;
    const fs::path settingsPath = temporary.path() / "Settings.json";
    auto root = minimalConfig();
    root["config"]["hide_mode"] = true;
    writeUtf8(settingsPath, root);

    const auto loaded = ConverterJSON::getSettings(settingsPath.string());
    EXPECT_TRUE(loaded.hideConsoleWindow);
}

TEST(SettingsBlock1LegacyTest, CanonicalHideConsoleWindowWinsOverLegacyHideMode)
{
    TemporaryDirectory temporary;
    const fs::path settingsPath = temporary.path() / "Settings.json";
    auto root = minimalConfig();
    root["config"]["hide_console_window"] = false;
    root["config"]["hide_mode"] = true;
    writeUtf8(settingsPath, root);

    const auto loaded = ConverterJSON::getSettings(settingsPath.string());
    EXPECT_FALSE(loaded.hideConsoleWindow);
}

TEST(SettingsBlock1LegacyTest, SettingsWithoutNameLoadsSuccessfully)
{
    TemporaryDirectory temporary;
    const fs::path settingsPath = temporary.path() / "Settings.json";
    writeUtf8(settingsPath, minimalConfig());

    EXPECT_NO_THROW(ConverterJSON::getSettings(settingsPath.string()));
}

TEST(SettingsBlock1LegacyTest, LegacyRetiredFieldsAreIgnoredOnLoad)
{
    TemporaryDirectory temporary;
    const fs::path settingsPath = temporary.path() / "Settings.json";
    auto root = minimalConfig();
    root["config"]["Name"] = "Server";
    root["config"]["Version"] = "1.1";
    root["config"]["dir"] = "";
    root["config"]["text_request"] = false;
    root["config"]["save_dictionary_to_file"] = false;
    root["Files"] = nh::json::array({"D:\\a.txt"});
    writeUtf8(settingsPath, root);

    const auto loaded = ConverterJSON::getSettings(settingsPath.string());
    EXPECT_FALSE(loaded.hideConsoleWindow);
}

namespace
{
    template<typename T, typename = void>
    struct HasDirMember : std::false_type {};

    template<typename T>
    struct HasDirMember<T, std::void_t<decltype(std::declval<T>().dir)>>
        : std::true_type {};
}

TEST(SettingsBlock1LegacyTest, RuntimeModelHasNoLegacyDirField)
{
    static_assert(!HasDirMember<search_server::Settings>::value);
}

// SearchServer::updateStep() and stop() always call index->saveIndex() after
// BLOCK 1; save_dictionary_to_file no longer gates persistence.
