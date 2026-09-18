
#include <iostream>

#include "runtime/Context.hpp"
#include "runtime/VirtualMachine.hpp"

int main() {
  std::cout << "==================================================\n";
  std::cout << "        Sapo DSL Runtime Environment CLI         \n";
  std::cout << "==================================================\n";

  // Sample workflow to consume an API with query params and headers
  std::string mock_workflow = R"([
        {
            "type": "noop",
            "id": "fetch_balance_triggered",
            "meta": { "label": "Triggered to fetch the session balance" }
        },
        {
            "type": "command",
            "id": "fetch_session_balance",
            "command": "http.get",
            "http_request": {
                "url": "http://213.165.245.124:5109/api/v1/subscription/get-session-balance",
                "query": {
                    "serviceId": "$service_id",
                    "serviceType": "$service_type"
                },
                "headers": {
                    "Private-Key": "e9e59eda-e5af-4ac7-8260-7f6c551e66b8"
                }
            },
            "output": [
                "result.accountBalance",
                "message",
                "success"
            ]
        },
        {
            "type": "terminate",
            "id": "finalize_run",
            "status": "success"
        }
    ])";

  // 1. Initialize our memory arena context layer
  sapo::runtime::RuntimeContext context;

    context.setVariable("service_id", "3bc86932-a4d1-475d-a1a9-6584e7eafc80");
    context.setVariable("service_type", "ussd");
  // 2. Fire up the Sapo Virtual Machine supervisor
  sapo::runtime::VirtualMachine vm;
  sapo::runtime::VMStatus execution_result = vm.runBlueprint(mock_workflow, context);

  // 3. Inspect variable outputs left inside the state container after completion
  if (execution_result == sapo::runtime::VMStatus::Success) {
    std::cout << "\n[Success] Verification Check:\n";
    auto out_var = context.getVariable("result.accountBalance");
    if (out_var.has_value()) {
      std::cout << "-> Evaluated Context Output (result.accountBalance):\n"
                << out_var.value().dump(2) << "\n";
    }
    auto msg_var = context.getVariable("message");
    if (msg_var.has_value()) {
      std::cout << "-> Evaluated Context Output (message):\n"
                << msg_var.value().dump(2) << "\n";
    }
  } else {
    std::cout << "\n[Failure] Sapo VM runtime terminated with an error state.\n";
  }

  return 0;
}
