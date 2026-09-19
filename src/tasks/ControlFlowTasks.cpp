//
//  Sapo Engine — control-flow helpers (condition, choice, loop, parallel).
//
//  These nodes own *structure*, so the interpreter drives their frames; what the
//  task classes provide is the evaluation each construct needs. Keeping the
//  decision functions here (instead of inline in the VM) means the branch logic is
//  unit-testable without running a whole workflow.
//
#include "tasks/Tasks.hpp"

#include <cstdlib>

using json = nlohmann::json;

namespace sapo::tasks {

    namespace {

        /// Canonical key for a `choice` case lookup: numbers lose the trailing
        /// `.0`, booleans become "true"/"false", strings are used verbatim.
        std::string selectorKey(const json &value) {
            if (value.is_string()) return value.get<std::string>();
            if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
            if (value.is_number_integer()) return std::to_string(value.get<int64_t>());
            if (value.is_number()) {
                const double number = value.get<double>();
                if (number == static_cast<int64_t>(number)) return std::to_string(static_cast<int64_t>(number));
                return value.dump();
            }
            if (value.is_null()) return "null";
            return value.dump();
        }

    } // namespace

    // ---------------------------------------------------------------------
    // condition / if
    // ---------------------------------------------------------------------
    ConditionBranch ConditionTask::branch(const parser::ConditionNode &node, ExecutionContext &execution) {
        ConditionBranch branch;
        branch.taken = execution.evalBool(node.expression);
        if (branch.taken) {
            branch.body = node.then_body;
            branch.target = node.on_true;
        } else {
            branch.body = node.else_body;
            branch.target = node.on_false.value_or("");
        }
        return branch;
    }

    bool ConditionTask::take(const parser::ConditionNode &node, ExecutionContext &execution) {
        return execution.evalBool(node.expression);
    }

    ControlSignal ConditionTask::execute(ExecutionContext &execution) const {
        const auto &node = execution.as<parser::ConditionNode>();
        const ConditionBranch branch_result = branch(node, execution);
        if (!branch_result.body.empty()) return Continue{};   // the VM runs the body
        if (!branch_result.target.empty()) return JumpTo{branch_result.target};
        return Continue{};
    }

    // ---------------------------------------------------------------------
    // choice
    // ---------------------------------------------------------------------
    std::string ChoiceTask::select(const parser::ChoiceNode &node, ExecutionContext &execution) {
        const json selector = execution.eval(node.expression);
        const std::string key = selectorKey(selector);
        auto match = node.cases.find(key);
        if (match == node.cases.end()) {
            // Tolerate `"1"` vs `1` mismatches from generated blueprints.
            for (const auto &[candidate, target] : node.cases) {
                if (selectorKey(json(candidate)) == key) {
                    match = node.cases.find(candidate);
                    break;
                }
            }
        }
        if (match != node.cases.end()) return match->second;
        return node.default_target.value_or("");
    }

    ControlSignal ChoiceTask::execute(ExecutionContext &execution) const {
        const auto &node = execution.as<parser::ChoiceNode>();
        const std::string target = select(node, execution);
        if (target.empty()) {
            // A selector that matches nothing is a blueprint bug: report it with the
            // offending value instead of silently ending the session.
            const json selector = execution.eval(node.expression);
            json allowed = json::array();
            for (const auto &[candidate, target_id] : node.cases) {
                (void) target_id;
                allowed.push_back(candidate);
            }
            std::string listed;
            for (const auto &candidate : allowed) {
                if (!listed.empty()) listed += ", ";
                listed += candidate.get<std::string>();
            }
            throw runtime::SapoError(
                runtime::ErrorCode::Routing,
                "choice node '" + node.id + "' has no case for value '" + selectorKey(selector) +
                    "' and no 'default' target (known cases: " + (listed.empty() ? std::string("none") : listed) + ")",
                json{{"value", selector}, {"cases", allowed}}, node.id);
        }
        return JumpTo{target};
    }

    // ---------------------------------------------------------------------
    // loop
    // ---------------------------------------------------------------------
    json LoopTask::items(const parser::LoopNode &node, ExecutionContext &execution) {
        if (!node.count.empty()) {
            json value = execution.eval(node.count);
            if (value.is_string()) {
                // `"count": "10"` and `"count": "${size}"` are both normal ways to
                // write the field, and a template resolves to text: accept it.
                const std::string text = value.get<std::string>();
                auto trimmed = [](std::string view) {
                    const auto first = view.find_first_not_of(" \t\n\r");
                    const auto last = view.find_last_not_of(" \t\n\r");
                    if (first == std::string::npos) return std::string();
                    return view.substr(first, last - first + 1);
                };
                const std::string body = trimmed(text);
                bool numeric = !body.empty();
                size_t start = (body.front() == '-' || body.front() == '+') ? 1 : 0;
                if (start >= body.size()) numeric = false;
                for (size_t index = start; index < body.size() && numeric; ++index) {
                    const char c = body[index];
                    numeric = (c >= '0' && c <= '9') || (index == start + 1 && c == '.');
                }
                if (numeric) value = json(std::strtod(body.c_str(), nullptr));
            }
            if (value.is_number() && value.is_number_float()) value = json(static_cast<int64_t>(value.get<double>()));
            if (!value.is_number()) {
                throw runtime::SapoError(runtime::ErrorCode::Validation,
                                         "loop 'count' must resolve to a number, got " + std::string(value.type_name()),
                                         json{{"count", value}}, node.id);
            }
            const int64_t total = static_cast<int64_t>(value.get<double>());
            json out = json::array();
            for (int64_t index = 0; index < total; ++index) out.push_back(index);
            return out;
        }
        if (!node.collection.empty()) {
            const json value = execution.eval(node.collection);
            if (value.is_array()) return value;
            if (value.is_null()) return json::array();   // "nothing to iterate" is a valid zero-trip loop
            if (value.is_object()) {
                json out = json::array();
                for (auto it = value.begin(); it != value.end(); ++it) out.push_back(it.value());
                return out;
            }
            return json::array({value});
        }
        return json();   // condition-driven loop: the VM iterates on the guard
    }

    bool LoopTask::guard(const parser::LoopNode &node, ExecutionContext &execution) {
        if (node.condition.empty()) return true;
        return execution.evalBool(node.condition);
    }

    ControlSignal LoopTask::execute(ExecutionContext &execution) const {
        // Standalone execution validates the iteration source; the interpreter drives
        // the actual passes through its frame stack.
        const auto &node = execution.as<parser::LoopNode>();
        const json collection = items(node, execution);
        if (!collection.is_array() && !collection.is_null()) {
            throw runtime::SapoError(runtime::ErrorCode::Validation, "loop source is not a collection",
                                     json{{"type", std::string(collection.type_name())}}, node.id);
        }
        if (node.body.empty()) {
            throw runtime::SapoError(runtime::ErrorCode::Validation, "loop node has an empty 'body'", json::object(),
                                     node.id);
        }
        return Continue{};
    }

    // ---------------------------------------------------------------------
    // parallel
    // ---------------------------------------------------------------------
    runtime::RuntimeContext::MergePolicy ParallelTask::mergePolicy(const std::string &name) {
        if (name == "skip_conflicts") return runtime::RuntimeContext::MergePolicy::SkipConflicts;
        if (name == "fail_conflicts") return runtime::RuntimeContext::MergePolicy::FailConflicts;
        return runtime::RuntimeContext::MergePolicy::LastWriterWins;
    }

    ControlSignal ParallelTask::execute(ExecutionContext &execution) const {
        const auto &node = execution.as<parser::ParallelNode>();
        throw runtime::SapoError(
            runtime::ErrorCode::Internal,
            "parallel node '" + node.id + "' must be executed by the interpreter (it drives the worker pool)",
            json{{"branches", node.branches.size()}}, node.id);
    }

} // namespace sapo::tasks
