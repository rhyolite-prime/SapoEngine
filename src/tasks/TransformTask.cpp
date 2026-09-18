//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//

#include "TransformTask.hpp"
#include <iostream>
#include <future>
#include <string>
#include <algorithm>

namespace sapo::tasks {

    std::future<TaskOutcome> TransformTask::execute(TaskExecutionContext exec_context) {

        auto node_config = exec_context.node_config;
        auto& memory = exec_context.runtime_memory;
        std::string node_id = node_config->id.value_or("unnamed_transform");

        return std::async(std::launch::async, [node_id, node_config, &memory]() -> TaskOutcome {
            try {
                std::cout << "[Transform Worker: " << node_id << "] Processing data mutation rules...\n";

                // Simulated schema extraction. In your actual TransformNode, these will be native fields:
                // e.g., std::string action_type = transform_node->action;
                // For demonstration, we'll look up how your schema drives mutations:
                std::string action_type = "MAP"; // Options: UPPERCASE, LOWERCASE, EXTRACT, MAP
                std::string source_key = "api_response";
                std::string target_key = "cleaned_payload";

                // --- Scenario 1: Case Modification ---
                if (action_type == "UPPERCASE" || action_type == "LOWERCASE") {
                    auto var_opt = memory.getVariable(source_key);
                    if (var_opt.has_value() && var_opt->is_string()) {
                        std::string data = var_opt->get<std::string>();
                        if (action_type == "UPPERCASE") {
                            std::transform(data.begin(), data.end(), data.begin(), ::toupper);
                        } else {
                            std::transform(data.begin(), data.end(), data.begin(), ::tolower);
                        }
                        memory.setVariable(target_key, data);
                        std::cout << "   -> [Transform Success] Mutated string written to: $" << target_key << "\n";
                    }
                }

                // --- Scenario 2: Deep Field Extraction ---
                else if (action_type == "EXTRACT") {
                    auto var_opt = memory.getVariable(source_key); // e.g. {"status": "OK", "data": {"id": 102}}
                    if (var_opt.has_value() && var_opt->is_object()) {
                        // Let's assume your node config tells it to look for a nested key path like "/data/id"
                        std::string json_pointer_path = "/data/transaction_id";

                        try {
                            nlohmann::json extracted_val = var_opt->at(nlohmann::json::json_pointer(json_pointer_path));
                            memory.setVariable(target_key, extracted_val);
                            std::cout << "   -> [Transform Success] Extracted pointer path " << json_pointer_path << "\n";
                        } catch (const std::exception&) {
                            std::cerr << "   -> [Transform Warning] Pointer target path not found in source object.\n";
                            return TaskOutcome::Failed;
                        }
                    }
                }

                // --- Scenario 3: Structural Mapping / Translation ---
                else if (action_type == "MAP") {
                    // Say your DSL specifies a translation template: e.g., {"output_field": "$source_field"}
                    nlohmann::json transformation_template = {
                        {"receipt_id", "$api_response/transaction_id"},
                        {"system_status", "PROCESSED_SUCCESSFULLY"},
                        {"generated_by", "Sapo Core Engine v1"}
                    };

                    nlohmann::json mapped_result = nlohmann::json::object();

                    for (const auto& [key, value] : transformation_template.items()) {
                        if (value.is_string() && !value.get<std::string>().empty() && value.get<std::string>()[0] == '$') {
                            std::string variable_expr = value.get<std::string>().substr(1);

                            // Check for simple reference vs nested reference splits
                            size_t slash_pos = variable_expr.find('/');
                            if (slash_pos == std::string::npos) {
                                // Simple reference variable swap
                                auto source_data = memory.getVariable(variable_expr);
                                mapped_result[key] = source_data.has_value() ? *source_data : nlohmann::json(nullptr);
                            } else {
                                // Deep object reference extraction swap
                                std::string base_var = variable_expr.substr(0, slash_pos);
                                std::string ptr_path = variable_expr.substr(slash_pos); // keep leading '/'

                                auto source_obj = memory.getVariable(base_var);
                                if (source_obj.has_value() && source_obj->is_object()) {
                                    try {
                                        mapped_result[key] = source_obj->at(nlohmann::json::json_pointer(ptr_path));
                                    } catch (...) {
                                        mapped_result[key] = nullptr;
                                    }
                                }
                            }
                        } else {
                            mapped_result[key] = value;
                        }
                    }

                    // Save the newly structured record object back to the engine state registers
                    memory.setVariable(target_key, mapped_result);
                    std::cout << "   -> [Transform Success] Struct mapping finalized: " << mapped_result.dump() << "\n";
                }

                return TaskOutcome::Success;

            } catch (const std::exception& e) {
                std::cerr << "[Transform Worker Exception]: Failure reshaping data frame structures: " << e.what() << "\n";
                return TaskOutcome::Failed;
            }
        });
    }

}
