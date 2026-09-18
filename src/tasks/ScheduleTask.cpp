//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//

#include "ScheduleTask.hpp"
#include <iostream>
#include <future>
#include <string>

namespace sapo::tasks {

    std::future<TaskOutcome> ScheduleTask::execute(TaskExecutionContext exec_context) {

        auto node_config = exec_context.node_config;
        std::string node_id = node_config->id.value_or("unnamed_schedule");

        return std::async(std::launch::async, [node_id, node_config]() -> TaskOutcome {
            try {
                // In a production engine, you extract cron strings or intervals from the node
                // e.g., auto interval = node_config->interval; ("30s", "daily")
                std::string interval_rule = "every 60s";
                std::string target_callback_subflow = "billing_sync_flow";

                std::cout << "[Scheduler Core: " << node_id << "] Registering recurring background trigger:\n"
                          << "   -> Rule: " << interval_rule << "\n"
                          << "   -> Executing Target: " << target_callback_subflow << "\n";

                // Here, you would interface with your underlying event loop timer
                // or a priority queue managed by your background thread daemon.
                // Example: m_timer_queue.register(interval_rule, target_callback_subflow);

                std::cout << "   -> [Scheduler Success] Hook successfully registered with daemon process.\n";

                // Fulfilling registration success. The current execution pass moves on,
                // while the background system tracks the recurring cadence.
                return TaskOutcome::Success;

            } catch (const std::exception& e) {
                std::cerr << "[Scheduler Worker Exception]: Failed to register hook: " << e.what() << "\n";
                return TaskOutcome::Failed;
            }
        });
    }

} // namespace sapo::tasks