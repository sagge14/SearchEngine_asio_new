#pragma once
#include "AbstractScheduledTask.h"
#include <unordered_map>
#include <memory>
#include <mutex>
#include <type_traits>
#include <deque>
#include <iostream>

struct DelayedEvent {
    std::function<void()> action;
    std::chrono::steady_clock::time_point execute_after;
    std::function<bool()> condition; // опционально: условие для выполнения
    int count{0};
};

template<typename TaskID>
class PeriodicTaskManager {

    std::unordered_multimap<TaskID, std::shared_ptr<AbstractScheduledTask>> tasks_;
    std::mutex mtx_;
    std::deque<DelayedEvent> delayedEvents;

    void scheduler_tick() {
        auto now = std::chrono::steady_clock::now();
        size_t initialQueueSize = delayedEvents.size();

        while (!delayedEvents.empty()) {
            auto evt = std::move(delayedEvents.front());
            delayedEvents.pop_front();

            // --- Проверка по времени и condition отдельно
            if (now < evt.execute_after) {
                /*
                std::cout << "[scheduler_tick] Not time yet for event (try " << evt.count
                          << "), execute_after in "
                          << std::chrono::duration_cast<std::chrono::seconds>(evt.execute_after - now).count()
                          << " sec, rescheduling." << std::endl;
                */
                delayedEvents.push_back(std::move(evt));
                break;
            }

            if (evt.condition && !evt.condition()) {
                /*
                std::cout << "[scheduler_tick] Condition function returned false (try " << evt.count
                          << "), rescheduling." << std::endl;
                */
                evt.execute_after = now + std::chrono::seconds(5);
                delayedEvents.push_back(std::move(evt));
                break;
            }

            // --- Запуск action
            bool error_occurred = false;

            try {
                evt.action();
                /*
                std::cout << "[scheduler_tick] Action executed, attempt " << evt.count << std::endl;
                 */
            } catch (const std::exception& ex) {
                error_occurred = true;
                std::cout << "[scheduler_tick] Exception in action (attempt "
                          << evt.count << "): " << ex.what() << std::endl;
            } catch (...) {
                error_occurred = true;
                std::cout << "[scheduler_tick] Unknown exception in action (attempt "
                          << evt.count << ")" << std::endl;
            }

            if (error_occurred) {
                evt.count++;
                if (evt.count < 20) {
                    delayedEvents.push_back(std::move(evt));
                    std::cout << "[scheduler_tick] Retry scheduled (attempt " << evt.count << ")" << std::endl;
                } else {
                    std::cout << "[scheduler_tick] Action failed after 20 errors, giving up." << std::endl;
                }
            }

            // После одного события — break, чтобы не зависнуть если условие всё время не выполняется
      //      break;
        }
/*
        std::cout << "[scheduler_tick] Queue size: " << delayedEvents.size()
                  << " (was " << initialQueueSize << ")" << std::endl;*/
    }


public:


    template<typename Task, typename... Args>
    void addTask(TaskID id, Args&&... args) {
        static_assert(std::is_base_of<AbstractScheduledTask, Task>::value, "Task must inherit AbstractScheduledTask");
        auto task = std::make_shared<Task>(std::forward<Args>(args)...);
        std::lock_guard lock(mtx_);

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

    void addDelayedEvent(const DelayedEvent& evt) {
        std::lock_guard lock(mtx_);
        delayedEvents.push_back(evt);
    }

    void tickDelayedEvents() {
        std::lock_guard lock(mtx_);
        scheduler_tick();
    }
};
