#include "Backup/BackupConfig.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

class TemporaryConfigFile {
public:
    explicit TemporaryConfigFile(const std::string& contents)
    {
        static std::atomic<unsigned long long> sequence{0};
        const auto unique =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ =
            std::filesystem::temp_directory_path() /
            (
                "searchengine_backup_config_" + std::to_string(unique) + "_" +
                std::to_string(sequence.fetch_add(1)) + ".json"
            );

        std::ofstream stream(path_, std::ios::binary | std::ios::trunc);
        stream << contents;
    }

    ~TemporaryConfigFile()
    {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    const std::filesystem::path& path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

TEST(BackupConfig, LoadsValidGroupsAndDefaults)
{
    TemporaryConfigFile config(R"json(
{
  "BackupJobs": [
    {
      "backup_dir": "D:/Backup",
      "period_sec": 120,
      "targets": [
        {
          "src": "D:/Data",
          "max_versions": 7,
          "is_directory": true
        },
        {
          "src": "D:/Data/file.txt"
        }
      ]
    }
  ]
}
)json");

    const BackupConfigResult result = loadBackupConfig(config.path());

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(1u, result.groups.size());
    EXPECT_EQ("D:/Backup", result.groups.front().backup_dir);
    EXPECT_EQ(120u, result.groups.front().period_sec);
    ASSERT_EQ(2u, result.groups.front().targets.size());
    EXPECT_EQ(7u, result.groups.front().targets[0].max_versions);
    EXPECT_TRUE(result.groups.front().targets[0].is_directory);
    EXPECT_TRUE(
        result.groups.front().targets[0].mode == BackupMode::Auto
    );
    EXPECT_TRUE(result.groups.front().targets[0].cache);
    EXPECT_FALSE(result.groups.front().targets[0].skip_unchanged);
    EXPECT_EQ(5u, result.groups.front().targets[1].max_versions);
    EXPECT_FALSE(result.groups.front().targets[1].is_directory);
    EXPECT_TRUE(
        result.groups.front().targets[1].mode == BackupMode::Auto
    );
    EXPECT_TRUE(result.groups.front().targets[1].cache);
    EXPECT_FALSE(result.groups.front().targets[1].skip_unchanged);
}

TEST(BackupConfig, ReportsMissingConfigurationFile)
{
    const auto missing =
        std::filesystem::temp_directory_path() /
        "searchengine_backup_config_missing_file.json";
    std::error_code error;
    std::filesystem::remove(missing, error);

    const BackupConfigResult result = loadBackupConfig(missing);

    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.groups.empty());
    ASSERT_EQ(1u, result.issues.size());
    EXPECT_NE(
        std::string::npos,
        result.issues.front().message.find("cannot be opened")
    );
}

TEST(BackupConfig, RejectsZeroPeriodAndNegativeRetention)
{
    TemporaryConfigFile config(R"json(
{
  "BackupJobs": [
    {
      "backup_dir": "D:/Backup",
      "period_sec": 0,
      "targets": [
        {
          "src": "D:/Data/file.txt",
          "max_versions": -1
        }
      ]
    }
  ]
}
)json");

    const BackupConfigResult result = loadBackupConfig(config.path());

    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.groups.empty());
    ASSERT_EQ(2u, result.issues.size());
    EXPECT_NE(
        std::string::npos,
        result.issues[0].location.find("period_sec")
    );
    EXPECT_NE(
        std::string::npos,
        result.issues[1].location.find("max_versions")
    );
}

TEST(BackupConfig, RejectsDuplicateScheduledTarget)
{
    TemporaryConfigFile config(R"json(
{
  "BackupJobs": [
    {
      "backup_dir": "D:/Backup",
      "targets": [{"src": "D:/Data/file.txt"}]
    },
    {
      "backup_dir": "D:/Backup",
      "targets": [{"src": "D:/Data/file.txt"}]
    }
  ]
}
)json");

    const BackupConfigResult result = loadBackupConfig(config.path());

    EXPECT_FALSE(result.ok());
    ASSERT_EQ(1u, result.groups.size());
    ASSERT_EQ(1u, result.issues.size());
    EXPECT_NE(
        std::string::npos,
        result.issues.front().message.find("duplicate")
    );
}

TEST(BackupConfig, ReportsMalformedJson)
{
    TemporaryConfigFile config("{\"BackupJobs\": [");

    const BackupConfigResult result = loadBackupConfig(config.path());

    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.groups.empty());
    ASSERT_EQ(1u, result.issues.size());
    EXPECT_NE(
        std::string::npos,
        result.issues.front().message.find("parse error")
    );
}

TEST(BackupConfig, LoadsFilesystemModeAndRejectsUnknownMode)
{
    TemporaryConfigFile valid_config(R"json(
{
  "BackupJobs": [
    {
      "backup_dir": "D:/Backup",
      "targets": [
        {
          "src": "D:/Data/file.txt",
          "mode": "filesystem"
        }
      ]
    }
  ]
}
)json");
    const BackupConfigResult valid =
        loadBackupConfig(valid_config.path());
    ASSERT_TRUE(valid.ok());
    ASSERT_EQ(1u, valid.groups.size());
    ASSERT_EQ(1u, valid.groups.front().targets.size());
    EXPECT_TRUE(
        valid.groups.front().targets.front().mode ==
        BackupMode::Filesystem
    );

    TemporaryConfigFile invalid_config(R"json(
{
  "BackupJobs": [
    {
      "backup_dir": "D:/Backup",
      "targets": [
        {
          "src": "D:/Data/file.txt",
          "mode": "raw-ish"
        }
      ]
    }
  ]
}
)json");
    const BackupConfigResult invalid =
        loadBackupConfig(invalid_config.path());
    EXPECT_FALSE(invalid.ok());
    EXPECT_TRUE(invalid.groups.empty());
    ASSERT_EQ(1u, invalid.issues.size());
    EXPECT_NE(
        std::string::npos,
        invalid.issues.front().location.find(".mode")
    );
}

TEST(BackupConfig, LoadsCacheAndSkipUnchangedOptions)
{
    TemporaryConfigFile config(R"json(
{
  "BackupJobs": [
    {
      "backup_dir": "F:/AutoPadBackups",
      "period_sec": 60,
      "targets": [
        {
          "src": "D:/BASES_PRD/ARCHIVE.db3",
          "cache": false,
          "skip_unchanged": true
        }
      ]
    }
  ]
}
)json");

    const BackupConfigResult result = loadBackupConfig(config.path());

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(1u, result.groups.size());
    ASSERT_EQ(1u, result.groups.front().targets.size());
    EXPECT_FALSE(result.groups.front().targets.front().cache);
    EXPECT_TRUE(result.groups.front().targets.front().skip_unchanged);
}

TEST(BackupConfig, RejectsNonBooleanCacheOptions)
{
    TemporaryConfigFile config(R"json(
{
  "BackupJobs": [
    {
      "backup_dir": "F:/AutoPadBackups",
      "targets": [
        {
          "src": "D:/BASES_PRD/ARCHIVE.db3",
          "cache": "no",
          "skip_unchanged": 1
        }
      ]
    }
  ]
}
)json");

    const BackupConfigResult result = loadBackupConfig(config.path());

    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.groups.empty());
    ASSERT_EQ(2u, result.issues.size());
    EXPECT_NE(
        std::string::npos,
        result.issues[0].location.find(".cache")
    );
    EXPECT_NE(
        std::string::npos,
        result.issues[1].location.find(".skip_unchanged")
    );
}

TEST(BackupConfig, LoadsFlexibleMirrorHistoryPeriods)
{
    TemporaryConfigFile config(R"json(
{
  "BackupJobs": [
    {
      "backup_dir": "F:/EconomicalBackups",
      "period_sec": 10,
      "targets": [
        {
          "src": "D:/BASES_PRD",
          "is_directory": true,
          "strategy": "mirror_history",
          "history_periods": [
            "10s",
            {"every": "2min", "keep": 30},
            "3h",
            "1d",
            "2w",
            {"every": "1mo", "keep": 12}
          ]
        }
      ]
    }
  ]
}
)json");

    const BackupConfigResult result = loadBackupConfig(config.path());

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(1u, result.groups.size());
    ASSERT_EQ(1u, result.groups.front().targets.size());
    const BackupTarget& target = result.groups.front().targets.front();
    EXPECT_TRUE(target.strategy == BackupStrategy::MirrorHistory);
    ASSERT_EQ(6u, target.history_tiers.size());
    EXPECT_EQ("every_10s", target.history_tiers[0].name);
    EXPECT_EQ(10u, target.history_tiers[0].period_sec);
    EXPECT_EQ(1u, target.history_tiers[0].max_points);
    EXPECT_EQ("every_2min", target.history_tiers[1].name);
    EXPECT_EQ(120u, target.history_tiers[1].period_sec);
    EXPECT_EQ(30u, target.history_tiers[1].max_points);
    EXPECT_EQ(2u * 7u * 24u * 60u * 60u,
              target.history_tiers[4].period_sec);
    EXPECT_EQ(30u * 24u * 60u * 60u,
              target.history_tiers[5].period_sec);
    EXPECT_EQ(12u, target.history_tiers[5].max_points);
}

TEST(BackupConfig, LoadsCommaSeparatedMirrorPeriods)
{
    TemporaryConfigFile config(R"json(
{
  "BackupJobs": [
    {
      "backup_dir": "F:/EconomicalBackups",
      "targets": [
        {
          "src": "D:/BASES/ARCHIVE.db3",
          "strategy": "mirror_history",
          "history_periods": "10s, 2min, 3h, 1d, 2w, 1mo"
        }
      ]
    }
  ]
}
)json");

    const BackupConfigResult result = loadBackupConfig(config.path());

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(6u, result.groups.front().targets.front().history_tiers.size());
}

TEST(BackupConfig, RejectsAmbiguousAndDuplicateMirrorPeriods)
{
    TemporaryConfigFile config(R"json(
{
  "BackupJobs": [
    {
      "backup_dir": "F:/EconomicalBackups",
      "targets": [
        {
          "src": "D:/BASES",
          "strategy": "mirror_history",
          "history_periods": ["2mm", "120s"]
        },
        {
          "src": "D:/BASES_PRD",
          "strategy": "mirror_history",
          "history_periods": ["2min", "120s"]
        }
      ]
    }
  ]
}
)json");

    const BackupConfigResult result = loadBackupConfig(config.path());

    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.groups.empty());
    ASSERT_EQ(2u, result.issues.size());
    EXPECT_NE(
        std::string::npos,
        result.issues[0].message.find("use s, min, h, d, w or mo")
    );
    EXPECT_NE(
        std::string::npos,
        result.issues[1].message.find("duplicates")
    );
}

} // namespace
