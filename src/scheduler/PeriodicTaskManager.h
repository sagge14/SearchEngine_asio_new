#pragma once
#include "AbstractScheduledTask.h"
#include <unordered_map>
#include <memory>
#include <mutex>
#include <type_traits>

template<typename TaskID>
class PeriodicTaskManager {
    //std::unordered_map<TaskID, std::shared_ptr<AbstractScheduledTask>> tasks_;
    std::unordered_multimap<TaskID, std::shared_ptr<AbstractScheduledTask>> tasks_;
    std::mutex mtx_;

public:
    template<typename Task, typename... Args>
    void addTask(TaskID id, Args&&... args) {
        static_assert(std::is_base_of<AbstractScheduledTask, Task>::value, "Task must inherit AbstractScheduledTask");
        auto task = std::make_shared<Task>(std::forward<Args>(args)...);
        std::lock_guard lock(mtx_);
       // tasks_[id] = task;
        tasks_.emplace(id, task);
        task->start();
    }

    void stopTask(TaskID id) {
        std::lock_guard lock(mtx_);
        if (auto it = tasks_.find(id); it != tasks_.end()) {
            it->second->stop();
        }
    }

    void startTask(TaskID id) {
        std::lock_guard lock(mtx_);
        if (auto it = tasks_.find(id); it != tasks_.end()) {
            it->second->start();
        }
    }

    void removeTask(TaskID id) {
        std::lock_guard lock(mtx_);
        if (auto it = tasks_.find(id); it != tasks_.end()) {
            it->second->stop();
            tasks_.erase(it);
        }
    }

    void stopAll() {
        std::lock_guard lock(mtx_);
        for (auto& [id, task] : tasks_) {
            task->stop();
        }
    }

    void startAll() {
        std::lock_guard lock(mtx_);
        for (auto& [id, task] : tasks_) {
            task->start();
        }
    }

    std::shared_ptr<AbstractScheduledTask> getTask(TaskID id) {
        std::lock_guard lock(mtx_);
        if (auto it = tasks_.find(id); it != tasks_.end()) {
            return it->second;
        }
        return nullptr;
    }
};
