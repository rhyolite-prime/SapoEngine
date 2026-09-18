//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//

#pragma once

#include "Task.hpp"

namespace sapo::tasks {

    class QueryTask : public ITask {
    public:
        QueryTask() = default;
        ~QueryTask() override = default;

        std::future<TaskOutcome> execute(TaskExecutionContext exec_context) override;
    };

} // namespace sapo::tasks