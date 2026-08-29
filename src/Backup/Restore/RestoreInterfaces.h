#pragma once

#include "Backup/Restore/RestoreTypes.h"

#include <memory>
#include <string>
#include <vector>

struct IRestoreProgress {
    virtual void onPhase(const std::string& name) = 0;
    virtual void onFile(
        size_t index,
        size_t total,
        const std::string& path
    ) = 0;
    virtual void onWarning(const std::string& message) = 0;
    virtual bool isCancelled() const = 0;
    virtual ~IRestoreProgress() = default;
};

struct IBackupStoreScanner {
    virtual std::vector<RestoreTargetInfo> scanRoot(
        const std::filesystem::path& backup_root,
        std::string& error
    ) = 0;
    virtual ~IBackupStoreScanner() = default;
};

struct IRestorePointCatalog {
    virtual std::vector<RestorePointInfo> listPoints(
        const RestoreTargetInfo& target,
        std::string& error
    ) = 0;
    virtual RestorePointInfo currentPoint(
        const RestoreTargetInfo& target,
        std::string& error
    ) = 0;
    virtual bool findTarget(
        const std::filesystem::path& backup_root,
        const std::string& id_or_path,
        RestoreTargetInfo& target,
        std::string& error
    ) = 0;
    virtual bool loadPointFromManifest(
        const std::filesystem::path& manifest_path,
        RestorePointInfo& point,
        std::string& error
    ) = 0;
    virtual ~IRestorePointCatalog() = default;
};

struct IRestoreFileCatalog {
    virtual std::vector<RestoreFileInfo> listFiles(
        const RestorePointInfo& point,
        std::string& error
    ) = 0;
    virtual ~IRestoreFileCatalog() = default;
};

struct IRestorePlanner {
    virtual std::vector<RestorePlanEntry> plan(
        const RestorePointInfo& point,
        const std::vector<std::string>& path_filter,
        std::string& error
    ) = 0;
    virtual ~IRestorePlanner() = default;
};

struct IRestoreVerifier {
    virtual bool verify(
        const RestorePointInfo& point,
        const std::vector<std::string>& path_filter,
        IRestoreProgress* progress,
        std::string& error
    ) = 0;
    virtual ~IRestoreVerifier() = default;
};

struct IRestoreExecutor {
    virtual bool restore(
        const RestoreRequest& request,
        IRestoreProgress* progress,
        std::string& error
    ) = 0;
    virtual ~IRestoreExecutor() = default;
};

struct RestoreServices {
    std::shared_ptr<IBackupStoreScanner> scanner;
    std::shared_ptr<IRestorePointCatalog> points;
    std::shared_ptr<IRestoreFileCatalog> files;
    std::shared_ptr<IRestorePlanner> planner;
    std::shared_ptr<IRestoreVerifier> verifier;
    std::shared_ptr<IRestoreExecutor> executor;
};

RestoreServices createMirrorHistoryRestoreServices();
