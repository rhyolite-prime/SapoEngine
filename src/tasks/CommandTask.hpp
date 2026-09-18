//
// Created by Emmanuel Addo-Odame on 14/06/2026.
//

#pragma once

#include <future>
#include "Task.hpp"

namespace sapo::tasks {

    /**
     * @brief Executes external system-level commands and integrates
     * results into the RuntimeContext.
     */
    class CommandTask : public ITask {
    public:
        CommandTask() = default;
        ~CommandTask() override = default;

        /**
         * @brief Spawns an asynchronous process to execute the system command.
         * @param ctx The execution context containing the node definition and master memory.
         * @return A future tracking the completion of the command execution.
         */
        std::future<TaskOutcome> execute(TaskExecutionContext ctx) override;
    };

} // namespace sapo::tasks