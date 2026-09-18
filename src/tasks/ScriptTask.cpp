//
// Created by Emmanuel Addo-Odame on 14/06/2026.
//

#include "ScriptTask.hpp"
#include <future>
#include "Task.hpp"


namespace sapo::tasks {

    std::future<TaskOutcome> ScriptTask::execute(TaskExecutionContext& ctx) {

        return std::async(std::launch::async, [&ctx]() {

            auto node = std::static_pointer_cast<parser::ScriptNode>(ctx.node_config);

            // 1. Prepare environment
            // In practice: Pass memory context as JSON/environment variables to the script.

            //std::string result = executeScriptEngine(node->language, node->code, ctx.runtime_memory);

            // 2. Bind output to memory.
            if (!node->output->empty()) {
                //ctx.runtime_memory.setVariable(node->output, result);
            }

            return TaskOutcome::Success;
        });
    }
}
