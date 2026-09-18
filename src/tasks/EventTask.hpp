//
// Created by Emmanuel Addo-Odame on 14/06/2026.
//

#pragma once
#include "Task.hpp"
#include <future>

namespace sapo::tasks {
    class EventTask : public ITask {
    public:
        std::future<TaskOutcome> execute(TaskExecutionContext& ctx);
    };
}