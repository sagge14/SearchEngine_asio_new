#include "BackupServiceApplication.h"
#include "BackupServiceOptions.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

class TemporaryApplicationConfig {
public:
    explicit TemporaryApplicationConfig(const std::string& jobs)
    {
        static std::atomic<unsigned long long> sequence{0};
        root_ =
            fs::temp_directory_path() /
            (
                "backup_service_application_" +
                std::to_string(
                    std::chrono::steady_clock::now()
                        .time_since_epoch()
                        .count()
                ) +
                "_" +
                std::to_string(sequence.fetch_add(1))
            );
        fs::create_directories(root_);
        config_ = root_ / "Backup.json";
        std::ofstream stream(config_, std::ios::binary | std::ios::trunc);
        stream << "{\"BackupJobs\":" << jobs << "}";
    }

    ~TemporaryApplicationConfig()
    {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    BackupRuntimePaths paths() const
    {
        return {
            root_ / "BackupService.exe",
            root_,
            config_,
            root_ / "logs"
        };
    }

    const fs::path& root() const
    {
        return root_;
    }

private:
    fs::path root_;
    fs::path config_;
};

TEST(BackupServiceOptions, RejectsIncompatibleModes)
{
    BackupServiceOptions options;
    std::string error;

    EXPECT_FALSE(parseBackupServiceOptions(
        {L"BackupService", L"--service", L"--once"},
        options,
        error
    ));
    EXPECT_NE(std::string::npos, error.find("mutually exclusive"));
}

TEST(BackupServiceOptions, ResolvesPathsFromExecutableAndDataDirectory)
{
    BackupServiceOptions options;
    options.data_dir = L"runtime";
    options.config = L"profiles/Backup.json";

    const BackupRuntimePaths paths = resolveBackupRuntimePaths(
        options,
        fs::path(L"C:/Program Files/Backup/BackupService.exe")
    );

    EXPECT_EQ(
        fs::path(L"C:/Program Files/Backup/runtime").lexically_normal(),
        paths.data_dir
    );
    EXPECT_EQ(
        fs::path(
            L"C:/Program Files/Backup/runtime/profiles/Backup.json"
        ).lexically_normal(),
        paths.config
    );
    EXPECT_EQ(paths.data_dir / "logs", paths.logs);
}

TEST(BackupServiceApplication, ReportsMissingConfiguration)
{
    BackupServiceOptions options;
    BackupRuntimePaths paths{
        fs::path(L"C:/Backup/BackupService.exe"),
        fs::temp_directory_path(),
        fs::temp_directory_path() / "definitely-missing-backup.json",
        fs::temp_directory_path() / "backup-service-test-logs"
    };
    std::error_code ignored;
    fs::remove(paths.config, ignored);

    BackupServiceApplication application;
    std::string error;
    EXPECT_FALSE(application.configure(options, paths, error));
    EXPECT_NE(std::string::npos, error.find("cannot be opened"));
}

TEST(BackupServiceApplication, ResolvesJobPathsRelativeToConfig)
{
    TemporaryApplicationConfig config(R"json(
[
  {
    "backup_dir": "snapshots",
    "period_sec": 3600,
    "targets": [{"src": "input.txt"}]
  }
]
)json");
    BackupServiceOptions options;
    BackupServiceApplication application;
    std::string error;

    ASSERT_TRUE(application.configure(options, config.paths(), error))
        << error;
    ASSERT_EQ(1u, application.groups().size());
    EXPECT_EQ(
        (config.root() / "snapshots").lexically_normal(),
        fs::path(application.groups().front().backup_dir)
    );
    EXPECT_EQ(
        (config.root() / "input.txt").lexically_normal(),
        application.groups().front().targets.front().src
    );
}

TEST(BackupServiceApplication, StopIsIdempotentAndJoinsWorkers)
{
    TemporaryApplicationConfig config(R"json(
[
  {
    "backup_dir": "snapshots",
    "period_sec": 3600,
    "targets": [{"src": "missing-input.txt"}]
  }
]
)json");
    BackupServiceOptions options;
    BackupServiceApplication application;
    std::string error;

    ASSERT_TRUE(application.configure(options, config.paths(), error))
        << error;
    ASSERT_TRUE(application.start(error)) << error;

    application.requestStop();
    application.requestStop();
    EXPECT_TRUE(application.waitFor(std::chrono::seconds(10)));
    application.stop();

    EXPECT_TRUE(application.stopRequested());
    EXPECT_FALSE(application.isRunning());
}

TEST(BackupServiceApplication, StopRequestedBeforeStartIsHonored)
{
    TemporaryApplicationConfig config(R"json(
[
  {
    "backup_dir": "snapshots",
    "period_sec": 3600,
    "targets": [{"src": "missing-input.txt"}]
  }
]
)json");
    BackupServiceOptions options;
    BackupServiceApplication application;
    std::string error;

    ASSERT_TRUE(application.configure(options, config.paths(), error))
        << error;
    application.requestStop();
    ASSERT_TRUE(application.start(error)) << error;
    EXPECT_TRUE(application.waitFor(std::chrono::seconds(10)));
    application.wait();
    EXPECT_FALSE(application.isRunning());
}

} // namespace
