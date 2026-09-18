//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//

#pragma once

#include "Task.hpp"

namespace sapo::tasks {

    class ParallelTask : public ITask {
    public:
        ParallelTask() = default;
        ~ParallelTask() override = default;

        std::future<TaskOutcome> execute(TaskExecutionContext exec_context) override;
    };

} // namespace sapo::tasks