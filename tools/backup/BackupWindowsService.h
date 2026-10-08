#pragma once

#include "BackupServiceOptions.h"

int runBackupWindowsService(
    const BackupServiceOptions& options,
    const BackupRuntimePaths& paths
);
