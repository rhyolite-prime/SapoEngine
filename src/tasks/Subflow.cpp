//
// Created by Emmanuel Addo-Odame on 14/06/2026.
//


#include "Subflow.hpp"
#include <future>
#include "Task.hpp"
#include "runtime/VirtualMachine.hpp"


namespace sapo::tasks {

    std::future<TaskOutcome> SubflowTask::execute(TaskExecutionContext& ctx) {
        return std::async(std::launch::async, [&ctx]() {
            auto node = std::static_pointer_cast<parser::SubflowNode>(ctx.node_config);

            // 1. Create a child context
            sapo::runtime::RuntimeContext child_memory;

            // 2. Map input variables
            // for (auto const& [key, val] : node->inputs) {
            //     child_memory.setVariable(key, ctx.runtime_memory.getVariable(val).value_or(""));
            // }

            // 3. Execute child workflow
            // runtime::VirtualMachine sub_vm(child_memory);
            // sub_vm.loadWorkflow(node->workflow);
            // auto result = sub_vm.run(); // Assume run() blocks if wait_for_completion is true

            //return result ? TaskOutcome::Success : TaskOutcome::Failed;
            return TaskOutcome::Success;
        });
    }
}
