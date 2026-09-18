//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//

#include "Interpreter.hpp"
#include <iostream>
#include <stdexcept>
#include "tasks/ActionTask.hpp"
#include "tasks/CommandTask.hpp"
#include "tasks/ConditionTask.hpp"
#include "tasks/LoopTask.hpp"
#include "tasks/ScheduleTask.hpp"
#include "tasks/TransformTask.hpp"
#include "tasks/ChoiceTask.hpp"
#include "tasks/QueryTask.hpp"
#include "third_party/exprtk.hpp"

namespace sapo::runtime {

    Interpreter::Interpreter(parser::WorkflowAST ast) : m_ast(std::move(ast)) {
        indexNodes();
    }

    void Interpreter::indexNodes() {
        // Map all nodes that have an explicit user-defined ID
        for (const auto& node : m_ast) {
            if (node->id.has_value()) {
                m_node_registry[node->id.value()] = node;
            }
        }
    }

    bool Interpreter::execute(RuntimeContext& context, const std::string& start_node_id) {
        if (m_ast.empty()) {
            std::cout << "[Sapo Engine] Warning: Empty workflow layout. Execution skipped.\n";
            return true;
        }

        std::shared_ptr<parser::AstNode> current_node = nullptr;
        size_t linear_index = 0;

        // Resume functionality: fast-forward to the target node
        if (!start_node_id.empty() && m_node_registry.contains(start_node_id)) {
            current_node = m_node_registry[start_node_id];
            for (size_t i = 0; i < m_ast.size(); ++i) {
                if (m_ast[i] == current_node) { linear_index = i; break; }
            }
            std::cout << "[Sapo Engine] Resuming workflow from node: " << start_node_id << "\n";
        } else {
            current_node = m_ast.front();
            std::cout << "[Sapo Engine] Initiating workflow execution graph from the beginning...\n";
        }

        while (current_node != nullptr) {
            std::optional<std::string> next_target_id;

            try {
                next_target_id = executeNode(current_node, context);
            } catch (const std::exception& e) {
                std::cerr << "[Sapo Engine] Critical Runtime Exception: " << e.what() << "\n";
                return false;
            }

            // Routing Engine Logic: Determine where to point the execution cursor next
            if (next_target_id.has_value()) {
                // Scenario A: The task explicitly commanded a jump to a named node ID (e.g., branching or looping)
                const std::string& target = next_target_id.value();
                if (target == "__TERMINATE_SUCCESS__") {
                    return true;
                } else if (target == "__TERMINATE_FAILED__") {
                    return false;
                } else if (target == "__SUSPEND__") {
                    // Save the next target node ID so we can resume properly
                    std::string resume_target = "";
                    
                    if (current_node->getType() == parser::TaskType::Action) {
                        auto action_node = std::static_pointer_cast<parser::ActionNode>(current_node);
                        if (!action_node->next_tasks.empty()) {
                            resume_target = action_node->next_tasks.front().task_id;
                        }
                    }
                    
                    if (resume_target.empty() && linear_index + 1 < m_ast.size() && m_ast[linear_index + 1]->id.has_value()) {
                        resume_target = m_ast[linear_index + 1]->id.value();
                    }
                    
                    if (!resume_target.empty()) {
                        context.setVariable("__SYS_RESUME_NODE", resume_target);
                    }
                    std::cout << "[Sapo Engine] Workflow suspended successfully. Awaiting resume signal.\n";
                    return true;
                }

                if (m_node_registry.contains(target)) {
                    current_node = m_node_registry[target];
                    // Sync our linear index tracking in case we break back into a sequence later
                    for (size_t i = 0; i < m_ast.size(); ++i) {
                        if (m_ast[i] == current_node) { linear_index = i; break; }
                    }
                } else {
                    std::cerr << "[Sapo Engine] Routing Failure: Target node ID '" << target << "' not found.\n";
                    return false;
                }
            } else {
                // Scenario B: No explicit jump command; simply step forward to the next linear task in the sequence array
                linear_index++;
                if (linear_index < m_ast.size()) {
                    current_node = m_ast[linear_index];
                } else {
                    current_node = nullptr; // End of the line reached naturally
                }
            }
        }

        std::cout << "[Sapo Engine] Workflow completed execution path successfully.\n";
        return true;
    }

    std::optional<std::string> Interpreter::executeNode(const std::shared_ptr<parser::AstNode>& node, RuntimeContext& context) {
        // Output debugging track if node has an assigned ID
        std::string node_log_id = node->id.value_or("unnamed_step");

        switch (node->getType()) {
            case parser::TaskType::Noop: {
                std::cout << "[Step: " << node_log_id << "] Processing Noop (Pass-Through).\n";
                return std::nullopt;
            }

            case parser::TaskType::Script: {
                auto script_node = std::static_pointer_cast<parser::ScriptNode>(node);
                std::cout << "[Step: " << node_log_id << "] Evaluating live " << script_node->language << " script engine...\n";

                if (script_node->language == "expr") {
                    // 1. Setup the ExprTk symbol table mapping
                    exprtk::symbol_table<double> symbol_table;

                    // 2. Dynamically pull dependent variable parameters out of the Sapo Context memory arena
                    auto all_vars = context.getAllVariables();
                    std::map<std::string, double> numeric_vars;
                    
                    for (auto& [key, val] : all_vars.items()) {
                        if (val.is_number()) {
                            numeric_vars[key] = val.get<double>();
                        }
                    }

                    // Bind local variables to the text token string used inside the script "code"
                    for (auto& [key, val] : numeric_vars) {
                        symbol_table.add_variable(key, val);
                    }
                    symbol_table.add_constants();

                    // 3. Register the symbol table into an execution expression target
                    exprtk::expression<double> expression;
                    expression.register_symbol_table(symbol_table);

                    // 4. Parse and compile the raw string expression on the fly
                    exprtk::parser<double> parser;
                    if (!parser.compile(script_node->code, expression)) {
                        throw std::runtime_error("ExprTk Compilation Failed for code: " + script_node->code);
                    }

                    // 5. Execute the math calculation!
                    double calculated_result = expression.value();
                    std::cout << "   -> [Engine Math Execution] Calculated result: " << calculated_result << "\n";

                    // 6. Write the real computed value back into the Sapo Memory Context
                    if (script_node->output.has_value()) {
                        context.setVariable(script_node->output.value(), calculated_result);
                    }
                } else {
                    throw std::runtime_error("Unsupported script sandboxing runtime target: " + script_node->language);
                }
                return std::nullopt;
            }

            case parser::TaskType::Terminate: {
                auto term_node = std::static_pointer_cast<parser::TerminateNode>(node);
                std::cout << "[Step: " << node_log_id << "] Explicit Termination Encountered. Status: " << term_node->status << "\n";

                if (term_node->status == "failed") {
                    if (term_node->error_code.has_value()) {
                        std::cerr << "[Sapo Error Code]: " << term_node->error_code.value() << "\n";
                    }
                    return "__TERMINATE_FAILED__";
                }
                return "__TERMINATE_SUCCESS__";
            }

            case parser::TaskType::Transform: {

                std::cout << "[Step: " << node_log_id << "] Dispatching to In-Memory Transform Engine...\n";

                // 1. Package context boundaries
                tasks::TaskExecutionContext task_ctx{ node, context };
                tasks::TransformTask transform_worker;

                // 2. Fire the asynchronous worker data layer
                std::future<tasks::TaskOutcome> task_future = transform_worker.execute(task_ctx);
                tasks::TaskOutcome outcome = task_future.get();

                if (outcome == tasks::TaskOutcome::Failed) {
                    std::cerr << "[Sapo VM Routing] Data transformation failed. Halting pipeline.\n";
                    return "__TERMINATE_FAILED__";
                }

                // Success: Move straight ahead linearly
                return std::nullopt;
            }

            case parser::TaskType::Condition: {

                std::cout << "[Step: " << node_log_id << "] Dispatching condition via ITask asynchronous worker...\n";

                // 1. Bundle the AST configuration node and the live runtime memory context
                tasks::TaskExecutionContext task_ctx{ node, context };

                // 2. Instantiate the condition worker component
                tasks::ConditionTask condition_worker;

                // 3. Fire the worker on a background thread pool and capture its execution future
                std::future<tasks::TaskOutcome> task_future = condition_worker.execute(task_ctx);

                // 4. Safely halt this workflow instance loop until ExprTk finishes evaluating the logic
                tasks::TaskOutcome outcome = task_future.get();

                if (outcome == tasks::TaskOutcome::Failed) {
                    std::cerr << "[Sapo VM Routing] Condition task emitted a critical failure. Halting pipeline.\n";
                    return "__TERMINATE_FAILED__";
                }

                // 5. Query the context to see if the worker determined a conditional jump route
                auto jump_override = context.getVariable("__SYS_NEXT_JUMP");

                if (jump_override.has_value() && jump_override->is_string()) {
                    std::string next_id = jump_override->get<std::string>();

                    // Clear the system flag immediately so it does not interfere with future steps
                    context.setVariable("__SYS_NEXT_JUMP", nullptr);

                    std::cout << "   -> [Engine Router] Diverting execution cursor to node: " << next_id << "\n";
                    return next_id; // Returns the target ID string to trigger Scenario A routing
                }

                return std::nullopt;
            }

            case parser::TaskType::Query: {
                std::cout << "[Step: " << node_log_id << "] Dispatching database read query task configuration...\n";

                // 1. Bundle execution environments
                tasks::TaskExecutionContext task_ctx{ node, context };
                tasks::QueryTask query_worker;

                // 2. Execute non-blocking thread and block explicitly on the future barrier
                std::future<tasks::TaskOutcome> task_future = query_worker.execute(task_ctx);
                tasks::TaskOutcome outcome = task_future.get();

                if (outcome == tasks::TaskOutcome::Failed) {
                    std::cerr << "[Sapo VM Routing] Critical query worker failure encountered. Halting pipeline execution.\n";
                    return "__TERMINATE_FAILED__";
                }

                // Step forward cleanly to the next sequential node layout
                return std::nullopt;
            }

            case parser::TaskType::Command: {
                std::cout << "[Step: " << node_log_id << "] Dispatching command via ITask asynchronous worker...\n";

                // 1. Bundle the configuration and memory state
                tasks::TaskExecutionContext task_ctx{ node, context };

                // 2. Instantiate the worker (In the future, you'll pull this from a mapped registry)
                tasks::CommandTask command_worker;

                // 3. Fire the worker non-blocking, and immediately capture the future
                std::future<tasks::TaskOutcome> task_future = command_worker.execute(task_ctx);

                // 4. Safely block the main VM loop until the background system network call finishes
                tasks::TaskOutcome outcome = task_future.get();

                // 5. Route the engine based on the worker's definitive outcome token
                if (outcome == tasks::TaskOutcome::Failed) {
                    std::cerr << "[Sapo VM Routing] Command task emitted a critical failure. Halting pipeline.\n";
                    return "__TERMINATE_FAILED__";
                }

                // Success: Return nullopt to advance to the next step linearly
                return std::nullopt;
            }

            case parser::TaskType::Action: {
                std::cout << "[Step: " << node_log_id << "] Action Task encountered. Suspending workflow for external input.\n";
                // Optionally execute the ActionTask worker here if it does pre-processing, 
                // but for USSD it just means 'suspend and return prompt'.
                
                tasks::TaskExecutionContext task_ctx{ node, context };
                tasks::ActionTask action_worker;

                std::future<tasks::TaskOutcome> task_future = action_worker.execute(task_ctx);
                tasks::TaskOutcome outcome = task_future.get();

                if (outcome == tasks::TaskOutcome::Failed) {
                    std::cerr << "[Sapo VM Routing] Action task emitted a critical failure. Halting pipeline.\n";
                    return "__TERMINATE_FAILED__";
                }

                return "__SUSPEND__";
            }


            case parser::TaskType::Event: {
                auto ev = std::static_pointer_cast<parser::EventEmitNode>(node);
                std::cout << "[Step: " << node_log_id << "] Emitting system event: " << ev->name << "\n";
                return ev->next; // Returns user-defined target jump pointer if specified
            }

            case parser::TaskType::Wait: {
                auto w = std::static_pointer_cast<parser::WaitNode>(node);
                std::cout << "[Step: " << node_log_id << "] Task 'wait' layer invoked.\n";
                return w->next;
            }

            case parser::TaskType::Choice: {
                std::cout << "[Step: " << node_log_id << "] Dispatching multi-branch choice switch worker...\n";

                tasks::TaskExecutionContext task_ctx{ node, context };
                tasks::ChoiceTask choice_worker;

                std::future<tasks::TaskOutcome> task_future = choice_worker.execute(task_ctx);
                tasks::TaskOutcome outcome = task_future.get();

                if (outcome == tasks::TaskOutcome::Failed) {
                    std::cerr << "[Sapo VM Routing] Choice branch evaluation crashed. Halting pipeline.\n";
                    return "__TERMINATE_FAILED__";
                }

                // Intercept dynamic jumps requested by the match parameters
                auto jump_override = context.getVariable("__SYS_NEXT_JUMP");
                if (jump_override.has_value() && jump_override->is_string()) {
                    std::string next_id = jump_override->get<std::string>();
                    context.setVariable("__SYS_NEXT_JUMP", nullptr); // clear flag

                    std::cout << "   -> [Engine Router] Case branch matching success. Moving to: " << next_id << "\n";
                    return next_id;
                }

                return std::nullopt; // Fallthrough straight ahead if no matches occurred
            }

            case parser::TaskType::Schedule: {
                std::cout << "[Step: " << node_log_id << "] Dispatching to Temporal Schedule Engine...\n";

                tasks::TaskExecutionContext task_ctx{ node, context };
                tasks::ScheduleTask schedule_worker;

                std::future<tasks::TaskOutcome> task_future = schedule_worker.execute(task_ctx);
                tasks::TaskOutcome outcome = task_future.get();

                if (outcome == tasks::TaskOutcome::Failed) {
                    std::cerr << "[Sapo VM Routing] Temporal registration failed. Halting pipeline.\n";
                    return "__TERMINATE_FAILED__";
                }
                return std::nullopt; // Moves forward line-by-line after setting up the schedule hook
            }

            case parser::TaskType::Loop: {
                std::cout << "[Step: " << node_log_id << "] Evaluating inline Re-entrant Loop Controller...\n";

                tasks::TaskExecutionContext task_ctx{ node, context };
                tasks::LoopTask loop_worker; // Uses the stateful counter logic we built earlier

                std::future<tasks::TaskOutcome> task_future = loop_worker.execute(task_ctx);
                tasks::TaskOutcome outcome = task_future.get();

                if (outcome == tasks::TaskOutcome::Failed) {
                    std::cerr << "[Sapo VM Routing] Loop condition processing error. Halting pipeline.\n";
                    return "__TERMINATE_FAILED__";
                }

                // Capture structural graph breaks commanded by the loop engine counter bounds
                auto jump_override = context.getVariable("__SYS_NEXT_JUMP");
                if (jump_override.has_value() && jump_override->is_string()) {
                    std::string next_id = jump_override->get<std::string>();
                    context.setVariable("__SYS_NEXT_JUMP", nullptr); // clear flag

                    std::cout << "   -> [Engine Router] Loop condition broken. Branching to node: " << next_id << "\n";
                    return next_id;
                }

                return std::nullopt; // Step right down into the loop body
            }

            case parser::TaskType::Subflow:
                std::cout << "[Step: " << node_log_id << "] Invoking Child Subflow pipeline.\n";
                return std::nullopt;

            default:
                throw std::runtime_error("Unhandled task execution type signature.");
        }
    }

} // namespace sapo