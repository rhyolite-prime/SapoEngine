//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//
#include "parser/BlueprintValidator.hpp"
#include "parser/WorkflowParser.hpp"
#include "runtime/SapoError.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace sapo::parser {

    namespace {

        void addIssue(std::vector<ValidationIssue> &issues, ValidationIssue::Level level, std::string node_id,
                      std::string message) {
            issues.push_back(ValidationIssue{level, std::move(node_id), std::move(message)});
        }

    } // namespace

    std::vector<std::string> BlueprintValidator::outgoingTargets(const AstNode &node) {
        std::vector<std::string> targets;
        auto push = [&targets](const std::optional<std::string> &value) {
            if (value.has_value() && !value->empty()) targets.push_back(*value);
        };
        auto pushAll = [&targets](const std::vector<std::string> &values) {
            for (const auto &value : values) {
                if (!value.empty()) targets.push_back(value);
            }
        };

        switch (node.getType()) {
            case TaskType::Condition: {
                const auto &condition = static_cast<const ConditionNode &>(node);
                if (!condition.on_true.empty()) targets.push_back(condition.on_true);
                push(condition.on_false);
                pushAll(condition.then_body);
                pushAll(condition.else_body);
                break;
            }
            case TaskType::Choice: {
                const auto &choice = static_cast<const ChoiceNode &>(node);
                for (const auto &[value, target] : choice.cases) {
                    (void) value;
                    if (!target.empty()) targets.push_back(target);
                }
                push(choice.default_target);
                break;
            }
            case TaskType::Loop: {
                const auto &loop = static_cast<const LoopNode &>(node);
                pushAll(loop.body);
                break;
            }
            case TaskType::Try: {
                const auto &try_node = static_cast<const TryNode &>(node);
                pushAll(try_node.body);
                pushAll(try_node.catch_body);
                pushAll(try_node.finally_body);
                for (const auto &[code, target] : try_node.catch_when) {
                    (void) code;
                    if (!target.empty()) targets.push_back(target);
                }
                break;
            }
            case TaskType::Parallel: {
                const auto &parallel = static_cast<const ParallelNode &>(node);
                for (const auto &branch : parallel.branches) pushAll(branch.node_ids);
                break;
            }
            case TaskType::Schedule: {
                const auto &schedule = static_cast<const ScheduleNode &>(node);
                if (!schedule.body.empty()) targets.push_back(schedule.body);
                break;
            }
            case TaskType::Action: {
                const auto &action = static_cast<const ActionNode &>(node);
                for (const auto &reference : action.next_tasks) {
                    if (!reference.task_id.empty()) targets.push_back(reference.task_id);
                }
                if (action.on_event.has_value()) push(action.on_event->handler);
                break;
            }
            case TaskType::Noop:
            case TaskType::Transform:
            case TaskType::Query:
            case TaskType::Command:
            case TaskType::Event:
            case TaskType::Wait:
            case TaskType::Subflow:
            case TaskType::Script:
            case TaskType::Terminate:
            case TaskType::LoopControl:
                break;
        }
        push(node.next);
        push(node.on_error);
        return targets;
    }

    std::vector<std::string> BlueprintValidator::reachableFrom(const ParsedWorkflow &workflow,
                                                                 const std::string &entry_id) {
        std::set<std::string> visited;
        std::vector<std::string> order;
        std::vector<std::string> stack{entry_id};
        while (!stack.empty()) {
            const std::string id = stack.back();
            stack.pop_back();
            if (id.empty() || visited.count(id)) continue;
            const NodePtr *node = workflow.find(id);
            if (node == nullptr) continue;
            visited.insert(id);
            order.push_back(id);
            for (const auto &target : outgoingTargets(**node)) stack.push_back(target);
        }
        return order;
    }

    std::vector<ValidationIssue> BlueprintValidator::collect(const ParsedWorkflow &workflow, const ParseOptions &options) {
        std::vector<ValidationIssue> issues;

        if (workflow.nodes.empty()) {
            addIssue(issues, ValidationIssue::Level::Error, "", "blueprint contains no nodes");
            return issues;
        }

        // --- ids and target existence --------------------------------------
        std::set<std::string> ids;
        for (const auto &node : workflow.nodes) {
            if (node->id.empty()) {
                addIssue(issues, ValidationIssue::Level::Error, "", "a node has an empty id");
                continue;
            }
            if (!ids.insert(node->id).second) {
                addIssue(issues, ValidationIssue::Level::Error, node->id, "duplicate node id");
            }
        }
        for (const auto &node : workflow.nodes) {
            for (const auto &target : outgoingTargets(*node)) {
                if (ids.count(target)) continue;
                // `schedule.body` may name a workflow instead of a node.
                if (node->getType() == TaskType::Schedule) {
                    const auto &schedule = static_cast<const ScheduleNode &>(*node);
                    if (schedule.workflow.has_value()) continue;
                }
                addIssue(issues, ValidationIssue::Level::Error, node->id,
                         "references unknown node id '" + target + "'");
            }
            if (node->next.has_value() && *node->next == node->id) {
                addIssue(issues, ValidationIssue::Level::Error, node->id, "'next' points at itself");
            }
            if (node->on_error.has_value() && *node->on_error == node->id) {
                addIssue(issues, ValidationIssue::Level::Error, node->id, "'on_error' points at itself");
            }
        }

        // --- loop_control must live inside a loop body ----------------------
        std::set<std::string> loop_body_nodes;
        for (const auto &node : workflow.nodes) {
            if (node->getType() != TaskType::Loop) continue;
            const auto &loop = static_cast<const LoopNode &>(*node);
            std::vector<std::string> frontier = loop.body;
            std::set<std::string> seen;
            while (!frontier.empty()) {
                const std::string id = frontier.back();
                frontier.pop_back();
                if (!seen.insert(id).second) continue;
                loop_body_nodes.insert(id);
                if (const NodePtr *child = workflow.find(id); child != nullptr) {
                    const auto &child_node = **child;
                    if (child_node.getType() == TaskType::Loop) {
                        for (const auto &nested : static_cast<const LoopNode &>(child_node).body) frontier.push_back(nested);
                    } else if (child_node.next.has_value()) {
                        frontier.push_back(*child_node.next);
                    }
                }
            }
        }
        for (const auto &node : workflow.nodes) {
            if (node->getType() != TaskType::LoopControl) continue;
            const auto &control = static_cast<const LoopControlNode &>(*node);
            if (control.loop.has_value() && !ids.count(*control.loop)) {
                addIssue(issues, ValidationIssue::Level::Error, node->id,
                         "'loop' targets unknown loop id '" + *control.loop + "'");
            }
            if (loop_body_nodes.count(node->id)) continue;
            if (control.loop.has_value()) {
                // An explicit loop id is enough: the body may be built dynamically.
                continue;
            }
            addIssue(issues, ValidationIssue::Level::Error, node->id,
                     std::string(control.action) + " node is not inside a loop body (give it a 'loop' id or move it "
                                                   "into the loop's 'body')");
        }

        // --- parallel branches must not contain themselves ------------------
        for (const auto &node : workflow.nodes) {
            if (node->getType() != TaskType::Parallel) continue;
            const auto &parallel = static_cast<const ParallelNode &>(*node);
            for (const auto &branch : parallel.branches) {
                if (std::find(branch.node_ids.begin(), branch.node_ids.end(), node->id) != branch.node_ids.end()) {
                    addIssue(issues, ValidationIssue::Level::Error, node->id, "parallel branch includes the node itself");
                }
            }
        }

        // --- subflow recursion on itself ------------------------------------
        for (const auto &node : workflow.nodes) {
            if (node->getType() != TaskType::Subflow) continue;
            const auto &subflow = static_cast<const SubflowNode &>(*node);
            if (!workflow.metadata.name.empty() && subflow.workflow == workflow.metadata.name) {
                addIssue(issues, ValidationIssue::Level::Error, node->id,
                         "subflow starts its own workflow ('" + subflow.workflow + "'), which recurses forever");
            }
        }

        // --- reachability ----------------------------------------------------
        const std::string entry = workflow.nodes.front()->id;
        std::set<std::string> reachable;
        for (const auto &id : reachableFrom(workflow, entry)) reachable.insert(id);
        for (const auto &node : workflow.nodes) {
            if (!node->enabled) continue; // disabled nodes are skipped, not "lost"
            if (reachable.count(node->id)) continue;
            // `schedule` and event-triggered nodes are entry points of their own.
            if (node->getType() == TaskType::Schedule) continue;
            addIssue(issues, ValidationIssue::Level::Warning, node->id,
                     "node is unreachable from the entry node '" + entry + "'");
        }

        // --- cycles (reported, not rejected: polling patterns are legal) ----
        std::map<std::string, int> colour; // 0 = white, 1 = grey, 2 = black
        for (const auto &node : workflow.nodes) colour[node->id] = 0;
        std::set<std::string> reported;
        size_t reported_count = 0;
        std::vector<std::string> path;
        std::function<void(const std::string &)> visit = [&](const std::string &id) {
            auto entry_it = workflow.find(id);
            if (entry_it == nullptr) return;
            colour[id] = 1;
            path.push_back(id);
            for (const auto &target : outgoingTargets(**entry_it)) {
                if (colour[target] == 1) {
                    auto begin = std::find(path.begin(), path.end(), target);
                    if (begin == path.end()) continue;
                    std::string cycle;
                    for (auto it = begin; it != path.end(); ++it) {
                        if (!cycle.empty()) cycle += " → ";
                        cycle += *it;
                    }
                    if (cycle.find(target) == std::string::npos) cycle = target + " → " + cycle;
                    if (reported.insert(cycle).second && reported_count < 20) {
                        ++reported_count;
                        addIssue(issues, ValidationIssue::Level::Warning, target, "control-flow cycle: " + cycle);
                    }
                } else if (colour[target] == 0) {
                    visit(target);
                }
            }
            path.pop_back();
            colour[id] = 2;
        };
        for (const auto &node : workflow.nodes) {
            if (colour[node->id] == 0) visit(node->id);
        }

        // --- terminality -----------------------------------------------------
        bool has_terminal = false;
        for (const auto &node : workflow.nodes) {
            if (node->getType() == TaskType::Terminate) has_terminal = true;
        }
        if (!has_terminal) {
            addIssue(issues, ValidationIssue::Level::Warning, "",
                     "workflow has no 'terminate' node; it finishes when control falls off the graph");
        }
        for (const auto &node : workflow.nodes) {
            if (node->getType() != TaskType::Terminate) continue;
            if (node->next.has_value()) {
                addIssue(issues, ValidationIssue::Level::Warning, node->id,
                          "'next' on a terminate node is ignored");
            }
        }

        // --- runaway `while` loops ------------------------------------------
        for (const auto &node : workflow.nodes) {
            if (node->getType() != TaskType::Loop) continue;
            const auto &loop = static_cast<const LoopNode &>(*node);
            if (!loop.condition.empty() && loop.collection.empty() && loop.count.empty() &&
                loop.max_iterations >= 1000) {
                addIssue(issues, ValidationIssue::Level::Warning, node->id,
                          "condition-driven loop relies on the default max_iterations (" +
                              std::to_string(loop.max_iterations) + "); set it explicitly if that is not intended");
            }
        }

        if (options.strict_fields) {
            // Already enforced by the parser; nothing extra to report here.
        }
        return issues;
    }

    void BlueprintValidator::validate(ParsedWorkflow &workflow, const ParseOptions &options,
                                      std::vector<std::string> &warnings) {
        const auto issues = collect(workflow, options);
        std::string first_error;
        for (const auto &issue : issues) {
            const std::string line = (issue.is_error() ? "ERROR" : "WARNING") +
                                     (issue.node_id.empty() ? std::string(" — ") : " [" + issue.node_id + "] ") +
                                     issue.message;
            warnings.push_back(line);
            if (issue.is_error() && first_error.empty()) first_error = line;
        }
        if (!first_error.empty()) {
            throw runtime::SapoError(runtime::ErrorCode::Parse,
                                     "blueprint failed validation: " + first_error.substr(6), json{{"issues", issues.size()}});
        }
    }

} // namespace sapo::parser
