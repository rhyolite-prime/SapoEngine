//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//

#pragma once

#include "Task.hpp"

namespace sapo::tasks {

    class ActionTask : public ITask {
    public:
        ActionTask() = default;
        ~ActionTask() override = default;

        /**
         * @brief Dispatches the low-level action call pipeline asynchronously.
         */
        std::future<TaskOutcome> execute(TaskExecutionContext exec_context) override;
    };

} // namespace sapo