//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//

#include "QueryTask.hpp"
#include "parser/AstNodes.hpp"
#include <iostream>
#include <future>
#include <string>

namespace sapo::tasks {

    std::future<TaskOutcome> QueryTask::execute(TaskExecutionContext exec_context) {

        auto query_node = std::static_pointer_cast<parser::QueryNode>(exec_context.node_config);

        auto& memory = exec_context.runtime_memory;

        std::string node_id = query_node->id.value_or("unnamed_query");
        std::string source_id = query_node->data_source_id;
        std::string sql_statement = query_node->query_statement;
        auto params = query_node->query_parameters;
        std::string target_key = query_node->output_context_key;

        return std::async(std::launch::async, [node_id, source_id, sql_statement, params, target_key, &memory]() -> TaskOutcome {
            try {
                std::cout << "[Query Worker: " << node_id << "] Resolving read statement against connection profile: '" << source_id << "'...\n";

                // 1. Hydrate query arguments dynamically out of Sapo context registers
                nlohmann::json bound_arguments = nlohmann::json::object();
                for (const auto& param_key : params) {
                    auto live_val = memory.getVariable(param_key);
                    if (live_val.has_value()) {
                        bound_arguments[param_key] = *live_val;
                    } else {
                        bound_arguments[param_key] = nullptr;
                        std::cout << "   -> [Query Warning] Bind parameter target '$" << param_key << "' evaluates to NULL.\n";
                    }
                }

                std::cout << "   -> [Query Executing] Statement: \"" << sql_statement << "\"\n";
                if (!bound_arguments.empty()) {
                    std::cout << "   -> [Query Parameters] Bound mapping data frame: " << bound_arguments.dump() << "\n";
                }

                // 2. Database/Storage Mock Execution Step
                // In production, you will pull your pooled DB connector instance using 'source_id'
                // and pass 'sql_statement' along with 'bound_arguments'.

                // Let's mock a successful database result payload frame:
                nlohmann::json simulated_result_set = {
                    {"status", "ACTIVE"},
                    {"account_balance", 4500.25},
                    {"currency", "GHS"},
                    {"last_updated", "2026-06-13T18:00:00Z"}
                };

                // 3. Thread-safe persistence back to core VM storage
                memory.setVariable(target_key, simulated_result_set);
                std::cout << "   -> [Query Success] Dataset mapped back to context slot: $" << target_key << "\n";

                return TaskOutcome::Success;

            } catch (const std::exception& e) {
                std::cerr << "[Query Worker Exception]: Read transaction abort on node '" << node_id << "': " << e.what() << "\n";
                return TaskOutcome::Failed;
            }
        });
    }

} // namespace sapo::tasks
