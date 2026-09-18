//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//

#include "VirtualMachine.hpp"
#include <iostream>
#include "Interpreter.hpp"
#include "parser/AstNodes.hpp"
#include "parser/WorkflowParser.hpp"
#include "ExpressionEvaluator.hpp"

namespace sapo::runtime {

    VMStatus VirtualMachine::runBlueprint(const std::string& json_blueprint, RuntimeContext& context) {
        m_status = VMStatus::Running;
        auto start_time = std::chrono::steady_clock::now();

        std::cout << "[Sapo VM] Initializing Execution Sandbox...\n";

        parser::WorkflowAST ast;
        try {
            // 1. Pass raw input through the frontend parser compiler pipeline
            ast = parser::WorkflowParser::parse(json_blueprint);
        }
        catch (const std::exception& e) {
            std::cerr << "[Sapo VM] Frontend Rejection: " << e.what() << "\n";
            m_status = VMStatus::ParseError;
            return m_status;
        }

        // 2. Pass the validated AST into our runtime evaluation core
        Interpreter interpreter(std::move(ast));
        bool execution_success = interpreter.execute(context);

        // 3. Calculate execution telemetry metrics
        auto end_time = std::chrono::steady_clock::now();
        m_metrics.execution_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        if (execution_success) {
            std::cout << "[Sapo VM] Run finalized successfully. Duration: " << m_metrics.execution_duration.count() << "ms\n";
            m_status = VMStatus::Success;
        } else {
            std::cerr << "[Sapo VM] Run finalized with an internal execution hazard.\n";
            m_status = VMStatus::ExecutionError;
        }

        return m_status;
    }

    VMStatus VirtualMachine::resumeBlueprint(const std::string& json_blueprint, RuntimeContext& context) {
        m_status = VMStatus::Running;
        auto start_time = std::chrono::steady_clock::now();

        std::cout << "[Sapo VM] Resuming Execution Sandbox...\n";

        parser::WorkflowAST ast;
        try {
            ast = parser::WorkflowParser::parse(json_blueprint);
        }
        catch (const std::exception& e) {
            std::cerr << "[Sapo VM] Frontend Rejection: " << e.what() << "\n";
            m_status = VMStatus::ParseError;
            return m_status;
        }

        Interpreter interpreter(std::move(ast));
        
        std::string resume_node_id = "";
        auto resume_var = context.getVariable("__SYS_RESUME_NODE");
        if (resume_var.has_value() && resume_var->is_string()) {
            resume_node_id = resume_var->get<std::string>();
        }

        // We can safely clear the resume node ID now, so it doesn't loop infinitely if we terminate
        context.setVariable("__SYS_RESUME_NODE", nlohmann::json());

        // Validate user input if validation rule exists
        auto validation_rule = context.getVariable("__SYS_INPUT_VALIDATION");
        if (validation_rule.has_value() && validation_rule->is_string()) {
            std::string rule = validation_rule->get<std::string>();
            try {
                auto resolved = ExpressionEvaluator::resolveValue(rule, context);
                // Assume the expression must evaluate to a truthy value (1.0 or true string)
                bool is_valid = false;
                if (resolved.is_boolean()) {
                    is_valid = resolved.get<bool>();
                } else if (resolved.is_number()) {
                    is_valid = (resolved.get<double>() != 0);
                } else if (resolved.is_string()) {
                    is_valid = (resolved.get<std::string>() == "true" || resolved.get<std::string>() == "1");
                }
                
                if (!is_valid) {
                    std::cerr << "[Sapo VM] Input validation failed for rule: " << rule << "\n";
                    
                    // Prefix the existing prompt with an error message
                    auto existing_prompt = context.getVariable("__SYS_SUSPENDED_PROMPT");
                    if (existing_prompt.has_value() && existing_prompt->is_string()) {
                        context.setVariable("__SYS_SUSPENDED_PROMPT", "Invalid Input. " + existing_prompt->get<std::string>());
                    } else {
                        context.setVariable("__SYS_SUSPENDED_PROMPT", "Invalid Input. Please try again.");
                    }
                    
                    // Re-suspend to ask again instead of failing the pipeline
                    m_status = VMStatus::Running;
                    return m_status;
                }
            } catch (const std::exception& e) {
                std::cerr << "[Sapo VM] Input validation exception: " << e.what() << "\n";
                m_status = VMStatus::ExecutionError;
                return m_status;
            }
        }

        bool execution_success = interpreter.execute(context, resume_node_id);

        auto end_time = std::chrono::steady_clock::now();
        m_metrics.execution_duration += std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        if (execution_success) {
            std::cout << "[Sapo VM] Resume finalized successfully. Total Duration: " << m_metrics.execution_duration.count() << "ms\n";
            m_status = VMStatus::Success;
        } else {
            std::cerr << "[Sapo VM] Resume finalized with an internal execution hazard.\n";
            m_status = VMStatus::ExecutionError;
        }

        return m_status;
    }

} // namespace sapo