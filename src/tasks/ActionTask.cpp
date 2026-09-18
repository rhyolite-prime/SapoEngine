//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//

#include "ActionTask.hpp"
#include "parser/AstNodes.hpp"
#include "runtime/ExpressionEvaluator.hpp"
#include <iostream>
#include <future>
#include <chrono>
#include <thread>

namespace sapo::tasks {

std::future<TaskOutcome> ActionTask::execute(TaskExecutionContext exec_context) {

  auto action_node = std::static_pointer_cast<parser::ActionNode>(exec_context.node_config);

  std::string node_id = action_node->id.value_or("unnamed_action");
  std::string capability = action_node->capability;

  return std::async(std::launch::async, [node_id, capability, exec_context]() mutable -> TaskOutcome {
      try {
          std::cout << "[Action Worker: " << node_id << "] Executing high-level capability: '" << capability << "'...\n";
          
          // Simulated delay for capability processing (e.g. plugin call)
          std::this_thread::sleep_for(std::chrono::milliseconds(150));
          
          // In the future, this is where we lookup the `capability` against registered plugins, 
          // extract the `inputs` from the context, run the plugin, and store the result in `outputs`.

          // Evaluate PromptConfig if defined, so the VM can suspend with a dynamic prompt
          auto action_node = std::static_pointer_cast<parser::ActionNode>(exec_context.node_config);
          if (action_node->prompt_config.has_value()) {
              const auto& pc = action_node->prompt_config.value();
              std::string evaluated_message;
              if (std::holds_alternative<std::string>(pc.message)) {
                  nlohmann::json resolved = sapo::runtime::ExpressionEvaluator::resolveValue(std::get<std::string>(pc.message), exec_context.runtime_memory);
                  if (resolved.is_string()) {
                      evaluated_message = resolved.get<std::string>();
                  } else {
                      evaluated_message = resolved.dump();
                  }
              } else {
                  evaluated_message = "[Structured message expression not supported yet]";
              }
              exec_context.runtime_memory.setVariable("__SYS_SUSPENDED_PROMPT", evaluated_message);
              exec_context.runtime_memory.setVariable("__SYS_INTERACTION_TYPE", pc.interaction_type);
              
              if (pc.timeout_ms.has_value()) {
                  exec_context.runtime_memory.setVariable("__SYS_TIMEOUT_MS", pc.timeout_ms.value());
              }
              if (pc.input_validation.has_value()) {
                  if (std::holds_alternative<std::string>(pc.input_validation.value())) {
                      exec_context.runtime_memory.setVariable("__SYS_INPUT_VALIDATION", std::get<std::string>(pc.input_validation.value()));
                  }
              }
          }
          
          std::cout << "   -> [Plugin Engine] Capability executed successfully.\n";

          return TaskOutcome::Success;

      } catch (const std::exception &e) {
          std::cerr << "[Action Worker Exception]: " << e.what() << "\n";
          return TaskOutcome::Failed;
      }
  });
}

} // namespace sapo::tasks