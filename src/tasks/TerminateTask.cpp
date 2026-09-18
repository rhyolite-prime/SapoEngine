//
// Created by Emmanuel Addo-Odame on 14/06/2026.
//

#include "TerminateTask.hpp"


namespace sapo::tasks {

    std::future<TaskOutcome> TerminateTask::execute(TaskExecutionContext& ctx) {

        return std::async(std::launch::async, [&ctx]() {
            auto node = std::static_pointer_cast<parser::TerminateNode>(ctx.node_config);

            // 1. Log the termination reason
            ctx.runtime_memory.setVariable("final_status", node->status);

            // if (!node->error_code->empty()) {
            //     ctx.runtime_memory.setVariable("error_code", node->error_code);
            // }

            // 2. Set the global stop flag in the context
            // This signals the VM to halt execution
            ctx.runtime_memory.setVariable("__SYS_TERMINATE", "true");

            return TaskOutcome::Success; // Task itself succeeded in terminating
        });
    }
}