#pragma once

enum class TaskId {
    FlushPendingTask,
    PeriodicUpdateTask,
    BackupTask,
    DelayEventTickTask,
    DelayOpisBaseUpdateTask,
    // ... расширяй по необходимости
};
