//
// Created by Emmanuel Addo-Odame on 14/06/2026.
//

#include "EventTask.hpp"

namespace sapo::tasks {

    std::future<TaskOutcome> EventTask::execute(TaskExecutionContext& ctx) {

        return std::async(std::launch::async, [&ctx]() {
            auto node = std::static_pointer_cast<parser::EventEmitNode>(ctx.node_config);

            // 1. Log the emission
            // In a production system, this would interact with an Event Bus (e.g., RabbitMQ or Kafka)
            // For now, we simulate by appending to an "event_log" variable in memory
            std::string event_data = "Event: " + node->name + " triggered.";
            ctx.runtime_memory.setVariable("last_event_triggered", node->name);

            return TaskOutcome::Success;
        });
    }
}