//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//

#include "WaitTask.hpp"
#include <thread>
#include <future>
#include <chrono>

namespace sapo::tasks {

    std::future<TaskOutcome> WaitTask::execute(TaskExecutionContext& ctx) {

        return std::async(std::launch::async, [&ctx]() {
            auto node = std::static_pointer_cast<parser::WaitNode>(ctx.node_config);

            // Case 1: Duration-based wait
            if (!node->duration->empty()) {
                // Simplified: assuming format "Ns" (e.g., "5s")
                //int seconds = std::stoi(node->duration);
                int seconds = 1;
                std::this_thread::sleep_for(std::chrono::seconds(seconds));
            }
            // Case 2: Condition-based wait (Polling)
            else if (!node->until.has_value()) {

                std::this_thread::sleep_for(std::chrono::milliseconds(100));

                // i will come back to this later...

                // while (!evaluateCondition(ctx.runtime_memory, node->until)) {
                //     std::this_thread::sleep_for(std::chrono::milliseconds(100));
                // }
            }

            return TaskOutcome::Success;
        });
    }
}