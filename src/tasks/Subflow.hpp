//
// Created by Emmanuel Addo-Odame on 14/06/2026.
//

#pragma once


#include <future>

#include "Task.hpp"

namespace sapo::tasks {
    class SubflowTask: public ITask {
    public:
        std::future<TaskOutcome> execute(TaskExecutionContext& ctx);
    };
}
