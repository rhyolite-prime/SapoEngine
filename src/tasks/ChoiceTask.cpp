//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//

#include "ChoiceTask.hpp"
#include "parser/AstNodes.hpp"
#include <iostream>
#include <future>
#include <string>
#include <map>

namespace sapo::tasks {

    std::future<TaskOutcome> ChoiceTask::execute(TaskExecutionContext exec_context) {
        // Cast base node config safely to the specific choice payload layout
        auto choice_node = std::static_pointer_cast<parser::ChoiceNode>(exec_context.node_config);
        auto& memory = exec_context.runtime_memory;

        std::string node_id = choice_node->id.value_or("unnamed_choice");
        std::string target_var_key = choice_node->variable_key;
        auto cases_map = choice_node->cases;
        auto fallback_target = choice_node->default_target;

        return std::async(std::launch::async, [node_id, target_var_key, cases_map, fallback_target, &memory]() -> TaskOutcome {
            try {
                std::cout << "[Choice Router: " << node_id << "] Evaluating variable matches on key: '$" << target_var_key << "'...\n";

                // 1. Resolve variable from context memory pool
                auto live_value_opt = memory.getVariable(target_var_key);
                std::string stringified_value = "";

                if (!live_value_opt.has_value() || live_value_opt->is_null()) {
                    std::cout << "   -> [Choice Warning] Evaluated variable is empty or null.\n";
                    stringified_value = "NULL";
                } else if (live_value_opt->is_string()) {
                    stringified_value = live_value_opt->get<std::string>();
                } else if (live_value_opt->is_number()) {
                    // Turn numbers into strings cleanly to simplify dictionary matching keys
                    stringified_value = live_value_opt->dump();
                } else if (live_value_opt->is_boolean()) {
                    stringified_value = live_value_opt->get<bool>() ? "true" : "false";
                }

                std::cout << "   -> [Choice Target] Current runtime value resolved to: '" << stringified_value << "'\n";

                // 2. Perform Mapping Lookup Check
                std::string selected_destination = "";
                auto match_it = cases_map.find(stringified_value);

                if (match_it != cases_map.end()) {
                    selected_destination = match_it->second;
                    std::cout << "   -> [Choice Match Found] Routing execution to branch target ID: " << selected_destination << "\n";
                } else if (fallback_target.has_value()) {
                    selected_destination = fallback_target.value();
                    std::cout << "   -> [Choice Fallback Triggered] Routing execution to default target ID: " << selected_destination << "\n";
                } else {
                    std::cout << "   -> [Choice Fallthrough] No match and no default route specified. Proceeding linearly.\n";
                }

                // 3. Command structural jump if a valid routing track was found
                if (!selected_destination.empty()) {
                    memory.setVariable("__SYS_NEXT_JUMP", selected_destination);
                }

                return TaskOutcome::Success;

            } catch (const std::exception& e) {
                std::cerr << "[Choice Worker Exception]: Branch determination failure: " << e.what() << "\n";
                return TaskOutcome::Failed;
            }
        });
    }

} // namespace sapo::tasks