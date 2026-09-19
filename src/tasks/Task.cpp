//
//  Sapo Engine — task base + shared helpers.
//
#include "tasks/Task.hpp"
#include "tasks/Tasks.hpp"
#include "util/JsonPath.hpp"

#include <algorithm>

using json = nlohmann::json;

namespace sapo::tasks {

    runtime::EvaluationScope ExecutionContext::scope() const {
        runtime::EvaluationScope scope(context);
        scope.bindings = services.bindings.get();
        scope.locals = locals.is_object() ? locals : json::object();
        scope.node_id = node->id;
        scope.options.strict = true;
        return scope;
    }

    json ExecutionContext::eval(const parser::Expression &expression) const {
        return runtime::ExpressionEvaluator::evaluate(expression, scope());
    }

    json ExecutionContext::evalOr(const parser::Expression &expression, const json &fallback) const {
        if (expression.empty()) return fallback;
        return runtime::ExpressionEvaluator::evaluate(expression, scope());
    }

    bool ExecutionContext::evalBool(const parser::Expression &expression) const {
        return runtime::ExpressionEvaluator::evaluateBool(expression, scope());
    }

    bool ExecutionContext::evalBoolOr(const std::optional<parser::Expression> &expression, bool fallback) const {
        if (!expression.has_value() || expression->empty()) return fallback;
        return runtime::ExpressionEvaluator::evaluateBool(*expression, scope());
    }

    json ExecutionContext::resolve(const json &template_value) const {
        return resolveObjectTemplate(template_value, *this, true);
    }

    json ExecutionContext::resolveLenient(const json &template_value) const {
        return resolveObjectTemplate(template_value, *this, false);
    }

    void ExecutionContext::write(const std::string &key, const json &value) const {
        if (key.empty()) return;
        context.setByPath(key, value);
    }

    json ExecutionContext::read(const std::string &dotted_path) const {
        const auto value = context.getByPath(dotted_path);
        return value.has_value() ? *value : json();
    }

    bool ExecutionContext::has(const std::string &dotted_path) const {
        return context.hasVariable(dotted_path);
    }

    void ExecutionContext::log_debug(const std::string &message, const json &fields) const {
        if (services.logger) services.logger->debug(node->id, message, fields);
    }
    void ExecutionContext::log_info(const std::string &message, const json &fields) const {
        if (services.logger) services.logger->info(node->id, message, fields);
    }
    void ExecutionContext::log_warn(const std::string &message, const json &fields) const {
        if (services.logger) services.logger->warn(node->id, message, fields);
    }

    // ---------------------------------------------------------------------
    json resolveObjectTemplate(const json &object, const ExecutionContext &execution, bool strict) {
        if (object.is_string()) {
            auto scope = execution.scope();
            scope.options.strict = strict;
            return runtime::ExpressionEvaluator::resolveValue(object.get<std::string>(), scope);
        }
        if (object.is_array()) {
            json out = json::array();
            for (const auto &item : object) out.push_back(resolveObjectTemplate(item, execution, strict));
            return out;
        }
        if (object.is_object()) {
            // An object that *is* a single expression ({"expression": "…"}) is
            // evaluated, which is how structured `{left, operator, right}`
            // conditions arrive from the DSL.
            if (object.size() == 1 && object.contains("expression") && object["expression"].is_string()) {
                auto scope = execution.scope();
                scope.options.strict = strict;
                return runtime::ExpressionEvaluator::evaluate(parser::Expression(object["expression"].get<std::string>()),
                                                               scope);
            }
            json out = json::object();
            for (auto it = object.begin(); it != object.end(); ++it) {
                out[it.key()] = resolveObjectTemplate(it.value(), execution, strict);
            }
            return out;
        }
        return object;
    }

    json extractField(const json &source, const std::string &reference, const ExecutionContext &execution) {
        if (reference.empty()) return source;
        std::string path = reference;
        if (path.front() == '$') path.erase(0, 1);
        while (!path.empty() && path.front() == '.') path.erase(0, 1);

        if (!path.empty() && path.find_first_of(" +-*/<>[]()") == std::string::npos) {
            if (source.is_object()) {
                if (auto value = util::getPath(source, path); value.has_value()) return *value;
            }
            if (source.is_array()) {
                try {
                    const size_t index = std::stoul(path);
                    if (index < source.size()) return source[index];
                } catch (...) { // fall through to expression evaluation
                }
            }
            if (source.is_null()) {
                // Reference against the session context instead (e.g. "$order.id").
                if (auto value = execution.context.getByPath(reference); value.has_value()) return *value;
            }
        }

        // Expression form: the response is exposed as `response`/`$response` plus
        // its top-level keys, so `"body.total"` and `"status == 200"` both work.
        auto scope = execution.scope();
        if (source.is_object()) {
            for (auto it = source.begin(); it != source.end(); ++it) scope.locals[it.key()] = it.value();
        }
        scope.locals["response"] = source;
        scope.locals["body"] = source.is_object() ? source.value("body", json()) : json();
        scope.options.strict = false;
        return runtime::ExpressionEvaluator::evaluate(parser::Expression(reference), scope);
    }

    // ---------------------------------------------------------------------
    void TaskRegistry::add(parser::TaskType type, TaskPtr task) {
        if (task == nullptr) return;
        for (auto &entry : m_tasks) {
            if (entry.first == type) {
                entry.second = std::move(task);
                return;
            }
        }
        m_tasks.emplace_back(type, std::move(task));
    }

    const ITask *TaskRegistry::find(parser::TaskType type) const {
        for (const auto &[entry_type, task] : m_tasks) {
            if (entry_type == type) return task.get();
        }
        return nullptr;
    }

    bool TaskRegistry::handles(parser::TaskType type) const { return find(type) != nullptr; }

    std::vector<std::string> TaskRegistry::handledTypes() const {
        std::vector<std::string> out;
        for (const auto &[type, task] : m_tasks) out.push_back(parser::toString(type));
        std::sort(out.begin(), out.end());
        return out;
    }

    TaskRegistry &TaskRegistry::defaults() {
        static TaskRegistry registry = [] {
            TaskRegistry built_in;
            // NB: argument evaluation order is unspecified — `handles()` must be
            // read before `task` is moved out of.
            auto add = [&built_in](TaskPtr task) {
                const parser::TaskType type = task->handles();
                built_in.add(type, std::move(task));
            };
            add(std::make_shared<NoopTask>());
            add(std::make_shared<TransformTask>());
            add(std::make_shared<ScriptTask>());
            add(std::make_shared<EventTask>());
            add(std::make_shared<TerminateTask>());
            add(std::make_shared<LoopControlTask>());
            add(std::make_shared<WaitTask>());
            add(std::make_shared<ScheduleTask>());
            add(std::make_shared<QueryTask>());
            add(std::make_shared<CommandTask>());
            add(std::make_shared<ActionTask>());
            add(std::make_shared<SubflowTask>());
            add(std::make_shared<ConditionTask>());
            add(std::make_shared<ChoiceTask>());
            add(std::make_shared<LoopTask>());
            add(std::make_shared<TryTask>());
            add(std::make_shared<ParallelTask>());
            return built_in;
        }();
        return registry;
    }

} // namespace sapo::tasks
