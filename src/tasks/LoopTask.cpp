//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//

#include "LoopTask.hpp"
#include <iostream>
#include <future>
#include <string>

namespace sapo::tasks {

    std::future<TaskOutcome> LoopTask::execute(TaskExecutionContext exec_context) {

        auto node_config = exec_context.node_config;
        auto& memory = exec_context.runtime_memory;

        std::string node_id = node_config->id.value_or("unnamed_loop");

        return std::async(std::launch::async, [node_id, node_config, &memory]() -> TaskOutcome {
            try {
                // 1. Structural Configuration Parsing
                // Extract properties dynamically out of the internal meta/custom payload configurations
                // For direct adaptation, we parse these out of the node's native fields or unwrap json attributes
                std::string counter_name = "loop_idx";
                int limit_val = 5; // Default fallback execution boundary limit
                std::string break_target = "exit_step"; // Where to route when condition breaks

                // (Optional) If your parser binds custom attributes inside a schema mapping, extract here:
                // e.g., counter_name = node_config->counter;

                std::cout << "[Loop Controller: " << node_id << "] Processing iteration checkpoint...\n";

                // 2. Fetch or Initialize Loop Counter State within Runtime Memory Arena
                int current_index = 0;
                auto existing_counter = memory.getVariable(counter_name);

                if (existing_counter.has_value() && existing_counter->is_number_integer()) {
                    // Re-entrant pass: increment the active counter index
                    current_index = existing_counter->get<int>() + 1;
                } else {
                    // First execution pass initialize gate boundary
                    current_index = 0;
                }

                // 3. Evaluate Loop Execution Boundaries
                if (current_index < limit_val) {
                    std::cout << "   -> [Loop Continued] Index: " << current_index << " / " << limit_val << "\n";

                    // Write updated iteration counter back into shared context memory
                    memory.setVariable(counter_name, current_index);

                    // Return Success with no jump: allows interpreter to step down linearly into the loop body
                    return TaskOutcome::Success;
                } else {
                    std::cout << "   -> [Loop Terminated] Limit reached. Cleaning environment registers.\n";

                    // Scrape loop state cleanly from context so future runs don't carry corrupted offsets
                    memory.setVariable(counter_name, nullptr);

                    // Set jump override parameters to divert execution pointer forward past the loop body
                    memory.setVariable("__SYS_NEXT_JUMP", break_target);

                    return TaskOutcome::Success;
                }

            } catch (const std::exception& e) {
                std::cerr << "[Loop Worker Exception]: Runtime verification error: " << e.what() << "\n";
                return TaskOutcome::Failed;
            }
        });
    }

} // namespace sapo::tasks