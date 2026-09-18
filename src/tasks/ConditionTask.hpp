//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//

#pragma once

#include "Task.hpp"

namespace sapo::tasks {

    class ConditionTask : public ITask {
    public:
        ConditionTask() = default;
        ~ConditionTask() override = default;

        std::future<TaskOutcome> execute(TaskExecutionContext exec_context) override;
    };

} // namespace sapo