//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//

#include "WorkflowParser.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

using json = nlohmann::json;

namespace sapo::parser {

    // Helper function to extract expressions or structured expression objects safely
    static Expression parseExpression(const json& j) {
        if (j.is_string()) {
            return j.get<std::string>();
        } else if (j.is_object() && j.contains("left") && j.contains("operator") && j.contains("right")) {
            ExpressionMap exp_map;
            exp_map.left = j["left"].get<std::string>();
            exp_map.operator_str = j["operator"].get<std::string>();
            exp_map.right = j["right"].get<std::string>();
            return exp_map;
        }
        throw std::runtime_error("Invalid grammar shape: Expected a string expression or comparison object.");
    }

    static std::shared_ptr<AstNode> parseSingleNode(const json& node_json) {

        if (!node_json.contains("type")) {
            throw std::runtime_error("Task node missing 'type' identifier.");
        }

        std::string type_str = node_json["type"].get<std::string>();
        std::shared_ptr<AstNode> node = nullptr;

        if (type_str == "parallel") {
            auto p_node = std::make_shared<ParallelNode>();
            p_node->fail_fast = node_json.value("fail_fast", true);
            if (node_json.contains("child_tasks") && node_json["child_tasks"].is_array()) {
                for (const auto& child : node_json["child_tasks"]) {
                    p_node->child_tasks.push_back(parseSingleNode(child));
                }
            }
            node = p_node;
        }
        else if (type_str == "command") {
            auto commandNode = std::make_shared<CommandNode>();
            commandNode->command = node_json.at("command").get<std::string>();

            if (node_json.contains("output")) {
                if (node_json["output"].is_string()) {
                    commandNode->output = node_json["output"].get<std::string>();
                } else if (node_json["output"].is_array()) {
                    std::vector<std::string> out_arr;
                    for (const auto& item : node_json["output"]) {
                        out_arr.push_back(item.get<std::string>());
                    }
                    commandNode->output = out_arr;
                }
            }
            // Parse optional http_request block — only required for "http.*" commands
            if (node_json.contains("http_request") && node_json["http_request"].is_object()) {
                const auto& hr = node_json["http_request"];
                CommandNode::HttpRequestConfig config;
                config.url = hr.at("url").get<std::string>();
                if (hr.contains("auth") && hr["auth"].is_object()) {
                    CommandNode::HttpAuthConfig auth_config;
                    auth_config.type = hr["auth"].at("type").get<std::string>();
                    auth_config.username = hr["auth"].at("username").get<std::string>();
                    auth_config.password = hr["auth"].at("password").get<std::string>();
                    config.auth = auth_config;
                }
                if (hr.contains("headers") && hr["headers"].is_object()) {
                    config.headers = hr["headers"].get<std::map<std::string, std::string>>();
                }
                if (hr.contains("query") && hr["query"].is_object()) {
                    config.query = hr["query"].get<std::map<std::string, std::string>>();
                }
                if (hr.contains("body")) {
                    if (hr["body"].is_string()) {
                        config.body = hr["body"].get<std::string>();
                    } else if (hr["body"].is_object()) {
                        config.body = hr["body"].get<std::map<std::string, std::string>>();
                    }
                }
                if (hr.contains("timeout") && hr["timeout"].is_number_integer()) {
                    config.timeout = hr["timeout"].get<int>();
                }
                commandNode->http_request = std::move(config);
            }
            node = commandNode;
        }
        else if (type_str == "wait") {
            auto waitNode = std::make_shared<WaitNode>();
            if (node_json.contains("duration") && node_json.contains("until")) {
                throw std::runtime_error("Wait node constraint violation: Cannot contain both 'duration' and 'until'.");
            }
            if (node_json.contains("duration")) waitNode->duration = node_json["duration"].get<std::string>();
            if (node_json.contains("until")) waitNode->until = parseExpression(node_json["until"]);
            if (node_json.contains("next")) waitNode->next = node_json["next"].get<std::string>();
            node = waitNode;
        }
        else if (type_str == "noop") {
            auto noopNode = std::make_shared<NoopNode>();
            if (node_json.contains("meta") && node_json["meta"].is_object()) {
                noopNode->meta = node_json["meta"].get<std::map<std::string, std::string>>();
            }
            node = noopNode;
        }
        else if (type_str == "script") {
            auto scriptNode = std::make_shared<ScriptNode>();
            scriptNode->language = node_json.at("language").get<std::string>();
            scriptNode->code = node_json.at("code").get<std::string>();
            if (node_json.contains("output")) {
                scriptNode->output = node_json["output"].get<std::string>();
            }
            node = scriptNode;
        }
        else if (type_str == "transform") {
            auto transformNode = std::make_shared<TransformNode>();
            transformNode->operation = node_json.at("operation").get<std::string>();
            transformNode->input = node_json.at("input").get<std::string>();
            if (node_json.at("mapping").is_string()) {
                transformNode->mapping = node_json.at("mapping").get<std::string>();
            } else if (node_json.at("mapping").is_object()) {
                transformNode->mapping = node_json.at("mapping").get<std::map<std::string, std::string>>();
            } else {
                throw std::runtime_error("Transform mapping must be a string or an object.");
            }
            transformNode->output = node_json.at("output").get<std::string>();
            node = transformNode;
        }
        else if (type_str == "query") {
            auto queryNode = std::make_shared<QueryNode>();
            queryNode->data_source_id = node_json.at("data_source_id").get<std::string>();
            queryNode->query_statement = node_json.at("query_statement").get<std::string>();
            if (node_json.contains("query_parameters") && node_json["query_parameters"].is_array()) {
                for (const auto& param : node_json["query_parameters"]) {
                    queryNode->query_parameters.push_back(param.get<std::string>());
                }
            }
            queryNode->output_context_key = node_json.at("output_context_key").get<std::string>();
            node = queryNode;
        }
        else if (type_str == "event") {
            auto eventNode = std::make_shared<EventEmitNode>();
            eventNode->name = node_json.at("name").get<std::string>();
            if (node_json.contains("payload")) {
                eventNode->payload = node_json["payload"].get<std::map<std::string, std::string>>();
            }
            if (node_json.contains("next")) {
                eventNode->next = node_json["next"].get<std::string>();
            }
            node = eventNode;
        }
        else if (type_str == "schedule") {
            auto scheduleNode = std::make_shared<ScheduleNode>();
            scheduleNode->cron = node_json.at("cron").get<std::string>();
            if (node_json.contains("timezone")) {
                scheduleNode->timezone = node_json["timezone"].get<std::string>();
            }
            scheduleNode->body = node_json.at("body").get<std::string>();
            scheduleNode->enabled = node_json.value("enabled", true);
            node = scheduleNode;
        }
        else if (type_str == "subflow") {
            auto subflowNode = std::make_shared<SubflowNode>();
            subflowNode->workflow = node_json.at("workflow").get<std::string>();
            subflowNode->wait_for_completion = node_json.value("wait_for_completion", true);
            if (node_json.contains("inputs")) {
                subflowNode->inputs = node_json["inputs"].get<std::map<std::string, std::string>>();
            }
            if (node_json.contains("output")) {
                subflowNode->output = node_json["output"].get<std::string>();
            }
            node = subflowNode;
        }
        else if (type_str == "terminate") {
            auto terminateNode = std::make_shared<TerminateNode>();
            terminateNode->status = node_json.at("status").get<std::string>();
            if (node_json.contains("error_code")) {
                terminateNode->error_code = node_json["error_code"].get<std::string>();
            }
            if (node_json.contains("message")) {
                terminateNode->message = parseExpression(node_json["message"]);
            }
            node = terminateNode;
        }
        else if (type_str == "condition") {
            auto conditionNode = std::make_shared<ConditionNode>();
            conditionNode->expression = node_json.at("expression").get<std::string>();
            conditionNode->on_true = node_json.at("on_true").get<std::string>();
            if (node_json.contains("on_false")) {
                conditionNode->on_false = node_json["on_false"].get<std::string>();
            }
            node = conditionNode;
        }
        else if (type_str == "choice") {
            auto choiceNode = std::make_shared<ChoiceNode>();
            choiceNode->variable_key = node_json.at("variable_key").get<std::string>();
            choiceNode->cases = node_json.at("cases").get<std::map<std::string, std::string>>();
            if (node_json.contains("default_target")) {
                choiceNode->default_target = node_json["default_target"].get<std::string>();
            }
            node = choiceNode;
        }
        else if (type_str == "action") {
            auto actionNode = std::make_shared<ActionNode>();
            
            if (node_json.contains("capability")) {
                actionNode->capability = node_json.at("capability").get<std::string>();
            }

            if (node_json.contains("prompt_config") && node_json["prompt_config"].is_object()) {
                ActionNode::PromptConfig pConfig;
                auto& pc_json = node_json["prompt_config"];
                if (pc_json.contains("message")) {
                    pConfig.message = parseExpression(pc_json["message"]);
                }
                if (pc_json.contains("interaction_type")) {
                    pConfig.interaction_type = pc_json["interaction_type"].get<std::string>();
                }
                if (pc_json.contains("input_validation")) {
                    pConfig.input_validation = parseExpression(pc_json["input_validation"]);
                }
                if (pc_json.contains("timeout_ms")) {
                    pConfig.timeout_ms = pc_json["timeout_ms"].get<int>();
                }
                actionNode->prompt_config = pConfig;
            }
            if (node_json.contains("on_event") && node_json["on_event"].is_object()) {
                ActionNode::SystemEvent event;
                event.event_name = node_json["on_event"].at("event_name").get<std::string>();
                if (node_json["on_event"].contains("trigger_condition")) {
                    event.trigger_condition = parseExpression(node_json["on_event"]["trigger_condition"]);
                }
                actionNode->on_event = event;
            }

            if (node_json.contains("data_sources") && node_json["data_sources"].is_array()) {
                for (const auto& ds_json : node_json["data_sources"]) {
                    ActionNode::DataSource ds;
                    ds.name = ds_json.at("name").get<std::string>();
                    ds.scope = ds_json.at("scope").get<std::string>();
                    ds.provider = ds_json.at("provider").get<std::string>();
                    if (ds_json.contains("config") && ds_json["config"].is_object()) {
                        ds.config = ds_json["config"].get<std::map<std::string, std::string>>();
                    }
                    actionNode->data_sources.push_back(ds);
                }
            }

            if (node_json.contains("inputs") && node_json["inputs"].is_object()) {
                for (auto it = node_json["inputs"].begin(); it != node_json["inputs"].end(); ++it) {
                    if (it.value().is_string()) {
                        actionNode->inputs[it.key()] = it.value().get<std::string>();
                    } else if (it.value().is_object()) {
                        actionNode->inputs[it.key()] = it.value().get<std::map<std::string, std::string>>();
                    }
                }
            }

            if (node_json.contains("outputs") && node_json["outputs"].is_object()) {
                actionNode->outputs = node_json["outputs"].get<std::map<std::string, std::string>>();
            }

            if (node_json.contains("next_tasks") && node_json["next_tasks"].is_array()) {
                for (const auto& task_json : node_json["next_tasks"]) {
                    ActionNode::TaskReference ref;
                    ref.task_id = task_json.at("task_id").get<std::string>();
                    if (task_json.contains("execute_condition")) {
                        ref.execute_condition = parseExpression(task_json["execute_condition"]);
                    }
                    actionNode->next_tasks.push_back(ref);
                }
            }
            node = actionNode;
        }

        // Map ID if present
        if (node && node_json.contains("id")) {
            node->id = node_json["id"].get<std::string>();
        }

        if (!node) {
            throw std::runtime_error("Unknown task type: " + type_str);
        }
        return node;
    }

    WorkflowAST WorkflowParser::parse(const std::string& json_content) {
        WorkflowAST ast;
        auto root_json = json::parse(json_content);

        if (!root_json.is_array()) throw std::runtime_error("Root must be an array.");

        for (const auto& node_json : root_json) {
            ast.push_back(parseSingleNode(node_json));
        }
        return ast;
    }

} // namespace sapo