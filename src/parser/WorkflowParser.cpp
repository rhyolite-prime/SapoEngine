//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//
//  Grammar v1 parser: JSON blueprint → validated AST.
//
//  Design notes
//    • `Fields` reads a node object with alias support and records which keys
//      were consumed, so unknown/misspelled fields become warnings (or errors in
//      strict mode) instead of silently ignored configuration.
//    • Expressions are compiled here, once. A malformed expression is a parse
//      error, never a runtime surprise.
//    • Inline node bodies (`if.then: [...]`, `loop.body: [...]`, `try.body: [...]`)
//      are hoisted into the graph with generated ids and referenced by id from
//      the parent, so the interpreter only ever deals with a flat node list.
//    • Unknown `command`/`capability` references are rejected here (fail closed):
//      Sapo has no shell execution path at all.
//
#include "parser/WorkflowParser.hpp"
#include "capabilities/CapabilityRegistry.hpp"
#include "parser/BlueprintValidator.hpp"
#include "runtime/SapoError.hpp"
#include "runtime/Scheduler.hpp"
#include "runtime/expressions/Sel.hpp"
#include "util/TimeUtils.hpp"

#include <algorithm>
#include <cctype>
#include <set>

using json = nlohmann::json;

namespace sapo::parser {

    namespace {

        [[noreturn]] void reject(const std::string &message) {
            throw runtime::SapoError(runtime::ErrorCode::Parse, message);
        }

        std::string lower(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        std::string stripSigil(std::string name) {
            if (!name.empty() && name.front() == '$') name.erase(0, 1);
            return name;
        }

        NodePtr parseNode(const json &node_json, const std::string &auto_id, const ParseOptions &options,
                          std::vector<std::string> &warnings, class BodyExpander &expander);

        /// Top-level document string field (tolerates numbers/booleans).
        std::string docString(const json &document, const char *key, std::string fallback = "") {
            auto it = document.find(key);
            if (it == document.end() || it->is_null()) return fallback;
            if (it->is_string()) return it->get<std::string>();
            if (it->is_number() || it->is_boolean()) return it->dump();
            return fallback;
        }

        /// Reads a node's JSON object with alias support + consumed-key tracking.
        class Fields {
        public:
            Fields(const json &object, std::set<std::string> &used_keys) : m_json(object), m_used(used_keys) {}

            [[nodiscard]] const json *find(std::initializer_list<const char *> names) const {
                for (const char *name : names) {
                    auto it = m_json.find(name);
                    if (it != m_json.end()) {
                        m_used.emplace(name);
                        return &(*it);
                    }
                }
                return nullptr;
            }

            [[nodiscard]] bool has(std::initializer_list<const char *> names) const { return find(names) != nullptr; }

            [[nodiscard]] std::optional<std::string> optionalString(std::initializer_list<const char *> names) const {
                const json *value = find(names);
                if (value == nullptr || value->is_null()) return std::nullopt;
                if (value->is_string()) return value->get<std::string>();
                return value->dump();
            }

            [[nodiscard]] std::string stringOr(std::string fallback, std::initializer_list<const char *> names) const {
                auto value = optionalString(names);
                return value.value_or(std::move(fallback));
            }

            [[nodiscard]] std::string requireString(std::initializer_list<const char *> names,
                                                    const std::string &node_label) const {
                auto value = optionalString(names);
                if (!value.has_value()) {
                    std::string expected;
                    for (const char *name : names) {
                        if (!expected.empty()) expected += "' / '";
                        expected += name;
                    }
                    reject("'" + node_label + "' is missing the required field '" + expected + "'");
                }
                return *value;
            }

            [[nodiscard]] int intOr(int fallback, std::initializer_list<const char *> names) const {
                const json *value = find(names);
                return (value != nullptr && value->is_number()) ? value->get<int>() : fallback;
            }

            [[nodiscard]] int64_t int64Or(int64_t fallback, std::initializer_list<const char *> names) const {
                const json *value = find(names);
                return (value != nullptr && value->is_number()) ? value->get<int64_t>() : fallback;
            }

            /// A duration in milliseconds: a number is already milliseconds, a string
            /// may be `"90s"`, `"2m"`, `"1500ms"` or bare seconds.
            [[nodiscard]] std::optional<int64_t> durationMs(std::initializer_list<const char *> names) const {
                const json *value = find(names);
                if (value == nullptr || value->is_null()) return std::nullopt;
                if (value->is_number()) return value->get<int64_t>();
                if (value->is_string()) {
                    const std::string text = value->get<std::string>();
                    if (auto delay = util::parseDuration(text); delay.has_value()) return delay->count();
                    if (auto relative = util::parseRelativeDelay(text); relative.has_value()) return relative->count();
                    try {
                        return std::stoll(text);
                    } catch (const std::exception &) {
                        return std::nullopt;
                    }
                }
                return std::nullopt;
            }

            [[nodiscard]] double doubleOr(double fallback, std::initializer_list<const char *> names) const {
                const json *value = find(names);
                return (value != nullptr && value->is_number()) ? value->get<double>() : fallback;
            }

            [[nodiscard]] bool boolOr(bool fallback, std::initializer_list<const char *> names) const {
                const json *value = find(names);
                if (value == nullptr) return fallback;
                if (value->is_boolean()) return value->get<bool>();
                if (value->is_number()) return value->get<double>() != 0.0;
                if (value->is_string()) {
                    const std::string text = lower(value->get<std::string>());
                    return text == "true" || text == "yes" || text == "1";
                }
                return fallback;
            }

            /// Template value: returned verbatim so nested JSON structure survives.
            [[nodiscard]] json templateOr(json fallback, std::initializer_list<const char *> names) const {
                const json *value = find(names);
                return value != nullptr ? *value : fallback;
            }

            [[nodiscard]] Expression expression(std::initializer_list<const char *> names) const {
                const json *value = find(names);
                return value == nullptr ? Expression() : toExpression(*value);
            }

            [[nodiscard]] std::optional<Expression> optionalExpression(std::initializer_list<const char *> names) const {
                const json *value = find(names);
                if (value == nullptr || value->is_null()) return std::nullopt;
                return toExpression(*value);
            }

            /// Predicate fields (`condition`, `when`, `while`, `input_validation`, …)
            /// are SEL *programs*: `amount > 100` is a comparison, never the literal
            /// text "amount > 100". Structured `{left, operator, right}` objects keep
            /// their canonical form, so both spellings of the grammar work.
            [[nodiscard]] Expression predicate(std::initializer_list<const char *> names) const {
                const json *value = find(names);
                return value == nullptr ? Expression() : toPredicate(*value);
            }

            [[nodiscard]] std::optional<Expression> optionalPredicate(std::initializer_list<const char *> names) const {
                const json *value = find(names);
                if (value == nullptr || value->is_null()) return std::nullopt;
                return toPredicate(*value);
            }

            /// Like `predicate`, but a source that is not valid SEL stays a template
            /// (`wait.until` also accepts an absolute timestamp or a `$var` holding one).
            [[nodiscard]] Expression softPredicate(std::initializer_list<const char *> names) const {
                const json *value = find(names);
                if (value == nullptr || value->is_null()) return Expression();
                if (!value->is_string()) return toExpression(*value);
                const std::string text = value->get<std::string>();
                try {
                    return Expression::fromExpression(text);
                } catch (const runtime::SapoError &) {
                    return Expression(text);
                }
            }

            [[nodiscard]] static Expression toPredicate(const json &value) {
                if (value.is_string()) return Expression::fromExpression(value.get<std::string>());
                return toExpression(value);
            }

            [[nodiscard]] std::vector<std::string> stringList(std::initializer_list<const char *> names) const {
                std::vector<std::string> out;
                const json *value = find(names);
                if (value == nullptr) return out;
                if (value->is_array()) {
                    for (const auto &item : *value) {
                        out.push_back(item.is_string() ? item.get<std::string>() : item.dump());
                    }
                } else if (value->is_string()) {
                    out.push_back(value->get<std::string>());
                }
                return out;
            }

            [[nodiscard]] std::map<std::string, std::string> stringMap(std::initializer_list<const char *> names) const {
                std::map<std::string, std::string> out;
                const json *value = find(names);
                if (value == nullptr || !value->is_object()) return out;
                for (auto it = value->begin(); it != value->end(); ++it) {
                    out[it.key()] = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
                }
                return out;
            }

            /// String | number | bool | {left,operator,right} → compiled expression.
            [[nodiscard]] static Expression toExpression(const json &value) {
                if (value.is_string()) return Expression(value.get<std::string>());
                if (value.is_object() || value.is_array() || value.is_number() || value.is_boolean()) {
                    return Expression::fromJson(value);
                }
                return Expression();
            }

        private:
            const json &m_json;
            std::set<std::string> &m_used;
        };

        /// Turns a "body"-shaped value (node ids, or inline node objects) into ids.
        /// Inline objects are parsed and hoisted into the workflow.
        class BodyExpander {
        public:
            BodyExpander(std::vector<NodePtr> &hoisted, std::vector<std::string> &warnings, const ParseOptions &options)
                : m_hoisted(hoisted), m_warnings(warnings), m_options(options) {}

            [[nodiscard]] std::vector<std::string> expand(const json &value, const std::string &parent_id,
                                                           const char *slot) const {
                std::vector<std::string> ids;
                if (value.is_string()) {
                    ids.push_back(value.get<std::string>());
                    return ids;
                }
                if (!value.is_array()) return ids;
                size_t counter = 0;
                for (const auto &item : value) {
                    if (item.is_string()) {
                        ids.push_back(item.get<std::string>());
                        continue;
                    }
                    if (!item.is_object()) {
                        reject("node '" + parent_id + "': entries in '" + slot + "' must be node objects or node ids");
                    }
                    auto child = parseNode(item, parent_id + "." + slot + "_" + std::to_string(counter++), m_options,
                                           m_warnings, const_cast<BodyExpander &>(*this));
                    ids.push_back(child->id);
                    m_hoisted.push_back(std::move(child));
                }
                return ids;
            }

            void hoist(NodePtr node) const { m_hoisted.push_back(std::move(node)); }

        private:
            std::vector<NodePtr> &m_hoisted;
            std::vector<std::string> &m_warnings;
            const ParseOptions &m_options;
        };

        RetryPolicy parseRetry(const json &value) {
            RetryPolicy policy;
            if (value.is_number_integer()) {
                policy.max_attempts = std::max(1, value.get<int>());
                return policy;
            }
            if (!value.is_object()) return policy;
            std::set<std::string> used;
            Fields fields(value, used);
            policy.max_attempts = std::max(1, fields.intOr(3, {"max_attempts", "attempts", "retries"}));
            policy.backoff_ms = fields.int64Or(250, {"backoff_ms", "backoff", "delay_ms"});
            policy.multiplier = fields.doubleOr(2.0, {"multiplier", "backoff_multiplier"});
            policy.jitter = fields.doubleOr(0.2, {"jitter"});
            policy.max_backoff_ms = fields.int64Or(15000, {"max_backoff_ms"});
            policy.retry_on = fields.stringList({"retry_on", "retry_if", "retry_on_status"});
            return policy;
        }

        std::string normalizeStatus(std::string status) {
            status = lower(std::move(status));
            if (status == "failure" || status == "error" || status == "fail") return "failed";
            if (status == "ok" || status == "succeeded" || status == "complete" || status == "completed")
                return "success";
            if (status == "canceled") return "cancelled";
            if (status != "success" && status != "failed" && status != "cancelled") return "failed";
            return status;
        }

        // -----------------------------------------------------------------
        // Per-type builders
        // -----------------------------------------------------------------
        NodePtr buildNoop(Fields &fields) {
            auto node = std::make_shared<NoopNode>();
            node->meta = fields.templateOr(json::object(), {"meta"});
            node->assign = fields.templateOr(json::object(), {"assign", "set", "variables"});
            return node;
        }

        NodePtr buildTerminate(Fields &fields) {
            auto node = std::make_shared<TerminateNode>();
            node->status = normalizeStatus(fields.stringOr("success", {"status", "result"}));
            if (auto code = fields.optionalString({"error_code", "errorCode", "code"}); code.has_value()) {
                node->error_code = *code;
            }
            node->message = fields.optionalExpression({"message", "text", "output_message"});
            node->output = fields.templateOr(json::object(), {"output", "outputs", "result_data", "data"});
            return node;
        }

        NodePtr buildScript(Fields &fields, const std::string &id) {
            auto node = std::make_shared<ScriptNode>();
            node->language = lower(fields.stringOr("sel", {"language", "engine", "runtime"}));
            if (node->language != "sel" && node->language != "expr") {
                reject("script node '" + id + "' declares unsupported language '" + node->language +
                       "'; grammar v1 supports 'sel' and 'expr' (a Lua sandbox is a documented deferral, "
                       "see docs/LIMITATIONS.md)");
            }
            if (fields.has({"code", "expr", "source"})) {
                // A script body is a program, not an interpolation template.
                node->code = fields.predicate({"code", "expr", "source"});
            } else {
                node->code = fields.predicate({"script"});
            }
            if (node->code.empty()) reject("script node '" + id + "' needs 'code'");
            node->output = fields.optionalString({"output", "output_key", "save_to"});
            node->bindings = fields.stringMap({"bindings", "locals"});
            return node;
        }

        NodePtr buildTransform(Fields &fields, const std::string &id) {
            auto node = std::make_shared<TransformNode>();
            node->operation = lower(fields.stringOr("assign", {"operation", "op", "mode"}));
            node->input = fields.expression({"input", "source", "from"});
            node->mapping = fields.templateOr(json::object(), {"mapping", "map", "template"});
            node->predicate = fields.predicate({"predicate", "where", "filter"});
            node->output = fields.stringOr("", {"output", "output_key", "save_to", "to"});
            if (node->output.empty()) reject("transform node '" + id + "' requires an 'output' key");
            node->item_variable = stripSigil(fields.stringOr("item", {"item_variable", "as", "iterator"}));
            node->index_variable = stripSigil(fields.stringOr("index", {"index_variable"}));
            node->output_as_array = fields.boolOr(false, {"as_array", "output_as_array"});
            static const std::vector<std::string> kOperations = {"assign",  "filter", "map",     "project",
                                                                 "merge",   "group",  "sort",    "flatten",
                                                                 "reduce",  "set",    "copy"};
            if (std::find(kOperations.begin(), kOperations.end(), node->operation) == kOperations.end()) {
                std::string allowed;
                for (const auto &op : kOperations) {
                    if (!allowed.empty()) allowed += "/";
                    allowed += op;
                }
                reject("transform node '" + id + "' uses unknown operation '" + node->operation +
                       "'; expected one of " + allowed);
            }
            if ((node->operation == "filter" || node->operation == "map" || node->operation == "reduce") &&
                node->input.empty()) {
                reject("transform node '" + id + "' with operation '" + node->operation + "' requires an 'input'");
            }
            if (node->operation == "project" && (!node->mapping.is_object() || node->mapping.empty())) {
                reject("transform node '" + id + "' with operation 'project' needs a 'mapping' object");
            }
            return node;
        }

        NodePtr buildQuery(Fields &fields, const std::string &id) {
            auto node = std::make_shared<QueryNode>();
            node->source = fields.stringOr("", {"source", "data_source_id", "source_id", "from"});
            if (node->source.empty()) reject("query node '" + id + "' requires a 'source' data-source id");
            node->filter = fields.templateOr(json(), {"filter", "where", "match"});
            node->statement = fields.stringOr("", {"query_statement", "statement", "sql", "query"});
            node->parameters = fields.templateOr(json::object(), {"query_parameters", "parameters", "params"});
            if (auto limit = fields.intOr(-1, {"limit"}); limit >= 0) node->limit = limit;
            if (auto offset = fields.intOr(-1, {"offset", "skip"}); offset >= 0) node->offset = offset;
            node->output = fields.stringOr("", {"output", "output_context_key", "save_to"});
            if (node->output.empty()) reject("query node '" + id + "' requires an 'output' key");
            node->output_context_key = node->output;
            if (!node->filter.is_null() && !node->filter.is_object() && !node->filter.is_string() &&
                !node->filter.is_array()) {
                reject("query node '" + id + "' 'filter' must be an object, array or expression string");
            }
            return node;
        }

        NodePtr buildCommand(Fields &fields, const std::string &id, const ParseOptions &options,
                             std::vector<std::string> &warnings) {
            auto node = std::make_shared<CommandNode>();
            node->command = fields.requireString({"command", "action"}, id);

            if (const json *http = fields.find({"http_request", "request", "http"}); http != nullptr && http->is_object()) {
                CommandNode::HttpRequestConfig config;
                std::set<std::string> http_used;
                Fields http_fields(*http, http_used);
                config.url = Expression(http_fields.requireString({"url", "endpoint"}, id + ".http_request"));
                if (const json *auth_json = http_fields.find({"auth", "authentication"});
                    auth_json != nullptr && auth_json->is_object()) {
                    std::set<std::string> auth_used;
                    Fields auth(*auth_json, auth_used);
                    CommandNode::HttpAuthConfig auth_config;
                    auth_config.type = lower(auth.stringOr("basic", {"type", "scheme"}));
                    auth_config.username = auth.expression({"username", "user"});
                    auth_config.password = auth.expression({"password", "pass", "secret"});
                    auth_config.token = auth.expression({"token", "api_key", "key"});
                    if (auth_config.type != "basic" && auth_config.type != "bearer") {
                        reject("command node '" + id + "': http auth type '" + auth_config.type +
                               "' is unsupported (basic | bearer)");
                    }
                    for (auto it = auth_json->begin(); it != auth_json->end(); ++it) {
                        if (!auth_used.count(it.key())) {
                            warnings.push_back("command '" + id + "': unused auth field '" + it.key() + "'");
                        }
                    }
                    config.auth = auth_config;
                }
                config.headers = http_fields.templateOr(json::object(), {"headers"});
                config.query = http_fields.templateOr(json::object(), {"query", "params", "query_parameters"});
                config.body = http_fields.templateOr(json(), {"body", "payload", "data"});
                if (auto timeout = http_fields.durationMs({"timeout", "timeout_ms"});
                    timeout.has_value() && *timeout >= 0) {
                    config.timeout = static_cast<int>(*timeout);
                }
                config.follow_redirects = http_fields.boolOr(true, {"follow_redirects"});
                config.content_type = http_fields.optionalString({"content_type"});
                for (auto it = http->begin(); it != http->end(); ++it) {
                    if (!http_used.count(it.key()) && it.key() != "url") {
                        warnings.push_back("command '" + id + "': unused http_request field '" + it.key() + "'");
                    }
                }
                node->http_request = config;
            }

            node->inputs = fields.templateOr(json::object(), {"inputs", "input", "args", "config"});

            if (const json *output = fields.find({"output", "outputs", "save_to", "as"}); output != nullptr) {
                if (output->is_string()) {
                    node->output = output->get<std::string>();
                } else if (output->is_array()) {
                    std::vector<std::string> paths;
                    for (const auto &item : *output) {
                        if (!item.is_string()) {
                            reject("command node '" + id + "': every 'output' entry must be a string path");
                        }
                        paths.push_back(item.get<std::string>());
                    }
                    node->output_paths = paths;
                } else if (output->is_object()) {
                    node->outputs = *output;
                }
            }

            if (const json *method = fields.find({"method"}); method != nullptr && method->is_string()) {
                if (!node->http_request.has_value()) node->http_request = CommandNode::HttpRequestConfig{};
                node->command = "http." + lower(method->get<std::string>());
            }

            const bool is_http = node->command.rfind("http.", 0) == 0;
            static const std::vector<std::string> kHttpMethods = {"http.get",   "http.post",  "http.put",
                                                                  "http.patch", "http.delete", "http.head"};
            if (is_http) {
                if (std::find(kHttpMethods.begin(), kHttpMethods.end(), node->command) == kHttpMethods.end()) {
                    reject("command '" + node->command + "' on node '" + id +
                           "' is not a supported HTTP verb (http.get/post/put/patch/delete/head)");
                }
                if (!node->http_request.has_value() || node->http_request->url.empty()) {
                    reject("command '" + node->command + "' on node '" + id +
                           "' requires an 'http_request' block with a 'url'");
                }
            } else {
                // Fail closed (plan §0, P0-2): unknown non-http commands are rejected
                // at parse time. There is no shell fallback anywhere in the engine, and
                // an unresolvable name must never degrade into a silent no-op.
                const bool registered = options.capabilities != nullptr && options.capabilities->has(node->command);
                if (!registered) {
                    reject("node '" + id + "' calls unknown command '" + node->command + "'. No capability provider "
                           "is registered for it" +
                           (options.capabilities == nullptr ? " (no capability registry was installed)" : "") +
                           " — Sapo never executes DSL commands through a shell. Register the plugin, or use an "
                           "http.* command.");
                }
            }
            return node;
        }

        NodePtr buildEvent(Fields &fields, const std::string &id) {
            auto node = std::make_shared<EventEmitNode>();
            node->name = fields.requireString({"name", "event", "event_name", "topic"}, id);
            node->payload = fields.templateOr(json::object(), {"payload", "data", "body"});
            node->publish_now = fields.boolOr(true, {"publish_now", "immediate"});
            return node;
        }

        NodePtr buildWait(Fields &fields, const std::string &id) {
            auto node = std::make_shared<WaitNode>();
            node->duration = fields.optionalString({"duration", "for", "delay"});
            node->until = fields.softPredicate({"until", "condition", "until_condition"});
            if (!node->duration.has_value() && node->until.empty()) {
                reject("wait node '" + id + "' needs either 'duration' or 'until'");
            }
            if (node->duration.has_value() && !node->until.empty()) {
                reject("wait node '" + id + "' cannot combine 'duration' and 'until'");
            }
            if (node->duration.has_value() && !util::parseDuration(*node->duration).has_value() &&
                !util::parseIso8601(*node->duration).has_value() &&
                !util::parseRelativeDelay(*node->duration).has_value()) {
                reject("wait node '" + id + "' has an unparseable duration '" + *node->duration +
                       "' (expected 30s, 10m, 2h, 1d, ISO-8601 or 'in 5 minutes')");
            }
            if (auto poll = fields.int64Or(-1, {"poll_interval_ms"}); poll > 0) node->poll_interval_ms = poll;
            node->durable = fields.boolOr(true, {"durable"});
            return node;
        }

        NodePtr buildSchedule(Fields &fields, const std::string &id) {
            auto node = std::make_shared<ScheduleNode>();
            node->cron = fields.stringOr("", {"cron", "schedule", "when", "every"});
            if (node->cron.empty()) reject("schedule node '" + id + "' requires 'cron' (or 'every')");
            node->timezone = fields.optionalString({"timezone", "tz"});
            node->workflow = fields.optionalString({"workflow", "workflow_id"});
            node->job_id = fields.optionalString({"job_id", "name"});
            node->input = fields.templateOr(json::object(), {"input", "inputs", "payload"});
            node->enabled = fields.boolOr(true, {"enabled"});
            node->body = fields.stringOr("", {"body", "target", "task", "task_id"});
            if (!runtime::isValidCronExpression(node->cron)) {
                reject("schedule node '" + id + "' has an invalid schedule '" + node->cron +
                       "' (expected 5/6-field cron, @hourly/@daily/@weekly/@monthly/@yearly, or 'in 5 minutes')");
            }
            if (node->body.empty() && !node->workflow.has_value()) {
                reject("schedule node '" + id + "' needs a 'body' target (node id) or a 'workflow' id to start");
            }
            return node;
        }

        NodePtr buildSubflow(Fields &fields, const std::string &id) {
            auto node = std::make_shared<SubflowNode>();
            node->workflow = fields.requireString({"workflow", "workflow_id", "call", "blueprint"}, id);
            node->wait_for_completion = fields.boolOr(true, {"wait_for_completion", "await", "wait"});
            node->inputs = fields.templateOr(json::object(), {"inputs", "input", "params"});
            node->output = fields.optionalString({"output", "output_key", "save_to", "as"});
            node->return_map = fields.templateOr(json::object(), {"return", "return_map", "returns"});
            node->max_depth = fields.intOr(8, {"max_depth"});
            if (node->max_depth < 1) reject("subflow node '" + id + "' max_depth must be >= 1");
            return node;
        }

        NodePtr buildCondition(Fields &fields, const json &node_json, const std::string &id,
                               BodyExpander &expander) {
            auto node = std::make_shared<ConditionNode>();
            node->expression = fields.predicate({"expression", "condition", "when"});
            if (node->expression.empty()) node->expression = fields.predicate({"if"});
            if (node->expression.empty()) {
                reject("condition node '" + id + "' needs an 'expression' (or an 'if' / 'condition' string)");
            }

            if (const json *then_value = fields.find({"then", "then_body", "then_nodes", "on_true", "onTrue"});
                then_value != nullptr) {
                if (then_value->is_array() && !then_value->empty()) {
                    node->then_body = expander.expand(*then_value, id, "then");
                } else if (then_value->is_string()) {
                    node->on_true = then_value->get<std::string>();
                }
            }
            if (const json *else_value = fields.find({"else", "else_body", "else_nodes", "on_false", "onFalse"});
                else_value != nullptr) {
                if (else_value->is_array() && !else_value->empty()) {
                    node->else_body = expander.expand(*else_value, id, "else");
                } else if (else_value->is_string()) {
                    node->on_false = else_value->get<std::string>();
                }
            }
            if (node->on_true.empty() && node->then_body.empty()) {
                reject("condition node '" + id + "' needs 'on_true' (or a non-empty 'then' body)");
            }
            (void) node_json;
            return node;
        }

        NodePtr buildChoice(Fields &fields, const std::string &id) {
            auto node = std::make_shared<ChoiceNode>();
            node->expression = fields.expression({"expression", "variable_key", "selector", "switch"});
            if (node->expression.empty()) reject("choice node '" + id + "' needs an 'expression'");
            node->cases = fields.stringMap({"cases", "branches", "options"});
            node->default_target = fields.optionalString({"default", "default_target", "fallback"});
            if (node->cases.empty()) reject("choice node '" + id + "' needs a non-empty 'cases' map");
            return node;
        }

        NodePtr buildParallel(Fields &fields, const std::string &id, const ParseOptions &options,
                              std::vector<std::string> &warnings, BodyExpander &expander) {
            auto node = std::make_shared<ParallelNode>();
            // `child_tasks`: inline node objects, or {"tasks": [...]}/id arrays per branch.
            if (const json *children = fields.find({"child_tasks", "children"}); children != nullptr) {
                if (!children->is_array()) reject("parallel node '" + id + "': 'child_tasks' must be an array");
                size_t counter = 0;
                for (const auto &child : *children) {
                    ParallelNode::Branch branch;
                    if (child.is_string()) {
                        branch.node_ids.push_back(child.get<std::string>());
                    } else if (child.is_object() && (child.contains("tasks") || child.contains("body"))) {
                        std::set<std::string> branch_used;
                        Fields branch_fields(child, branch_used);
                        if (auto label = branch_fields.optionalString({"id", "label", "name"}); label.has_value()) {
                            branch.label = *label;
                        }
                        (void) 0;
                        branch.node_ids = expander.expand(branch_fields.templateOr(json::array(), {"tasks", "body"}),
                                                            id, "branch");
                        for (auto it = child.begin(); it != child.end(); ++it) {
                            if (!branch_used.count(it.key()) && it.key() != "tasks" && it.key() != "body") {
                                warnings.push_back("parallel node '" + id + "': unread child_tasks entry field '" +
                                                   it.key() + "'");
                            }
                        }
                    } else if (child.is_object()) {
                        const std::string child_id =
                            child.contains("id") && child["id"].is_string()
                                ? child["id"].get<std::string>()
                                : id + ".branch_" + std::to_string(counter);
                        branch.label = branch.label.value_or(child_id);
                        branch.node_ids.push_back(child_id);
                        expander.hoist(parseNode(child, child_id, options, warnings, expander));
                    } else {
                        reject("parallel node '" + id + "': child_tasks entries must be nodes, ids or {tasks:[…]}");
                    }
                    if (!branch.node_ids.empty()) node->branches.push_back(std::move(branch));
                    ++counter;
                }
            }
            // `tasks`: each listed node id becomes its own concurrent branch.
            for (const auto &task_ref : fields.stringList({"tasks", "task_refs", "branches"})) {
                ParallelNode::Branch branch;
                branch.node_ids.push_back(task_ref);
                node->branches.push_back(std::move(branch));
            }
            node->fail_fast = fields.boolOr(true, {"fail_fast", "failFast"});
            node->merge_policy = fields.stringOr("last_writer_wins", {"merge_policy", "join"});
            node->max_concurrency = fields.intOr(0, {"max_concurrency", "concurrency"});
            if (node->branches.empty()) {
                reject("parallel node '" + id + "' needs 'child_tasks' (inline nodes) or 'tasks' (node ids)");
            }
            if (node->max_concurrency < 0) reject("parallel node '" + id + "' max_concurrency cannot be negative");
            static const std::vector<std::string> kPolicies = {"last_writer_wins", "skip_conflicts", "fail_conflicts"};
            if (std::find(kPolicies.begin(), kPolicies.end(), node->merge_policy) == kPolicies.end()) {
                reject("parallel node '" + id + "' has unknown merge_policy '" + node->merge_policy +
                       "'; expected last_writer_wins | skip_conflicts | fail_conflicts");
            }
            (void) options;
            (void) warnings;
            return node;
        }

        NodePtr buildAction(Fields &fields, const std::string &id, const ParseOptions &options,
                            std::vector<std::string> &warnings) {
            auto node = std::make_shared<ActionNode>();
            node->capability = fields.stringOr("", {"capability", "command"});
            node->await_input = fields.boolOr(true, {"await_input", "await", "suspend"});

            if (const json *prompt = fields.find({"prompt_config", "prompt"}); prompt != nullptr) {
                ActionNode::PromptConfig config;
                if (prompt->is_string()) {
                    config.message = Expression(prompt->get<std::string>());
                } else if (prompt->is_object()) {
                    std::set<std::string> prompt_used;
                    Fields prompt_fields(*prompt, prompt_used);
                    config.message = prompt_fields.expression({"message", "text"});
                    config.interaction_type = lower(prompt_fields.stringOr("input", {"interaction_type", "type", "mode"}));
                    config.input_validation = prompt_fields.optionalPredicate({"input_validation", "validation"});
                    if (auto timeout = prompt_fields.durationMs({"timeout", "timeout_ms"});
                        timeout.has_value() && *timeout >= 0) {
                        config.timeout_ms = static_cast<int>(*timeout);
                    }
                    config.output = prompt_fields.optionalString({"output", "save_to", "input_variable"});
                    if (const json *options = prompt_fields.find({"options", "dynamic_options"}); options != nullptr) {
                        if (!options->is_object()) reject("action node '" + id + "' prompt_config.options must be an object");
                        config.options = *options;
                    }
                    for (auto it = prompt->begin(); it != prompt->end(); ++it) {
                        if (!prompt_used.count(it.key())) {
                            warnings.push_back("action node '" + id + "': unused prompt_config field '" + it.key() + "'");
                        }
                    }
                } else {
                    reject("action node '" + id + "' prompt_config must be a string or an object");
                }
                static const std::vector<std::string> kInteractions = {"display", "input", "menu", "pin"};
                if (std::find(kInteractions.begin(), kInteractions.end(), config.interaction_type) == kInteractions.end()) {
                    reject("action node '" + id + "' prompt_config.interaction_type must be display|input|menu|pin, got '" +
                           config.interaction_type + "'");
                }
                if (config.message.empty()) reject("action node '" + id + "' prompt_config needs a 'message'");
                node->prompt_config = config;
            }

            node->input_variable = fields.optionalString({"input_variable"});
            if (!node->input_variable.has_value() && node->prompt_config.has_value()) {
                node->input_variable = node->prompt_config->output;
            }

            if (const json *event = fields.find({"on_event"}); event != nullptr && !event->is_null()) {
              if (event->is_string()) {
                // `"on_event": "bank.transfer.confirmed"` is the shorthand spelling.
                ActionNode::SystemEvent shorthand;
                shorthand.event_name = event->get<std::string>();
                node->on_event = shorthand;
              } else if (event->is_object()) {
                ActionNode::SystemEvent system_event;
                std::set<std::string> event_used;
                Fields event_fields(*event, event_used);
                system_event.event_name = event_fields.requireString({"event_name", "event", "name"}, id + ".on_event");
                system_event.trigger_condition = event_fields.optionalPredicate({"trigger_condition", "when", "condition"});
                system_event.handler = event_fields.optionalString({"handler", "target", "jump_to"});
                for (auto it = event->begin(); it != event->end(); ++it) {
                    if (!event_used.count(it.key())) {
                        warnings.push_back("action node '" + id + "': unused on_event field '" + it.key() + "'");
                    }
                }
                node->on_event = system_event;
              } else {
                reject("action node '" + id + "': 'on_event' must be an event name or an object with 'event_name'");
              }
            }

            if (const json *sources = fields.find({"data_sources"}); sources != nullptr && sources->is_array()) {
                for (const auto &source : *sources) {
                    if (!source.is_object()) reject("action node '" + id + "': data_sources entries must be objects");
                    ActionNode::DataSource data_source;
                    std::set<std::string> source_used;
                    Fields source_fields(source, source_used);
                    data_source.name = source_fields.requireString({"name", "id"}, id + ".data_sources[]");
                    data_source.scope = lower(source_fields.stringOr("internal", {"scope"}));
                    data_source.provider = lower(source_fields.stringOr("", {"provider", "type"}));
                    data_source.config = source_fields.templateOr(json::object(), {"config", "settings"});
                    if (data_source.provider.empty()) {
                        reject("action node '" + id + "' declares data source '" + data_source.name +
                               "' without a 'provider'");
                    }
                    if (data_source.scope != "internal" && data_source.scope != "external") {
                        reject("data source '" + data_source.name + "' scope must be 'internal' or 'external'");
                    }
                    for (auto it = source.begin(); it != source.end(); ++it) {
                        if (!source_used.count(it.key())) {
                            warnings.push_back("action node '" + id + "': unused data_sources field '" + it.key() + "'");
                        }
                    }
                    node->data_sources.push_back(std::move(data_source));
                }
            }

            node->inputs = fields.templateOr(json::object(), {"inputs", "input", "args", "params"});
            node->outputs = fields.templateOr(json::object(), {"outputs", "output_map", "returns"});
            // `output` names where the capability result goes: an object maps context
            // keys to result paths ({"sms_id": "$.message_id"}), a string stores the
            // whole payload under that key.
            if (node->outputs.empty()) {
                if (const json *out = fields.find({"output"}); out != nullptr) {
                    if (out->is_object()) {
                        node->outputs = *out;
                    } else if (out->is_string()) {
                        node->outputs = json{{out->get<std::string>(), "$"}};
                    } else {
                        reject("action node '" + id +
                               "': 'output' must name a context key or map keys to result paths");
                    }
                }
            }

            if (const json *next_tasks = fields.find({"next_tasks"}); next_tasks != nullptr && next_tasks->is_array()) {
                for (const auto &item : *next_tasks) {
                    ActionNode::TaskReference reference;
                    if (item.is_string()) {
                        reference.task_id = item.get<std::string>();
                    } else if (item.is_object()) {
                        std::set<std::string> ref_used;
                        Fields ref_fields(item, ref_used);
                        reference.task_id = ref_fields.requireString({"task_id", "node", "target"}, id + ".next_tasks[]");
                        reference.execute_condition = ref_fields.optionalPredicate({"execute_condition", "when", "condition"});
                    } else {
                        reject("action node '" + id + "': next_tasks entries must be node ids or objects");
                    }
                    node->next_tasks.push_back(std::move(reference));
                }
            }

            if (!node->capability.empty()) {
                const bool registered = options.capabilities != nullptr && options.capabilities->has(node->capability);
                if (!registered) {
                    reject("action node '" + id + "' references unknown capability '" + node->capability + "'. " +
                           (options.capabilities == nullptr ? "No capability registry was installed" : "No provider is registered for it") +
                           " — see docs/CAPABILITIES.md / docs/PLUGIN_AUTHORING.md");
                }
            }
            if (node->capability.empty() && !node->prompt_config.has_value() && !node->on_event.has_value() &&
                node->next_tasks.empty() && node->data_sources.empty()) {
                reject("action node '" + id + "' is inert: it declares no capability, prompt, event or next_tasks");
            }
            return node;
        }

        NodePtr buildLoop(Fields &fields, const std::string &id, BodyExpander &expander) {
            auto node = std::make_shared<LoopNode>();
            node->collection = fields.expression({"collection", "items", "over", "each"});
            node->count = fields.expression({"count", "times", "repeat"});
            node->condition = fields.predicate({"while", "condition", "until_done"});
            node->iterator = stripSigil(fields.stringOr("item", {"iterator", "as", "item_variable"}));
            node->index = stripSigil(fields.stringOr("index", {"index", "index_variable", "index_name"}));
            if (const json *body = fields.find({"body", "steps", "tasks"}); body != nullptr) {
                node->body = expander.expand(*body, id, "body");
            }
            node->max_iterations = fields.intOr(1000, {"max_iterations", "max", "iteration_limit"});
            node->on_item_error = lower(fields.stringOr("fail", {"on_item_error", "error_handling"}));
            if (node->on_item_error != "fail" && node->on_item_error != "continue" && node->on_item_error != "retry") {
                reject("loop node '" + id + "' on_item_error must be fail|continue|retry");
            }
            if (node->collection.empty() && node->count.empty() && node->condition.empty()) {
                reject("loop node '" + id + "' needs 'collection', 'count' or 'while'");
            }
            if (node->body.empty()) reject("loop node '" + id + "' needs a non-empty 'body'");
            if (node->max_iterations < 1) reject("loop node '" + id + "' max_iterations must be >= 1");
            return node;
        }

        NodePtr buildLoopControl(Fields &fields, const std::string &type, const std::string &id) {
            auto node = std::make_shared<LoopControlNode>();
            node->action = type == "break" ? "break"
                                           : (type == "continue" ? "continue"
                                                                  : lower(fields.stringOr("continue", {"action"})));
            if (node->action != "break" && node->action != "continue") {
                reject("loop_control node '" + id + "' action must be 'break' or 'continue'");
            }
            node->when = fields.optionalPredicate({"when", "if"});
            node->loop = fields.optionalString({"loop", "loop_id"});
            return node;
        }

        NodePtr buildTry(Fields &fields, const std::string &id, BodyExpander &expander) {
            auto node = std::make_shared<TryNode>();
            if (const json *body = fields.find({"body", "do", "steps"}); body != nullptr) {
                node->body = expander.expand(*body, id, "try");
            }
            if (const json *catch_block = fields.find({"catch", "error_body"}); catch_block != nullptr) {
                if (catch_block->is_object()) {
                    std::set<std::string> catch_used;
                    Fields catch_fields(*catch_block, catch_used);
                    node->error_variable = stripSigil(catch_fields.stringOr("error", {"as", "error_variable", "var"}));
                    if (const json *inner = catch_fields.find({"body", "steps"}); inner != nullptr) {
                        node->catch_body = expander.expand(*inner, id, "catch");
                    }
                    if (auto target = catch_fields.optionalString({"target", "jump_to"}); target.has_value()) {
                        node->catch_body.push_back(*target);
                    }
                    node->catch_when = catch_fields.stringMap({"when", "cases"});
                    for (auto it = catch_block->begin(); it != catch_block->end(); ++it) {
                        if (!catch_used.count(it.key())) {
                            throw runtime::SapoError(runtime::ErrorCode::Parse,
                                                     "try node '" + id + "': unused catch field '" + it.key() + "'");
                        }
                    }
                } else {
                    node->catch_body = expander.expand(*catch_block, id, "catch");
                }
            }
            if (const json *final_block = fields.find({"finally", "cleanup"}); final_block != nullptr) {
                node->finally_body = expander.expand(*final_block, id, "finally");
            }
            if (node->body.empty()) reject("try node '" + id + "' needs a non-empty 'body'");
            if (node->catch_body.empty() && node->catch_when.empty() && node->finally_body.empty()) {
                reject("try node '" + id + "' needs at least one of 'catch' or 'finally'");
            }
            return node;
        }

        NodePtr parseNode(const json &node_json, const std::string &auto_id, const ParseOptions &options,
                          std::vector<std::string> &warnings, BodyExpander &expander) {
            if (!node_json.is_object()) {
                reject("every node must be a JSON object (got " + std::string(node_json.type_name()) + ")");
            }
            if (!node_json.contains("type")) reject("task node missing 'type' identifier");
            if (!node_json.at("type").is_string()) reject("task node 'type' must be a string");
            const std::string type_str = lower(node_json.at("type").get<std::string>());

            std::set<std::string> used;
            Fields fields(node_json, used);

            // Resolved first: generated child ids (`loop.body_0`) are derived from it.
            std::string node_id = fields.stringOr(auto_id, {"id", "node_id"});
            if (node_id.empty()) node_id = auto_id;

            NodePtr node;
            if (type_str == "noop") node = buildNoop(fields);
            else if (type_str == "terminate") node = buildTerminate(fields);
            else if (type_str == "script") node = buildScript(fields, node_id);
            else if (type_str == "transform") node = buildTransform(fields, node_id);
            else if (type_str == "query") node = buildQuery(fields, node_id);
            else if (type_str == "command") node = buildCommand(fields, node_id, options, warnings);
            else if (type_str == "event") node = buildEvent(fields, node_id);
            else if (type_str == "wait") node = buildWait(fields, node_id);
            else if (type_str == "schedule") node = buildSchedule(fields, node_id);
            else if (type_str == "subflow") node = buildSubflow(fields, node_id);
            else if (type_str == "condition" || type_str == "if") {
                node = buildCondition(fields, node_json, node_id, expander);
            } else if (type_str == "choice" || type_str == "switch") node = buildChoice(fields, node_id);
            else if (type_str == "parallel") node = buildParallel(fields, node_id, options, warnings, expander);
            else if (type_str == "action") node = buildAction(fields, node_id, options, warnings);
            else if (type_str == "loop") node = buildLoop(fields, node_id, expander);
            else if (type_str == "break" || type_str == "continue" || type_str == "loop_control") {
                node = buildLoopControl(fields, type_str, node_id);
            } else if (type_str == "try") node = buildTry(fields, node_id, expander);
            else {
                reject("Unknown task type: '" + type_str + "'. Grammar v1 accepts: noop, transform, query, command, "
                       "event, wait, schedule, subflow, script, condition, if, choice, parallel, action, loop, break, "
                       "continue, loop_control, try, terminate");
            }

            node->id = node_id;
            if (auto next = fields.optionalString({"next", "next_node", "next_step"}); next.has_value() && !next->empty()) {
                node->next = *next;
            }
            if (auto on_error = fields.optionalString({"on_error", "onError"}); on_error.has_value() && !on_error->empty()) {
                node->on_error = *on_error;
            }
            node->label = fields.optionalString({"label", "name", "title"});
            node->enabled = fields.boolOr(true, {"enabled", "active"});
            if (node_json.contains("retry")) {
                used.emplace("retry");
                node->retry = parseRetry(node_json.at("retry"));
            }

            for (auto it = node_json.begin(); it != node_json.end(); ++it) {
                const std::string &key = it.key();
                if (key == "type" || key == "id" || used.count(key)) continue;
                std::string message = "node '" + node->id + "' (" + type_str + ") has an unread field '" + key + "'";
                if (options.strict_fields) reject(message + " — unknown field (strict_fields is on)");
                warnings.push_back(std::move(message));
            }
            return node;
        }

    } // namespace

    NodePtr WorkflowParser::parseSingleNode(const json &node_json) {
        std::vector<NodePtr> hoisted;
        std::vector<std::string> warnings;
        ParseOptions options;
        options.validate = false;
        BodyExpander expander(hoisted, warnings, options);
        return parseNode(node_json, "node_0", options, warnings, expander);
    }

    ParsedWorkflow WorkflowParser::parseWorkflowJson(const json &document, const ParseOptions &options) {
        ParsedWorkflow workflow;

        json nodes_json;
        if (document.is_array()) {
            nodes_json = document;
        } else if (document.is_object()) {
            workflow.metadata.name = docString(document, "name", "sapo.workflow");
            workflow.metadata.version = docString(document, "version", "1.0");
            workflow.metadata.description = docString(document, "description");
            if (document.contains("nodes")) nodes_json = document["nodes"];
            else if (document.contains("tasks")) nodes_json = document["tasks"];
            else if (document.contains("steps")) nodes_json = document["steps"];
            else reject("a workflow object must contain a 'nodes' array");

            if (document.contains("trigger") && document["trigger"].is_object()) {
                const auto &trigger = document["trigger"];
                if (trigger.contains("event") && trigger["event"].is_string()) {
                    workflow.metadata.trigger_event = trigger["event"].get<std::string>();
                } else if (trigger.contains("name") && trigger["name"].is_string()) {
                    workflow.metadata.trigger_event = trigger["name"].get<std::string>();
                }
                if (trigger.contains("input")) workflow.metadata.trigger_input = trigger["input"];
            }
            if (document.contains("vars")) workflow.metadata.defaults = document["vars"];
            else if (document.contains("defaults")) workflow.metadata.defaults = document["defaults"];
            workflow.metadata.raw = document;
        } else {
            reject("a Sapo blueprint must be a JSON array of nodes or a workflow object");
        }

        if (!nodes_json.is_array()) reject("'nodes' must be an array");
        if (nodes_json.empty()) reject("blueprint contains no nodes");
        if (nodes_json.size() > options.max_nodes) {
            reject("blueprint has " + std::to_string(nodes_json.size()) + " nodes; the limit is " +
                   std::to_string(options.max_nodes));
        }

        std::vector<NodePtr> hoisted;
        BodyExpander expander(hoisted, workflow.warnings, options);

        size_t index = 0;
        for (const auto &entry : nodes_json) {
            workflow.nodes.push_back(
                parseNode(entry, "node_" + std::to_string(index++), options, workflow.warnings, expander));
        }
        // Hoisted inline bodies come last: the interpreter reaches them through
        // their parent's body list, never by linear stepping.
        for (auto &node : hoisted) workflow.nodes.push_back(std::move(node));

        for (const auto &node : workflow.nodes) {
            if (node->id.empty()) node->id = "node_" + std::to_string(workflow.index.size());
            if (workflow.index.contains(node->id)) {
                reject("duplicate node id '" + node->id + "' — every node needs a unique identifier");
            }
            workflow.index[node->id] = node;
        }
        if (workflow.nodes.empty()) reject("blueprint produced no nodes");

        // Sequential fall-through is part of the grammar: a node without an explicit
        // `next` continues at the next *top-level* node. Materialising that edge here
        // keeps the VM a pure jump machine (no array-index cursor to reconstruct) and
        // makes the graph safe to serialise, checkpoint and analyse statically.
        const size_t top_level_count = nodes_json.size();
        workflow.entry_id = workflow.nodes.front()->id;
        for (size_t i = 0; i + 1 < top_level_count; ++i) {
            auto &node = workflow.nodes[i];
            if (!node->next.has_value() && node->getType() != TaskType::Terminate) {
                node->next = workflow.nodes[i + 1]->id;
            }
        }

        if (!workflow.nodes.front()->next.has_value() && workflow.nodes.size() == 1 &&
            workflow.nodes.front()->getType() != TaskType::Terminate) {
            workflow.warnings.push_back("workflow '" + workflow.metadata.name +
                                        "' has a single non-terminal node; it will end after it runs");
        }

        if (options.validate) {
            BlueprintValidator::validate(workflow, options, workflow.warnings);
        }
        return workflow;
    }

    ParsedWorkflow WorkflowParser::parseWorkflow(const std::string &json_content, const ParseOptions &options) {
        json document;
        try {
            document = json::parse(json_content);
        } catch (const std::exception &e) {
            reject(std::string("blueprint is not valid JSON: ") + e.what());
        }
        return parseWorkflowJson(document, options);
    }

    WorkflowAST WorkflowParser::parse(const std::string &json_content) {
        ParseOptions options;
        options.validate = false;
        return parseWorkflow(json_content, options).nodes;
    }

    const char *toString(TaskType type) {
        switch (type) {
            case TaskType::Noop: return "noop";
            case TaskType::Transform: return "transform";
            case TaskType::Query: return "query";
            case TaskType::Command: return "command";
            case TaskType::Event: return "event";
            case TaskType::Loop: return "loop";
            case TaskType::LoopControl: return "loop_control";
            case TaskType::Choice: return "choice";
            case TaskType::Condition: return "condition";
            case TaskType::Wait: return "wait";
            case TaskType::Schedule: return "schedule";
            case TaskType::Subflow: return "subflow";
            case TaskType::Script: return "script";
            case TaskType::Parallel: return "parallel";
            case TaskType::Terminate: return "terminate";
            case TaskType::Action: return "action";
            case TaskType::Try: return "try";
        }
        return "unknown";
    }

} // namespace sapo::parser
