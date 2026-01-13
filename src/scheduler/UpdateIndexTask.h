#include "scheduler/PeriodicTaskManager.h"
#include "scheduler/TaskId.h"
#include <iostream>
// Пример наследника задачи:
class UpdateIndexTask : public AbstractScheduledTask {
public:
    UpdateIndexTask(boost::asio::io_context& io, std::chrono::seconds period)
            : AbstractScheduledTask(io, period) {}
    void runTask() override {
        // Логика переиндексации...
        std::cout << "privet" << std::endl;
    }
};