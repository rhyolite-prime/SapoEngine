//
//  Sapo Engine — `action` and `subflow` tasks.
//
//  The action node is the conversation seam: it can call a capability, then it
//  suspends for user input (USSD menus) or for an event. Suspension is a typed
//  `SuspendRequest` — the session is serialised and no thread is parked, which
//  replaces the old `__SYS_PROMPT__` context-key protocol entirely.
//
#include "tasks/Tasks.hpp"

using json = nlohmann::json;

namespace sapo::tasks {

    namespace {
        std::string asText(const json &value) {
            if (value.is_string()) return value.get<std::string>();
            return value.dump();
        }
    } // namespace

    ControlSignal ActionTask::execute(ExecutionContext &execution) const {
        const auto &node = execution.as<parser::ActionNode>();

        // 1. Capability call (optional) — runs before the prompt so a menu can be
        //    built from data the node just fetched.
        if (!node.capability.empty()) {
            if (execution.services.capabilities == nullptr) {
                throw runtime::SapoError(runtime::ErrorCode::Internal, "no capability registry is installed",
                                         json::object(), node.id);
            }
            sapo::capabilities::CapabilityCall call;
            call.name = node.capability;
            call.node_id = node.id;
            call.context = &execution.context;
            call.inputs = execution.resolve(node.inputs);
            const auto result = execution.services.capabilities->dispatch(call);
            if (!result.ok) {
                throw runtime::SapoError(runtime::ErrorCode::Capability,
                                         "capability '" + node.capability + "' failed: " + result.error_message,
                                         json{{"capability", node.capability}}, node.id);
            }
            if (node.outputs.is_object() && !node.outputs.empty()) {
                const json outputs = execution.resolve(node.outputs);
                for (auto it = outputs.begin(); it != outputs.end(); ++it) {
                    execution.write(it.key(), extractField(result.value, it.value().is_string() ? it.value().get<std::string>() : it.value().dump(), execution));
                }
            } else {
                execution.write(node.id, result.value);
            }
        }

        // 2. Declared data sources are validated lazily here: a source that never
        //    resolved at startup fails the node rather than returning empty rows.
        for (const auto &source : node.data_sources) {
            if (execution.data_sources.is_object() && execution.data_sources.contains(source.name)) continue;
            if (!execution.services.data_sources || !execution.services.data_sources->has(source.provider)) {
                throw runtime::SapoError(runtime::ErrorCode::NotFound,
                                         "data source '" + source.name + "' declares provider '" + source.provider +
                                             "' which is not registered",
                                         json{{"data_source", source.name}}, node.id);
            }
        }

        // 3. Wait for an event instead of input, when asked.
        if (node.on_event.has_value()) {
            const auto &event = *node.on_event;
            if (event.trigger_condition.has_value() && !execution.evalBoolOr(event.trigger_condition, true)) {
                // Condition false ⇒ the event is not awaited; fall through to routing.
            } else {
                SuspendRequest suspend;
                suspend.session_id = execution.session_id;
                suspend.node_id = node.id;
                suspend.reason = "event";
                suspend.event_name = event.event_name;
                if (event.handler.has_value()) suspend.resume_node = *event.handler;
                suspend.data = json{{"event", event.event_name}};
                return suspend;
            }
        }

        // 4. Prompt (suspend for input) unless the node is capability-only, or the
        //    input already arrived (resume path writes `locals["input"]`).
        const bool resumed = execution.locals.is_object() && execution.locals.contains("input");
        if (node.prompt_config.has_value() && node.await_input && !resumed) {
            const auto &prompt = *node.prompt_config;
            SuspendRequest suspend;
            suspend.session_id = execution.session_id;
            suspend.node_id = node.id;
            suspend.reason = "input";
            suspend.input_variable = node.input_variable.value_or(prompt.output.value_or("input"));
            suspend.timeout_ms = prompt.timeout_ms.has_value()
                                     ? std::optional<int64_t>(static_cast<int64_t>(*prompt.timeout_ms))
                                     : execution.services.limits.default_timeout_ms;
            json prompt_json{{"message", asText(runtime::ExpressionEvaluator::resolve(prompt.message,
                                                                                       execution.scope()))},
                             {"interaction_type", prompt.interaction_type}};
            if (prompt.input_validation.has_value()) {
                prompt_json["input_validation"] = prompt.input_validation->source();
            }
            suspend.prompt = std::move(prompt_json);
            return suspend;
        }
        if (resumed && node.prompt_config.has_value()) {
            // The interpreter already wrote the reply to the context; echo the
            // validation result so a failing input is a hard error, not a surprise.
            const auto &prompt = *node.prompt_config;
            const json input = execution.locals.value("input", json());
            if (prompt.input_validation.has_value() && !prompt.input_validation->empty()) {
                auto scope = execution.scope();
                scope.locals["input"] = input;
                scope.locals["$input"] = input;
                scope.locals["value"] = input;
                if (!runtime::ExpressionEvaluator::evaluateBool(*prompt.input_validation, scope)) {
                    const std::string rule = prompt.input_validation->source();
                    std::string message = "input for node '" + node.id + "' did not satisfy the prompt rule '" +
                                          rule + "'";
                    if (node.input_variable.has_value() && !node.input_variable->empty()) {
                        message += " (expected a value for '" + *node.input_variable + "')";
                    }
                    json data{{"input", input}, {"rule", rule}};
                    if (prompt.message.empty() == false) data["prompt"] = prompt.message.source();
                    throw runtime::SapoError(runtime::ErrorCode::Validation, message, std::move(data), node.id);
                }
            }
        }

        // 5. Declarative routing: the first `next_tasks` entry whose condition holds.
        std::optional<JumpTo> jump;
        for (const auto &reference : node.next_tasks) {
            if (execution.evalBoolOr(reference.execute_condition, true)) {
                jump = JumpTo{reference.task_id};
                break;
            }
        }
        if (jump.has_value()) return *jump;
        return Continue{};
    }

    ControlSignal SubflowTask::execute(ExecutionContext &execution) const {
        const auto &node = execution.as<parser::SubflowNode>();

        runtime::ChildRunRequest request;
        request.workflow_id = node.workflow;
        request.input = execution.resolve(node.inputs);
        request.parent_session = execution.session_id;
        request.correlation_id = execution.session_id;   // one chain per top session
        request.depth = execution.depth + 1;
        request.wait_for_completion = node.wait_for_completion;
        request.node_id = node.id;
        if (node.output.has_value()) request.output_key = *node.output;
        request.return_map = node.return_map;

        const runtime::ChildRunResult result = execution.services.workflows->runChild(request);

        if (!result.output.is_null() && result.output.is_object()) {
            if (node.output.has_value() && !node.output->empty()) {
                execution.write(*node.output, result.output);
            }
            if (node.return_map.is_object()) {
                for (auto it = node.return_map.begin(); it != node.return_map.end(); ++it) {
                    const std::string child_key = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
                    execution.write(it.key(), result.output.contains(child_key) ? result.output[child_key] : json());
                }
            }
        }
        execution.write(node.id + "_session", result.session_id);
        execution.write(node.id + "_status", result.status);

        if (result.status == "failed") {
            // Surfaced as an error so `on_error` / `try` / retry can act on it.
            throw runtime::SapoError(runtime::ErrorCode::Capability,
                                     "subflow '" + node.workflow + "' failed: " + result.error,
                                     json{{"workflow", node.workflow}, {"child_session", result.session_id}}, node.id);
        }
        return Continue{};
    }

} // namespace sapo::tasks
