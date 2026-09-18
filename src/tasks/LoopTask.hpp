//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//

#pragma once

#include "Task.hpp"

namespace sapo::tasks {

    class LoopTask : public ITask {
    public:
        LoopTask() = default;
        ~LoopTask() override = default;

        std::future<TaskOutcome> execute(TaskExecutionContext exec_context) override;
    };

} // namespace sapo::tasks