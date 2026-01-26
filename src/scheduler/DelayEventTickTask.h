//
// Created by Sg on 12.07.2025.
//
#pragma  once

#include "AbstractScheduledTask.h" // твой базовый класс задачи

template<typename TaskID>
class PeriodicTaskManager;

template<typename TaskID>
class DelayEventTickTask : public AbstractScheduledTask {
    PeriodicTaskManager<TaskID>* periodicTaskManager_;
public:
    // Конструктор (без template!)
    DelayEventTickTask(boost::asio::io_context& io,
                       boost::asio::any_io_executor cpu_ex,
                       std::chrono::seconds period,
                       PeriodicTaskManager<TaskID>& periodicTaskManager)
            : AbstractScheduledTask(io, cpu_ex, period)
            , periodicTaskManager_(&periodicTaskManager)
    {}

    void runTask() override
    {
        // Просто тикаем отложенные события
        if (periodicTaskManager_)
            periodicTaskManager_->tickDelayedEvents();
    }
};





