#pragma once

enum class TaskId {
    FlushPendingTask,
    PeriodicUpdateTask,
    BackupTask,
    ReserveChecker,
    EventNotifier,
    // ... расширяй по необходимости
};
