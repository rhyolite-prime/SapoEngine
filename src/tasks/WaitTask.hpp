//
// Created by Emmanuel Addo-Odame on 14/06/2026.
//
#pragma once

#include "Task.hpp"

namespace sapo::tasks {

    class WaitTask : public ITask {
    public:
        WaitTask() = default;
        ~WaitTask() override = default;

        std::future<TaskOutcome> execute(TaskExecutionContext& exec_context);
    };

}