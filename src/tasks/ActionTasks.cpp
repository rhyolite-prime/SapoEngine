//
//  Sapo Engine — `action` and `subflow` tasks.
//
//  The action node is the conversation seam: it can call a capability, then it
//  suspends for user input (USSD menus) or for an event. Suspension is a typed
//  `SuspendRequest` — the session is serialised and no thread is parked, which
//  replaces the old `__SYS_PROMPT__` context-key protocol entirely.
//
#include "tasks/Tasks.hpp"

#include "util/JsonPath.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>

using json = nlohmann::json;

namespace sapo::tasks {

    namespace {
        std::string asText(const json &value) {
            if (value.is_string()) return value.get<std::string>();
            return value.dump();
        }
    } // namespace

    namespace {
        std::string optionText(const json &value) {
            if (value.is_string()) return value.get<std::string>();
            if (value.is_null()) return "";
            return value.dump();
        }

        json optionProperty(const json &item, const std::string &path) {
            if (item.is_object() && item.contains(path)) return item.at(path);
            if (item.is_object()) {
                if (auto found = sapo::util::getPath(item, path); found.has_value()) return *found;
            }
            return item;
        }

        std::vector<std::string> optionFields(const json &value) {
            if (value.is_array()) {
                std::vector<std::string> fields;
                for (const auto &entry : value) if (entry.is_string()) fields.push_back(entry.get<std::string>());
                return fields;
            }
            if (value.is_string()) return {value.get<std::string>()};
            return {};
        }

        std::string formatOption(const std::string &format, const json &item) {
            if (format.empty()) return optionText(item);
            std::string out;
            for (size_t i = 0; i < format.size();) {
                if (format[i] == '$' && i + 1 < format.size()) {
                    size_t end = i + 1;
                    while (end < format.size() &&
                           (std::isalnum(static_cast<unsigned char>(format[end])) || format[end] == '_' || format[end] == '.')) {
                        ++end;
                    }
                    if (end > i + 1) {
                        const std::string field = format.substr(i + 1, end - i - 1);
                        out += optionText(field == "value" ? item : optionProperty(item, field));
                        i = end;
                        continue;
                    }
                }
                out += format[i++];
            }
            return out;
        }

        struct DynamicMenu {
            json prompt;
            bool selected{false};
        };

        DynamicMenu renderDynamicMenu(const parser::ActionNode::PromptConfig &config,
                                      const json &rows, int page, const std::string &input,
                                      ExecutionContext &execution, const parser::ActionNode &node) {
            const json &options = config.options;
            if (!rows.is_array()) {
                throw runtime::SapoError(runtime::ErrorCode::Validation,
                                         "dynamic menu source must resolve to an array",
                                         json{{"source", options.value("source", "")}}, node.id);
            }
            const int pageSize = std::max(1, options.value("page_size", 5));
            const std::string pageVar = options.value("page_variable", node.id + ".page");
            const std::string nextValue = options.value("next_value", "n");
            const std::string previousValue = options.value("previous_value", "p");
            const std::string backValue = options.value("back_value", "0");
            const int pageCount = std::max(1, static_cast<int>((rows.size() + pageSize - 1) / pageSize));
            page = std::clamp(page, 0, pageCount - 1);

            if (!input.empty()) {
                if (input == nextValue && page + 1 < pageCount) {
                    execution.write(pageVar, page + 1);
                    page++;
                } else if (input == previousValue && page > 0) {
                    execution.write(pageVar, page - 1);
                    page--;
                } else if (input == backValue) {
                    // Back is represented as a normal selected value. A blueprint
                    // can route it with a choice node just like any other menu item.
                    execution.write(node.input_variable.value_or(config.output.value_or("input")), backValue);
                    return {{}, true};
                } else {
                    bool numeric = !input.empty() && std::all_of(input.begin(), input.end(),
                                                                  [](unsigned char c) { return std::isdigit(c); });
                    const int choice = numeric ? std::stoi(input) : -1;
                    const int offset = page * pageSize;
                    if (choice < 1 || choice > std::min(pageSize, static_cast<int>(rows.size()) - offset)) {
                        throw runtime::SapoError(runtime::ErrorCode::Validation,
                                                 "invalid dynamic menu selection '" + input + "'", json{{"input", input}}, node.id);
                    }
                    const json &item = rows.at(static_cast<size_t>(offset + choice - 1));
                    const std::string valueField = options.value("value", "");
                    json selected = valueField.empty() ? (item.is_object() ? json(choice) : item) : optionProperty(item, valueField);
                    execution.write(node.input_variable.value_or(config.output.value_or("input")), selected);
                    return {{}, true};
                }
            }

            const std::string labelFormat = options.value("labelFormat", options.value("label_format", ""));
            const std::string valueFormat = options.value("valueFormat", options.value("value_format", ""));
            const json labelConfig = options.contains("label") ? options.at("label") :
                                      (options.contains("labels") ? options.at("labels") : json(""));
            const auto labels = optionFields(labelConfig);
            const int offset = page * pageSize;
            const int end = std::min(offset + pageSize, static_cast<int>(rows.size()));
            std::ostringstream message;
            const json resolvedMessage = runtime::ExpressionEvaluator::resolve(config.message, execution.scope());
            message << (resolvedMessage.is_string() ? resolvedMessage.get<std::string>() : resolvedMessage.dump());
            message << "\n";
            json rendered = json::array();
            for (int i = offset; i < end; ++i) {
                const json &item = rows.at(static_cast<size_t>(i));
                std::string label;
                if (!labelFormat.empty()) label = formatOption(labelFormat, item);
                else {
                    std::vector<std::string> parts;
                    if (!labels.empty() && item.is_object()) {
                        for (const auto &field : labels) parts.push_back(optionText(optionProperty(item, field)));
                    }
                    label = parts.empty() ? optionText(item) : parts.front();
                    for (size_t part = 1; part < parts.size(); ++part) label += " " + parts[part];
                }
                json value;
                if (!valueFormat.empty()) value = formatOption(valueFormat, item);
                else {
                    const std::string valueField = options.value("value", "");
                    value = valueField.empty() ? (item.is_object() ? json(i - offset + 1) : item) : optionProperty(item, valueField);
                }
                message << (i - offset + 1) << ". " << label << "\n";
                rendered.push_back(json{{"label", label}, {"value", value}});
            }
            if (page > 0) message << previousValue << ". Previous\n";
            if (page + 1 < pageCount) message << nextValue << ". Next\n";
            message << backValue << ". Back";

            json prompt{{"message", message.str()}, {"interaction_type", config.interaction_type},
                        {"options", rendered}, {"page", page + 1}, {"page_count", pageCount},
                        {"input_validation", "dynamic_menu"}};
            return {std::move(prompt), false};
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
            if (!result.ok) throw runtime::SapoError(runtime::ErrorCode::Capability,
                "capability '" + node.capability + "' failed: " + result.error_message,
                json{{"capability", node.capability}}, node.id);
            if (node.outputs.is_object() && !node.outputs.empty()) {
                const json outputs = execution.resolve(node.outputs);
                for (auto it = outputs.begin(); it != outputs.end(); ++it)
                    execution.write(it.key(), extractField(result.value, it.value().is_string() ? it.value().get<std::string>() : it.value().dump(), execution));
            } else execution.write(node.id, result.value);
        }

        for (const auto &source : node.data_sources) {
            if (execution.data_sources.is_object() && execution.data_sources.contains(source.name)) continue;
            if (!execution.services.data_sources || !execution.services.data_sources->has(source.provider))
                throw runtime::SapoError(runtime::ErrorCode::NotFound,
                    "data source '" + source.name + "' declares provider '" + source.provider + "' which is not registered",
                    json{{"data_source", source.name}}, node.id);
        }

        if (node.on_event.has_value()) {
            const auto &event = *node.on_event;
            if (!event.trigger_condition.has_value() || execution.evalBoolOr(event.trigger_condition, true)) {
                SuspendRequest suspend;
                suspend.session_id = execution.session_id; suspend.node_id = node.id; suspend.reason = "event";
                suspend.event_name = event.event_name;
                if (event.handler.has_value()) suspend.resume_node = *event.handler;
                suspend.data = json{{"event", event.event_name}};
                return suspend;
            }
        }

        const bool resumed = execution.locals.is_object() && execution.locals.contains("input");
        if (node.prompt_config.has_value() && node.await_input) {
            const auto &prompt = *node.prompt_config;
            if (!prompt.options.empty()) {
                const json source = execution.resolve(prompt.options.value("source", ""));
                int page = 0;
                const std::string pageVar = prompt.options.value("page_variable", node.id + ".page");
                if (execution.has(pageVar)) {
                    const json pageValue = execution.read(pageVar);
                    if (pageValue.is_number_integer()) page = pageValue.get<int>();
                }
                const json input = resumed ? execution.locals.value("input", json()) : json();
                const auto rendered = renderDynamicMenu(prompt, source, page, input.is_string() ? input.get<std::string>() : optionText(input), execution, node);
                if (rendered.selected) {
                    if (input == prompt.options.value("back_value", "0")) {
                        // A dynamic menu's back value is still a value; normal choice routing can handle it.
                    }
                } else {
                    SuspendRequest suspend;
                    suspend.session_id = execution.session_id; suspend.node_id = node.id; suspend.reason = "input";
                    suspend.input_variable = node.input_variable.value_or(prompt.output.value_or("input"));
                    suspend.timeout_ms = prompt.timeout_ms.has_value() ? std::optional<int64_t>(*prompt.timeout_ms) : execution.services.limits.default_timeout_ms;
                    suspend.prompt = rendered.prompt;
                    return suspend;
                }
            } else if (!resumed) {
                SuspendRequest suspend;
                suspend.session_id = execution.session_id; suspend.node_id = node.id; suspend.reason = "input";
                suspend.input_variable = node.input_variable.value_or(prompt.output.value_or("input"));
                suspend.timeout_ms = prompt.timeout_ms.has_value() ? std::optional<int64_t>(*prompt.timeout_ms) : execution.services.limits.default_timeout_ms;
                json prompt_json{{"message", asText(runtime::ExpressionEvaluator::resolve(prompt.message, execution.scope()))}, {"interaction_type", prompt.interaction_type}};
                if (prompt.input_validation.has_value()) prompt_json["input_validation"] = prompt.input_validation->source();
                suspend.prompt = std::move(prompt_json);
                return suspend;
            }
        }
        if (resumed && node.prompt_config.has_value() && node.prompt_config->options.empty()) {
            const auto &prompt = *node.prompt_config; const json input = execution.locals.value("input", json());
            if (prompt.input_validation.has_value() && !prompt.input_validation->empty()) {
                auto scope = execution.scope(); scope.locals["input"] = input; scope.locals["$input"] = input; scope.locals["value"] = input;
                if (!runtime::ExpressionEvaluator::evaluateBool(*prompt.input_validation, scope)) {
                    std::string message = "input for node '" + node.id + "' did not satisfy the prompt rule '" +
                                          prompt.input_validation->source() + "'";
                    if (node.input_variable.has_value() && !node.input_variable->empty())
                        message += " (expected a value for '" + *node.input_variable + "')";
                    throw runtime::SapoError(runtime::ErrorCode::Validation, message, json{{"input", input}}, node.id);
                }
            }
        }

        for (const auto &reference : node.next_tasks)
            if (execution.evalBoolOr(reference.execute_condition, true)) return JumpTo{reference.task_id};
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
