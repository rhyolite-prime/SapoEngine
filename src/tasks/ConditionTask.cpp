//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//

#include "ConditionTask.hpp"
#include "parser/AstNodes.hpp"
#include "third_party/exprtk.hpp"
#include <iostream>
#include <future>
#include <map>

namespace sapo::tasks {

    std::future<TaskOutcome> ConditionTask::execute(TaskExecutionContext exec_context) {
        auto condition_node = std::static_pointer_cast<parser::ConditionNode>(exec_context.node_config);

        // Capture safe properties for the background thread
        std::string node_id = condition_node->id.value_or("unnamed_condition");
        std::string expression_str = condition_node->expression;
        std::string true_target = condition_node->on_true;
        std::optional<std::string> false_target = condition_node->on_false;

        return std::async(std::launch::async,
            [node_id, expression_str, true_target, false_target, exec_context]() mutable -> TaskOutcome {
                try {
                    std::cout << "[Condition Worker: " << node_id << "] Evaluating rule: '" << expression_str << "'...\n";

                    exprtk::symbol_table<double> symbol_table;

                    // 1. Dynamic Variable Binding
                    // We must hold variables in a container where memory addresses DO NOT move.
                    // std::map guarantees stable addresses for its values, unlike std::vector.
                    std::map<std::string, double> bound_numbers;
                    std::map<std::string, std::string> bound_strings;

                    // Parse the entire context state to feed known variables to the engine
                    auto state_map = nlohmann::json::parse(exec_context.runtime_memory.serializeState());

                    for (const auto& [key, val] : state_map.items()) {
                        if (val.is_number()) {
                            bound_numbers[key] = val.get<double>();
                            symbol_table.add_variable(key, bound_numbers[key]);
                        } else if (val.is_string()) {
                            bound_strings[key] = val.get<std::string>();
                            symbol_table.add_stringvar(key, bound_strings[key]);
                        }
                    }
                    symbol_table.add_constants();

                    // 2. Compile and Parse the Boolean Expression
                    exprtk::expression<double> expression;
                    expression.register_symbol_table(symbol_table);

                    exprtk::parser<double> parser;
                    if (!parser.compile(expression_str, expression)) {
                        std::cerr << "[Condition Exception]: Failed to compile syntax: " << expression_str << "\n";
                        return TaskOutcome::Failed;
                    }

                    // 3. Evaluate (ExprTk evaluates TRUE as 1.0, and FALSE as 0.0)
                    double result = expression.value();
                    bool is_true = (result != 0.0);

                    std::cout << "   -> [Condition Output] Result evaluated to: " << (is_true ? "TRUE" : "FALSE") << "\n";

                    // 4. Signal the routing engine via the execution context variables
                    std::string jump_destination = is_true ? true_target : false_target.value_or("");

                    if (!jump_destination.empty()) {
                        // Write a system-level hidden variable for the interpreter to catch
                        exec_context.runtime_memory.setVariable("__SYS_NEXT_JUMP", jump_destination);
                    }

                    return TaskOutcome::Success;

                } catch (const std::exception& e) {
                    std::cerr << "[Condition Worker Exception]: " << e.what() << "\n";
                    return TaskOutcome::Failed;
                }
            }
        );
    }

} // namespace sapo