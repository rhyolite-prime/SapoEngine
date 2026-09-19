//
//  Sapo Engine — leaf tasks (noop, transform, script, event, terminate,
//  loop control, wait, schedule, query).
//
#include "tasks/Tasks.hpp"
#include "util/TimeUtils.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <thread>

using json = nlohmann::json;

namespace sapo::tasks {

    namespace {

        /// Locals for one row of a collection operation.
        runtime::EvaluationScope rowScope(const ExecutionContext &execution, const json &row, int64_t index,
                                          const json &extra = json::object()) {
            auto scope = execution.scope();
            if (extra.is_object()) {
                for (auto it = extra.begin(); it != extra.end(); ++it) scope.locals[it.key()] = it.value();
            }
            if (row.is_object()) {
                for (auto it = row.begin(); it != row.end(); ++it) scope.locals[it.key()] = it.value();
            }
            scope.locals["item"] = row;
            scope.locals["$item"] = row;
            scope.locals["index"] = index;
            scope.locals["$index"] = index;
            return scope;
        }

        bool predicateTrue(const parser::Expression &predicate, const runtime::EvaluationScope &scope) {
            if (predicate.empty()) return true;
            return runtime::ExpressionEvaluator::evaluateBool(predicate, scope);
        }

        std::vector<json> asRows(const json &value, const std::string &node_id) {
            std::vector<json> rows;
            if (value.is_array()) {
                for (const auto &item : value) rows.push_back(item);
                return rows;
            }
            if (value.is_object()) {
                // A keyed object is treated as its values when it looks like a
                // collection of records (`{"a": {...}, "b": {...}}`).
                bool all_objects = !value.empty();
                for (auto it = value.begin(); it != value.end(); ++it) {
                    if (!it.value().is_object()) {
                        all_objects = false;
                        break;
                    }
                }
                if (all_objects) {
                    for (auto it = value.begin(); it != value.end(); ++it) {
                        json row = it.value();
                        row["key"] = it.key();
                        rows.push_back(std::move(row));
                    }
                    return rows;
                }
                rows.push_back(value);
                return rows;
            }
            if (value.is_null()) {
                throw runtime::SapoError(runtime::ErrorCode::Validation,
                                         "transform input resolved to null; there is nothing to iterate",
                                         json::object(), node_id);
            }
            rows.push_back(value);
            return rows;
        }

    } // namespace

    // ---------------------------------------------------------------------
    // noop
    // ---------------------------------------------------------------------
    ControlSignal NoopTask::execute(ExecutionContext &execution) const {
        const auto &node = execution.as<parser::NoopNode>();
        if (node.assign.is_object()) {
            for (auto it = node.assign.begin(); it != node.assign.end(); ++it) {
                execution.write(it.key(), execution.resolve(it.value()));
            }
        } else if (!node.assign.is_null()) {
            throw runtime::SapoError(runtime::ErrorCode::Validation, "a noop's 'assign' must be an object",
                                     json::object(), node.id);
        }
        return Continue{};
    }

    // ---------------------------------------------------------------------
    // transform (T2.7: real operations, no placeholder pass-through)
    // ---------------------------------------------------------------------
    ControlSignal TransformTask::execute(ExecutionContext &execution) const {
        const auto &node = execution.as<parser::TransformNode>();
        const json input = node.input.empty() ? json() : execution.eval(node.input);
        const json result = apply(node, input, execution);
        if (!node.output.empty()) execution.write(node.output, result);
        return Continue{};
    }

    json TransformTask::apply(const parser::TransformNode &node, const json &input, ExecutionContext &execution) {
        const std::string &operation = node.operation;

        if (operation == "assign" || operation == "set" || operation == "copy") {
            json value = input;
            if (value.is_null() && node.mapping.is_object() && !node.mapping.empty()) {
                value = resolveObjectTemplate(node.mapping, execution);
            }
            if (operation == "set") {
                json merged = execution.read(node.output);
                if (!merged.is_object()) merged = json::object();
                if (value.is_object()) {
                    for (auto it = value.begin(); it != value.end(); ++it) merged[it.key()] = it.value();
                } else if (value.is_null()) {
                    merged = resolveObjectTemplate(node.mapping, execution);
                } else {
                    throw runtime::SapoError(runtime::ErrorCode::Validation,
                                             "transform 'set' merges objects; the resolved input is " +
                                                 std::string(value.type_name()),
                                             json::object(), node.id);
                }
                return merged;
            }
            return value;
        }

        std::vector<json> rows = asRows(input, node.id);

        if (operation == "filter") {
            json out = json::array();
            int64_t index = 0;
            for (const auto &row : rows) {
                if (predicateTrue(node.predicate, rowScope(execution, row, index))) out.push_back(row);
                ++index;
            }
            return out;
        }

        if (operation == "map" || operation == "project") {
            json out = json::array();
            int64_t index = 0;
            for (const auto &row : rows) {
                const auto scope = rowScope(execution, row, index);
                if (node.mapping.is_object() && !node.mapping.empty()) {
                    out.push_back(resolveObjectTemplate(node.mapping, execution, true));
                    // Projection entries are expressions over the row:
                    json projected = json::object();
                    for (auto it = node.mapping.begin(); it != node.mapping.end(); ++it) {
                        projected[it.key()] = runtime::ExpressionEvaluator::resolveValue(
                            it.value().is_string() ? it.value().get<std::string>() : it.value().dump(), scope);
                    }
                    out.back() = std::move(projected);
                } else if (!node.predicate.empty()) {
                    out.push_back(runtime::ExpressionEvaluator::evaluate(node.predicate, scope));
                } else {
                    out.push_back(row);
                }
                ++index;
            }
            if (operation == "project" && !node.output_as_array && out.size() == 1) return out.front();
            return out;
        }

        if (operation == "merge") {
            if (node.output_as_array) {
                json out = json::array();
                for (const auto &row : rows) {
                    if (row.is_array()) out.insert(out.end(), row.begin(), row.end());
                    else out.push_back(row);
                }
                return out;
            }
            json out = json::object();
            for (const auto &row : rows) {
                if (!row.is_object()) {
                    throw runtime::SapoError(runtime::ErrorCode::Validation,
                                             "transform 'merge' needs object rows; got " + std::string(row.type_name()),
                                             json::object(), node.id);
                }
                for (auto it = row.begin(); it != row.end(); ++it) out[it.key()] = it.value();
            }
            return out;
        }

        if (operation == "group") {
            std::map<std::string, json> groups;
            if (!node.mapping.is_object() || node.mapping.empty()) {
                throw runtime::SapoError(runtime::ErrorCode::Validation,
                                        "transform 'group' needs a mapping with the grouping key expression",
                                        json::object(), node.id);
            }
            const auto key_expression = node.mapping.begin().value();
            int64_t index = 0;
            for (const auto &row : rows) {
                const auto scope = rowScope(execution, row, index);
                const json key = runtime::ExpressionEvaluator::resolveValue(
                    key_expression.is_string() ? key_expression.get<std::string>() : key_expression.dump(), scope);
                std::string name = key.is_string() ? key.get<std::string>() : key.dump();
                if (!groups[name].is_array()) groups[name] = json::array();
                groups[name].push_back(row);
                ++index;
            }
            json out = json::object();
            for (auto &[name, items] : groups) out[name] = std::move(items);
            return out;
        }

        if (operation == "sort") {
            std::string key_field;
            bool descending = false;
            if (node.mapping.is_object()) {
                if (auto it = node.mapping.find("by"); it != node.mapping.end() && it->is_string()) key_field = it->get<std::string>();
                if (auto it = node.mapping.find("order"); it != node.mapping.end() && it->is_string()) {
                    descending = it->get<std::string>() == "desc" || it->get<std::string>() == "descending";
                }
            }
            if (key_field.empty() && node.predicate.empty() && !node.mapping.is_null() && node.mapping.is_string()) {
                key_field = node.mapping.get<std::string>();
            }
            std::vector<std::pair<json, json>> keyed;
            int64_t index = 0;
            for (const auto &row : rows) {
                json key;
                if (!key_field.empty()) {
                    key = util::getPath(row, key_field).value_or(json());
                } else if (!node.predicate.empty()) {
                    key = runtime::ExpressionEvaluator::evaluate(node.predicate, rowScope(execution, row, index));
                } else {
                    key = row;
                }
                keyed.emplace_back(std::move(key), row);
                ++index;
            }
            const auto compare = [](const json &a, const json &b) {
                if (a.is_number() && b.is_number()) return a.get<double>() < b.get<double>();
                if (a.is_string() && b.is_string()) return a.get<std::string>() < b.get<std::string>();
                return a.dump() < b.dump();
            };
            std::stable_sort(keyed.begin(), keyed.end(), [&](const auto &left, const auto &right) {
                if (compare(left.first, right.first)) return !descending;
                if (compare(right.first, left.first)) return descending;
                return false;
            });
            json out = json::array();
            for (auto &[key, row] : keyed) out.push_back(row);
            return out;
        }

        if (operation == "flatten") {
            json out = json::array();
            std::function<void(const json &)> walk = [&](const json &value) {
                if (value.is_array()) {
                    for (const auto &item : value) walk(item);
                } else {
                    out.push_back(value);
                }
            };
            for (const auto &row : rows) walk(row);
            return out;
        }

        if (operation == "reduce") {
            json accumulator = json(0);
            if (node.mapping.is_object()) {
                if (auto it = node.mapping.find("initial"); it != node.mapping.end()) accumulator = *it;
            }
            int64_t index = 0;
            for (const auto &row : rows) {
                auto scope = rowScope(execution, row, index, json{{"accumulator", accumulator}, {"acc", accumulator}});
                if (node.predicate.empty()) {
                    if (row.is_number() && accumulator.is_number()) accumulator = accumulator.get<double>() + row.get<double>();
                } else {
                    accumulator = runtime::ExpressionEvaluator::evaluate(node.predicate, scope);
                }
                ++index;
            }
            return accumulator;
        }

        throw runtime::SapoError(runtime::ErrorCode::Validation,
                                 "transform operation '" + operation + "' is not implemented", json::object(), node.id);
    }

    // ---------------------------------------------------------------------
    // script
    // ---------------------------------------------------------------------
    ControlSignal ScriptTask::execute(ExecutionContext &execution) const {
        const auto &node = execution.as<parser::ScriptNode>();
        auto scope = execution.scope();
        for (const auto &[name, expression] : node.bindings) {
            scope.locals[name] = runtime::ExpressionEvaluator::evaluate(parser::Expression(expression), scope);
        }
        // Both languages evaluate through SEL. The exprtk fast path was dropped on
        // purpose: it coerces every value to double, so strings, arrays and
        // booleans silently became 0/1 — a correctness problem worth more than the
        // micro-optimisation (docs/LIMITATIONS.md).
        const json result = runtime::ExpressionEvaluator::evaluate(node.code, scope);
        if (node.output.has_value() && !node.output->empty()) execution.write(*node.output, result);
        // Scripts that resolve to `false` while a guard was intended are reported,
        // never treated as a silent success.
        if (result.is_object() && result.contains("__sapo_jump_to") && result["__sapo_jump_to"].is_string()) {
            return JumpTo{result["__sapo_jump_to"].get<std::string>()};
        }
        return Continue{};
    }

    // ---------------------------------------------------------------------
    // event
    // ---------------------------------------------------------------------
    ControlSignal EventTask::execute(ExecutionContext &execution) const {
        const auto &node = execution.as<parser::EventEmitNode>();
        runtime::Event event;
        event.name = node.name;
        event.payload = execution.resolve(node.payload);
        event.session_id = execution.session_id;
        event.execution_id = execution.execution_id;
        event.source_node = node.id;
        event.timestamp_ms = execution.services.clock ? execution.services.clock->now().count() : 0LL;
        if (node.publish_now) {
            execution.services.events->publish(std::move(event));
        }
        return Continue{};
    }

    // ---------------------------------------------------------------------
    // terminate
    // ---------------------------------------------------------------------
    ControlSignal TerminateTask::execute(ExecutionContext &execution) const {
        const auto &node = execution.as<parser::TerminateNode>();
        Terminate terminate;
        terminate.status = node.status;
        if (node.error_code.has_value()) terminate.error_code = *node.error_code;
        if (node.message.has_value()) {
            const json resolved = runtime::ExpressionEvaluator::resolve(*node.message, execution.scope());
            terminate.message = resolved.is_string() ? resolved.get<std::string>() : resolved.dump();
        }
        terminate.payload = node.output.is_object() ? execution.resolve(node.output) : node.output;
        return terminate;
    }

    // ---------------------------------------------------------------------
    // break / continue
    // ---------------------------------------------------------------------
    ControlSignal LoopControlTask::execute(ExecutionContext &execution) const {
        const auto &node = execution.as<parser::LoopControlNode>();
        if (!execution.evalBoolOr(node.when, true)) return Continue{};
        if (node.action == "break") return LoopBreak{node.loop};
        return LoopContinue{node.loop};
    }

    // ---------------------------------------------------------------------
    // wait
    // ---------------------------------------------------------------------
    std::optional<int64_t> WaitTask::deadlineFor(const parser::WaitNode &node, const runtime::TaskServices &services,
                                                  int64_t now_ms) {
        (void) services;
        if (node.duration.has_value()) {
            const std::string &text = *node.duration;
            if (auto delay = util::parseDuration(text); delay.has_value()) return now_ms + delay->count();
            if (auto delay = util::parseRelativeDelay(text); delay.has_value()) return now_ms + delay->count();
            if (auto absolute = util::parseIso8601(text); absolute.has_value()) return *absolute;
            throw runtime::SapoError(runtime::ErrorCode::Validation, "unparseable wait duration '" + text + "'",
                                     json::object(), node.id);
        }
        // Condition-driven waits have no deadline here: the caller evaluates `until`
        // against the session scope (an absolute timestamp in `until` is honoured by
        // the interpreter, not by this helper).
        return std::nullopt;
    }

    ControlSignal WaitTask::execute(ExecutionContext &execution) const {
        const auto &node = execution.as<parser::WaitNode>();
        const int64_t now = execution.services.clock ? execution.services.clock->now().count() : 0LL;

        // A condition-driven wait re-runs itself on each poll tick.
        if (!node.duration.has_value() && !node.until.empty()) {
            const bool satisfied = execution.evalBool(node.until);
            if (satisfied) return Continue{};
            SuspendRequest suspend;
            suspend.session_id = execution.session_id;
            suspend.node_id = node.id;
            suspend.reason = "timer";
            suspend.resume_node = node.id;
            suspend.resume_at_ms = now + static_cast<int64_t>(node.poll_interval_ms.value_or(1000));
            suspend.data = json{{"await_condition", node.until.source()}};
            return suspend;
        }

        const auto deadline = deadlineFor(node, execution.services, now);
        SuspendRequest suspend;
        suspend.session_id = execution.session_id;
        suspend.node_id = node.id;
        suspend.reason = "timer";
        suspend.resume_at_ms = deadline.value_or(now);
        suspend.data = json{{"duration", node.duration.value_or("")}};
        if (!node.durable) {
            // `durable: false` means "short enough to inline". The sleep goes through
            // the injected delay sink so a host can drive it in virtual time (tests)
            // instead of parking a pool thread.
            const int64_t wait_ms = suspend.resume_at_ms.value_or(now) - now;
            if (wait_ms > 0 && wait_ms <= execution.services.limits.inline_wait_limit_ms) {
                if (execution.services.delay_sink) execution.services.delay_sink(wait_ms);
                else std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
                return Continue{};
            }
        }
        return suspend;
    }

    // ---------------------------------------------------------------------
    // schedule
    // ---------------------------------------------------------------------
    ControlSignal ScheduleTask::execute(ExecutionContext &execution) const {
        const auto &node = execution.as<parser::ScheduleNode>();
        if (execution.services.scheduler == nullptr) {
            throw runtime::SapoError(runtime::ErrorCode::Internal, "no scheduler is installed", json::object(), node.id);
        }
        runtime::ScheduledJob job;
        job.id = node.job_id.value_or("schedule." + node.id);
        job.schedule = node.cron;
        job.timezone = node.timezone.value_or("UTC");
        job.workflow = node.workflow.value_or(execution.workflow_id);
        job.node_id = node.id;
        // `body` names the node to start at inside the target workflow; the
        // scheduler-side handler ignores it when that workflow has no such node.
        if (!node.body.empty()) job.entry_node = node.body;
        job.input = execution.resolve(node.input);
        job.enabled = node.enabled;
        execution.services.scheduler->addJob(std::move(job));
        return Continue{};
    }

    // ---------------------------------------------------------------------
    // query
    // ---------------------------------------------------------------------
    ControlSignal QueryTask::execute(ExecutionContext &execution) const {
        const auto &node = execution.as<parser::QueryNode>();

        data::DataSourceConfig config;
        const auto binding = execution.data_sources.is_object() ? execution.data_sources.find(node.source) : decltype(execution.data_sources.end()){};
        if (binding != execution.data_sources.end() && binding->is_object()) {
            config.name = binding->value("name", node.source);
            config.provider = binding->value("provider", "");
            config.scope = binding->value("scope", "internal");
            if (binding->contains("config")) config.config = (*binding)["config"];
        } else {
            config.name = node.source;
            // No declaration: treat the source id as a context collection, which is
            // what the generated USSD blueprints expect.
            config.provider = "context";
            config.config = json{{"source", "$" + node.source}};
        }
        config.origin = execution.workflow_id.empty() ? std::string("blueprint")
                                                        : "workflow '" + execution.workflow_id + "'";

        data::DataQuery query;
        query.filter = node.filter.is_string() ? json() : node.filter;
        query.limit = node.limit;
        query.offset = node.offset;
        query.statement = node.statement;
        query.parameters = execution.resolve(node.parameters);

        data::RowMatcher matcher = nullptr;
        if (node.filter.is_string()) {
            const parser::Expression predicate = node.filter.get<std::string>();
            const auto session = &execution;
            matcher = [predicate, session](const json &row) {
                auto scope = session->scope();
                if (row.is_object()) {
                    for (auto it = row.begin(); it != row.end(); ++it) scope.locals[it.key()] = it.value();
                }
                scope.locals["item"] = row;
                return runtime::ExpressionEvaluator::evaluateBool(predicate, scope);
            };
        } else if (!node.filter.is_null()) {
            matcher = data::makeRowMatcher(node.filter);
        }

        const data::DataPage page = execution.services.data_sources->run(config, query, execution.context, matcher);
        json result = page.rows;
        if (result.is_array() && page.total_matches != result.size()) {
            // Keep the plain array as the value, and expose the count separately.
            execution.write(node.output + "_total", page.total_matches);
        }
        if (page.truncated) execution.write(node.output + "_truncated", true);
        execution.write(node.output, std::move(result));
        return Continue{};
    }

} // namespace sapo::tasks
