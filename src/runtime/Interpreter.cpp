//
//  Sapo Engine — workflow interpreter.
//
#include "runtime/Interpreter.hpp"
#include "observability/Logger.hpp"
#include "parser/WorkflowParser.hpp"
#include "data/DataSourceProvider.hpp"
#include "runtime/SapoError.hpp"
#include "tasks/Tasks.hpp"
#include "util/Crypto.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <random>
#include <sstream>
#include <tuple>

using json = nlohmann::json;

namespace sapo::runtime {

    namespace {

        json idArray(const std::vector<std::string> &ids) {
            json out = json::array();
            for (const auto &id : ids) out.push_back(id);
            return out;
        }

        std::vector<std::string> idList(const json &ids) {
            std::vector<std::string> out;
            if (!ids.is_array()) return out;
            for (const auto &item : ids) {
                if (item.is_string()) out.push_back(item.get<std::string>());
            }
            return out;
        }

        int indexOfId(const json &ids, const std::string &id) {
            if (!ids.is_array()) return -1;
            for (size_t index = 0; index < ids.size(); ++index) {
                if (ids[index].is_string() && ids[index].get<std::string>() == id) return static_cast<int>(index);
            }
            return -1;
        }

        std::string outcomeName(const ControlSignal &signal) {
            if (signal::is<JumpTo>(signal)) return "jumped";
            if (signal::is<SuspendRequest>(signal)) return "suspended";
            if (signal::is<Terminate>(signal)) return "terminated";
            if (signal::is<LoopBreak>(signal) || signal::is<LoopContinue>(signal)) return "looped";
            return "success";
        }

    } // namespace

    // =========================================================================
    // small types
    // =========================================================================
    json ExecutionReport::toJson() const {
        json out{{"session_id", session_id},
                 {"execution_id", execution_id},
                 {"blueprint_id", blueprint_id},
                 {"status", status},
                 {"node_visits", node_visits},
                 {"elapsed_ms", elapsed_ms},
                 {"output", output}};
        if (!cursor.empty()) out["cursor"] = cursor;
        if (!prompt.empty()) out["prompt"] = prompt;
        if (!error_message.empty()) {
            out["error"] = json{{"code", error_code}, {"message", error_message}, {"node", error_node}};
            if (!error_data.is_null()) out["error"]["data"] = error_data;
        }
        if (!visited.empty()) out["visited"] = visited;
        if (!warnings.empty()) out["warnings"] = warnings;
        return out;
    }

    json Interpreter::CapturedError::toJson() const {
        json out{{"code", code}, {"message", message}};
        if (!node.empty()) out["node"] = node;
        if (!data.is_null()) out["data"] = data;
        return out;
    }

    Interpreter::CapturedError Interpreter::CapturedError::from(const SapoError &error) {
        CapturedError captured;
        captured.code = error.codeString();
        captured.message = error.message();
        captured.node = error.nodeId();
        captured.data = error.data();
        return captured;
    }

    Interpreter::CapturedError Interpreter::CapturedError::fromJson(const json &value) {
        CapturedError captured;
        captured.code = value.value("code", "INTERNAL_ERROR");
        captured.message = value.value("message", "");
        captured.node = value.value("node", "");
        if (value.contains("data")) captured.data = value["data"];
        return captured;
    }

    // =========================================================================
    Interpreter::Interpreter(TaskServices services, const tasks::TaskRegistry *tasks)
        : m_services(std::move(services)), m_tasks(tasks ? tasks : &tasks::TaskRegistry::defaults()) {}

    const tasks::ITask *Interpreter::taskFor(parser::TaskType type, const std::string &node_id) const {
        const tasks::ITask *task = m_tasks != nullptr ? m_tasks->find(type) : nullptr;
        if (task == nullptr) {
            throw SapoError(ErrorCode::Internal,
                            "no task implementation is registered for node type '" + std::string(parser::toString(type)) +
                                "'",
                            json::object(), node_id);
        }
        return task;
    }

    tasks::ExecutionContext Interpreter::makeExecution(RunState &state, const parser::NodePtr &node) {
        tasks::ExecutionContext execution(node, *state.context, m_services, state.session_id, state.execution_id,
                                         state.depth);
        execution.workflow_id = state.workflow_id;
        execution.locals = state.locals.is_object() ? state.locals : json::object();
        execution.data_sources = state.data_sources;
        if (!state.frames.empty()) {
            execution.iteration = state.frames.back().state.value("index", static_cast<int64_t>(-1));
        }
        return execution;
    }

    json Interpreter::buildDataSources(const parser::ParsedWorkflow &workflow) const {
        json out = json::object();
        for (const auto &node : workflow.nodes) {
            if (node->getType() != parser::TaskType::Action) continue;
            for (const auto &source : static_cast<const parser::ActionNode &>(*node).data_sources) {
                json entry{{"name", source.name},
                           {"provider", source.provider},
                           {"scope", source.scope},
                           {"config", source.config},
                           {"declared_by", node->id}};
                out[source.name] = std::move(entry);
            }
        }
        if (m_services.provider_config != nullptr) {
            for (const auto &declared : m_services.provider_config->dataSourceDeclarations()) {
                if (!declared.is_object() || !declared.contains("name") || !declared["name"].is_string()) continue;
                if (out.contains(declared["name"].get<std::string>())) continue;   // blueprint wins
                out[declared["name"].get<std::string>()] = declared;
            }
        }
        return out;
    }

    // =========================================================================
    // node activation (retry + error capture live here, not in the tasks)
    // =========================================================================
    ControlSignal Interpreter::activateNode(RunState &state, const parser::NodePtr &node) {
        const tasks::ITask *task = taskFor(node->getType(), node->id);
        const parser::RetryPolicy policy = node->retry.value_or(parser::RetryPolicy{});
        const int max_attempts = std::max(1, policy.max_attempts);

        for (int attempt = 1;; ++attempt) {
            obs::TraceSpan span;
            span.execution_id = state.execution_id;
            span.node_id = node->id;
            span.node_type = parser::toString(node->getType());
            span.start_ms = m_services.clock ? m_services.clock->now().count() : 0LL;
            span.depth = state.depth;
            if (m_services.traces) m_services.traces->begin(span);

            auto execution = makeExecution(state, node);
            try {
                ControlSignal signal = task->execute(execution);
                if (m_services.traces) {
                    m_services.traces->end(state.execution_id, node->id, outcomeName(signal));
                }
                if (m_services.metrics) m_services.metrics->increment("sapo.nodes.executed");
                if (!state.resume_node_id.empty() && state.resume_node_id == node->id) {
                    state.resume_node_id.clear();
                    if (state.locals.is_object()) {
                        state.locals.erase("input");
                        state.locals.erase("event");
                    }
                }
                return signal;
            } catch (const SapoError &error) {
                const bool attempts_left = attempt < max_attempts;
                bool retryable = false;
                if (attempts_left) {
                    if (!policy.retry_on.empty()) {
                        const std::string status = error.data().is_object() ? error.data().value("status", json()).dump()
                                                                          : std::string();
                        for (const auto &needle : policy.retry_on) {
                            if (needle == error.codeString() || (!status.empty() && needle == status)) {
                                retryable = true;
                                break;
                            }
                        }
                    } else {
                        retryable = error.code() == ErrorCode::Http || error.code() == ErrorCode::Timeout ||
                                    error.code() == ErrorCode::DataSource ||
                                    (error.code() == ErrorCode::HttpStatus && error.data().is_object() &&
                                     (error.data().value("status", 0) >= 500 || error.data().value("status", 0) == 429));
                    }
                }
                if (retryable) {
                    double delay = static_cast<double>(policy.backoff_ms) * std::pow(policy.multiplier, attempt - 1);
                    if (policy.jitter > 0.0) {
                        static thread_local std::mt19937 generator{std::random_device{}()};
                        std::uniform_real_distribution<double> spread(0.0, policy.jitter);
                        delay *= 1.0 + spread(generator);
                    }
                    auto delay_ms = static_cast<int64_t>(delay);
                    delay_ms = std::min<int64_t>(delay_ms, policy.max_backoff_ms);
                    delay_ms = std::min<int64_t>(delay_ms, m_services.limits.max_retry_delay_ms);
                    if (m_services.metrics) m_services.metrics->increment("sapo.nodes.retries");
                    if (m_services.logger) {
                        m_services.logger->warn("interpreter",
                                                "retrying node '" + node->id + "' (attempt " + std::to_string(attempt + 1) +
                                                    " of " + std::to_string(max_attempts) + ") after " +
                                                    std::to_string(delay_ms) + "ms: " + error.message());
                    }
                    if (m_services.delay_sink) m_services.delay_sink(delay_ms);
                    if (m_services.traces) {
                        m_services.traces->end(state.execution_id, node->id, "retrying", error.codeString(),
                                               error.message());
                    }
                    continue;
                }
                if (m_services.traces) {
                    m_services.traces->end(state.execution_id, node->id, "failed", error.codeString(), error.message());
                }
                if (m_services.metrics) m_services.metrics->increment("sapo.nodes.failed");
                throw;
            } catch (const std::exception &e) {
                if (m_services.traces) {
                    m_services.traces->end(state.execution_id, node->id, "failed", "INTERNAL_ERROR", e.what());
                }
                throw SapoError(ErrorCode::Internal,
                                std::string(parser::toString(node->getType())) + " node '" + node->id +
                                    "' failed unexpectedly: " + e.what(),
                                json::object(), node->id);
            }
        }
    }

    // =========================================================================
    // frames
    // =========================================================================
    void Interpreter::pushFrame(RunState &state, const std::string &kind, const std::string &node_id,
                                const std::vector<std::string> &ids, const std::string &resume_target,
                                const json &frame_state) {
        RuntimeContext::FrameState frame;
        frame.kind = kind;
        frame.node_id = node_id;
        frame.position = 0;
        frame.resume_target = resume_target;
        frame.state = frame_state.is_object() ? frame_state : json::object();
        frame.state["ids"] = idArray(ids);
        state.frames.push_back(std::move(frame));
        if (state.context) state.context->setFrames(state.frames);
    }

    void Interpreter::bindIteration(RunState &state, const RuntimeContext::FrameState &frame) {
        const json &fs = frame.state;
        const std::string mode = fs.value("mode", "");
        if (mode.empty()) return;
        const int64_t index = fs.value("index", 0LL);
        const json items = fs.value("items", json::array());
        if (items.is_array() && index >= 0 && static_cast<size_t>(index) < items.size()) {
            const std::string iterator = fs.value("iterator", "item");
            if (!iterator.empty()) state.locals[iterator] = items[static_cast<size_t>(index)];
        }
        const std::string index_variable = fs.value("index_variable", "index");
        if (!index_variable.empty()) state.locals[index_variable] = index;
        state.locals["iteration"] = index;
    }

    // =========================================================================
    // control-flow nodes
    // =========================================================================
    ControlSignal Interpreter::stepLoop(const parser::ParsedWorkflow &workflow, RunState &state,
                                        const parser::LoopNode &node) {
        const parser::NodePtr shared = [&] {
            const parser::NodePtr *found = workflow.find(node.id);
            return found != nullptr ? *found : parser::NodePtr();
        }();
        if (!shared) {
            throw SapoError(ErrorCode::Routing, "loop node vanished from the workflow index", json::object(), node.id);
        }
        auto execution = makeExecution(state, shared);
        const json items = tasks::LoopTask::items(node, execution);

        const bool while_mode = items.is_null();
        const int64_t total = while_mode ? node.max_iterations : static_cast<int64_t>(items.size());
        if (!while_mode && total == 0) return Continue{};   // zero iterations, fall through

        json frame_state{{"mode", while_mode ? "while" : "collection"},
                         {"items", while_mode ? json::array() : items},
                         {"index", 0LL},
                         {"total", total},
                         {"max_iterations", node.max_iterations},
                         {"iterator", node.iterator},
                         {"index_variable", node.index},
                         {"on_item_error", node.on_item_error},
                         {"loop_id", node.id}};
        pushFrame(state, "loop", node.id, node.body, node.next.value_or(""), frame_state);
        bindIteration(state, state.frames.back());
        return JumpTo{node.body.front()};
    }

    ControlSignal Interpreter::stepTry(const parser::ParsedWorkflow &workflow, RunState &state,
                                       const parser::TryNode &node) {
        (void) workflow;
        json frame_state = json::object();
        frame_state["phase"] = "body";
        frame_state["catch_ids"] = idArray(node.catch_body);
        frame_state["finally_ids"] = idArray(node.finally_body);
        frame_state["error_variable"] = node.error_variable;
        frame_state["catch_when"] = node.catch_when;   // {error code → handler node}
        pushFrame(state, "try", node.id, node.body, node.next.value_or(""), frame_state);
        return JumpTo{node.body.front()};
    }

    ControlSignal Interpreter::stepCondition(const parser::ParsedWorkflow &workflow, RunState &state,
                                             const parser::ConditionNode &node) {
        const parser::NodePtr shared = [&] {
            const parser::NodePtr *found = workflow.find(node.id);
            return found != nullptr ? *found : parser::NodePtr();
        }();
        if (!shared) {
            throw SapoError(ErrorCode::Routing, "condition node vanished from the workflow index", json::object(),
                             node.id);
        }
        auto execution = makeExecution(state, shared);
        auto branch = tasks::ConditionTask::branch(node, execution);
        if (!branch.body.empty()) {
            json frame_state{{"branch", std::string(branch.taken ? "then" : "else")}};
            pushFrame(state, "body", node.id, branch.body, node.next.value_or(""), frame_state);
            return JumpTo{branch.body.front()};
        }
        if (!branch.target.empty()) return JumpTo{branch.target};
        return Continue{};
    }

    ControlSignal Interpreter::runParallel(const parser::ParsedWorkflow &workflow, RunState &state,
                                           const parser::ParallelNode &node) {
        struct Outcome {
            json writes;
            std::vector<std::string> visited;
            std::string status{"completed"};
            json error;
            json terminate;
            std::string suspend_reason;
            std::string suspend_node;
            size_t visits{0};
            std::string failure;
        };

        std::vector<Outcome> outcomes(node.branches.size());
        std::vector<std::shared_ptr<RuntimeContext>> forks;
        forks.reserve(node.branches.size());
        for (size_t index = 0; index < node.branches.size(); ++index) {
            forks.push_back(std::shared_ptr<RuntimeContext>(state.context->fork().release()));
        }

        std::vector<std::function<void()>> jobs;
        jobs.reserve(node.branches.size());
        for (size_t index = 0; index < node.branches.size(); ++index) {
            jobs.emplace_back([this, &workflow, &state, &node, &forks, &outcomes, index] {
                Outcome &outcome = outcomes[index];
                const auto &branch = node.branches[index];
                if (branch.node_ids.empty()) return;
                RunState sub;
                sub.context = forks[index];
                sub.cursor = branch.node_ids.front();
                sub.session_id = state.session_id;
                sub.execution_id = state.execution_id + "#" + (branch.label.value_or(std::to_string(index)));
                sub.workflow_id = state.workflow_id;
                sub.depth = state.depth;
                sub.locals = state.locals;
                sub.data_sources = state.data_sources;
                sub.branch_mode = true;
                sub.started_ms = m_services.clock ? m_services.clock->now().count() : 0LL;
                const size_t remaining = state.budget > state.visits ? state.budget - state.visits : 1;
                sub.budget = std::min(m_services.limits.max_branch_visits == 0
                                           ? remaining
                                           : std::min(remaining, m_services.limits.max_branch_visits),
                                       remaining);
                pushFrame(sub, "branch", node.id, branch.node_ids, "", json{{"branch_index", index}});
                try {
                    drive(workflow, sub);
                } catch (const SapoError &error) {
                    outcome.failure = error.message();
                    outcome.error = CapturedError::from(error).toJson();
                    outcome.status = "failed";
                }
                outcome.visits = sub.visits;
                outcome.visited = sub.visited;
                outcome.writes = sub.context->snapshot();
                // Only the keys the branch actually touched are merged back.
                json changed = json::array();
                for (const auto &key : sub.context->changedSince(0)) changed.push_back(key);
                outcome.writes = json{{"changed", changed}, {"snapshot", sub.context->snapshot()}};
                outcome.status = sub.status;
                if (!sub.error.empty()) {
                    json error_json{{"code", sub.error_code}, {"message", sub.error}, {"node", sub.error_node}};
                    if (!sub.error_data.is_null()) error_json["data"] = sub.error_data;
                    outcome.error = std::move(error_json);
                }
                if (sub.status == "awaiting_input" || sub.status == "suspended") {
                    outcome.suspend_reason = sub.status;
                    outcome.suspend_node = sub.current_node_id;
                }
                if (sub.status == "terminated" || sub.status == "failed-by-terminate") {
                    outcome.terminate = json{{"status", sub.terminate_status}, {"payload", sub.terminate_payload}};
                }
            });
        }

        if (m_services.pool != nullptr) {
            m_services.pool->runAll(jobs);
        } else {
            for (auto &job : jobs) job();
        }

        const auto policy = tasks::ParallelTask::mergePolicy(node.merge_policy);
        std::vector<std::string> conflicts_total;
        for (size_t index = 0; index < outcomes.size(); ++index) {
            Outcome &outcome = outcomes[index];
            if (!outcome.error.is_null() && node.fail_fast) {
                throw SapoError(ErrorCode::Capability,
                                "parallel branch '" + node.branches[index].label.value_or(std::to_string(index)) +
                                    "' failed: " + outcome.error.value("message", ""),
                                outcome.error.contains("data") ? outcome.error["data"] : json(),
                                outcome.error.value("node", node.id));
            }
            if (!outcome.suspend_reason.empty()) {
                throw SapoError(ErrorCode::Validation,
                                "a parallel branch cannot suspend (" + outcome.suspend_reason + "); move the wait or "
                                "prompt outside the parallel section",
                                json{{"branch", index}}, node.id);
            }
            if (!outcome.writes.is_object()) continue;
            const std::set<std::string> keys = [&] {
                std::set<std::string> keys;
                for (const auto &key : outcome.writes.value("changed", json::array())) {
                    if (key.is_string()) keys.insert(key.get<std::string>());
                }
                return keys;
            }();
            if (keys.empty()) continue;
            auto conflicts = state.context->mergeFrom(*forks[index], keys, policy);
            conflicts_total.insert(conflicts_total.end(), conflicts.begin(), conflicts.end());
            state.visits += outcome.visits;
            state.visited.insert(state.visited.end(), outcome.visited.begin(), outcome.visited.end());
        }
        if (!conflicts_total.empty() && policy == RuntimeContext::MergePolicy::FailConflicts) {
            json detail = json::array();
            for (const auto &key : conflicts_total) detail.push_back(key);
            throw SapoError(ErrorCode::Validation,
                            "parallel branches wrote the same context keys ('" + node.merge_policy + "')",
                            json{{"conflicts", detail}}, node.id);
        }
        if (conflicts_total.empty() == false && policy == RuntimeContext::MergePolicy::SkipConflicts &&
            m_services.logger != nullptr) {
            m_services.logger->warn("interpreter", "parallel merge skipped " + std::to_string(conflicts_total.size()) +
                                                        " conflicting writes for node '" + node.id + "'");
        }
        for (const auto &outcome : outcomes) {
            if (!outcome.error.is_null() && !node.fail_fast) {
                state.warnings.push_back("parallel branch failed (fail_fast=false): " +
                                         outcome.error.value("message", std::string()));
            }
            if (!outcome.terminate.is_null()) {
                state.terminate_payload = outcome.terminate.value("payload", json::object());
                state.terminate_status = outcome.terminate.value("status", "success");
                state.status = state.terminate_status == "success" ? "terminated" : "failed";
                state.finished = true;
                return Continue{};
            }
        }
        return Continue{};
    }

    // =========================================================================
    // routing
    // =========================================================================
    void Interpreter::resolveJump(RunState &state, const std::string &target) {
        if (target.empty()) {
            state.cursor.clear();
            return;
        }
        // A target inside a live frame body steps that frame; anything else escapes
        // the frames that do not contain it.
        for (size_t depth = state.frames.size(); depth > 0; --depth) {
            auto &frame = state.frames[depth - 1];
            const int index = indexOfId(frame.state["ids"], target);
            if (index < 0) continue;
            state.frames.resize(depth);   // drop any frames nested inside the target's
            frame.position = index;
            if (frame.kind == "loop") bindIteration(state, frame);
            state.cursor = target;
            return;
        }
        state.frames.clear();
        if (state.context) state.context->setFrames(state.frames);
        state.cursor = target;
    }

    void Interpreter::advanceAfterNode(const parser::ParsedWorkflow &workflow, RunState &state) {
        std::string current = state.current_node_id;
        for (;;) {
            if (state.frames.empty()) {
                const parser::NodePtr *node = workflow.find(current);
                if (node != nullptr && (*node)->next.has_value()) {
                    state.cursor = *(*node)->next;
                    return;
                }
                state.cursor.clear();
                if (state.status == "running") state.status = "completed";
                state.finished = true;
                return;
            }

            RuntimeContext::FrameState &frame = state.frames.back();
            const int index = indexOfId(frame.state["ids"], current);

            // A body node with an explicit `next` that leaves the frame escapes it
            // (that is the `goto`-out-of-a-loop path the grammar allows).
            if (index >= 0) {
                const parser::NodePtr *stepping = workflow.find(current);
                if (stepping != nullptr && (*stepping)->next.has_value() &&
                    indexOfId(frame.state["ids"], *(*stepping)->next) < 0) {
                    resolveJump(state, *(*stepping)->next);
                    return;
                }
            }

            if (index < 0) {
                // `current` is not part of this frame: complete the frame and keep
                // unwinding outwards (a nested body ending inside an outer loop).
                const std::string owner = frame.node_id;
                if (frame.kind == "try") {
                    advanceTryPhase(workflow, state);
                    if (!state.cursor.empty() || state.finished) return;
                    current = owner;
                    continue;
                }
                if (frame.kind == "loop") {
                    startNextIteration(workflow, state);
                    return;
                }
                if (frame.kind == "branch") {
                    state.branch_finished = true;
                    state.finished = true;
                    state.cursor.clear();
                    return;
                }
                const std::string resume = frame.resume_target;
                state.frames.pop_back();
                if (state.context) state.context->setFrames(state.frames);
                if (!resume.empty()) {
                    state.cursor = resume;
                    return;
                }
                current = owner;
                continue;
            }

            // Still inside this frame's body: step to the sibling, whatever the
            // frame kind is. Only a finished body triggers frame completion.
            const json ids = frame.state["ids"];
            if (index + 1 < static_cast<int>(ids.size())) {
                frame.position = static_cast<int64_t>(index + 1);
                state.cursor = ids[static_cast<size_t>(index) + 1].get<std::string>();
                if (state.context) state.context->setFrames(state.frames);
                return;
            }
            if (frame.kind == "loop") {
                startNextIteration(workflow, state);
                return;
            }
            if (frame.kind == "try") {
                advanceTryPhase(workflow, state);
                if (!state.cursor.empty() || state.finished) return;
                current = frame.node_id;
                continue;
            }
            if (frame.kind == "branch") {
                state.branch_finished = true;
                state.finished = true;
                state.cursor.clear();
                if (state.status == "running") state.status = "completed";
                return;
            }
            const std::string resume = frame.resume_target;
            const std::string owner = frame.node_id;
            state.frames.pop_back();
            if (state.context) state.context->setFrames(state.frames);
            if (!resume.empty()) {
                state.cursor = resume;
                return;
            }
            current = owner;
        }
    }

    // =========================================================================
    // main loop
    // =========================================================================
    void Interpreter::drive(const parser::ParsedWorkflow &workflow, RunState &state) {
        while (!state.finished) {
            if (!state.pending_error.is_null()) {
                const auto error = CapturedError::fromJson(state.pending_error);
                state.pending_error = json();
                if (!handleError(workflow, state, error)) {
                    state.status = "failed";
                    state.error = error.message;
                    state.error_code = error.code;
                    state.error_node = error.node;
                    state.error_data = error.data;
                    state.finished = true;
                    return;
                }
                continue;
            }

            if (state.cursor.empty()) {
                if (state.status == "running") state.status = "completed";
                state.finished = true;
                return;
            }

            const parser::NodePtr *found = workflow.find(state.cursor);
            if (found == nullptr) {
                CapturedError error;
                error.code = "ROUTING_ERROR";
                error.message = "control reached unknown node id '" + state.cursor + "'";
                error.node = state.current_node_id;
                state.cursor.clear();
                if (!handleError(workflow, state, error)) {
                    state.status = "failed";
                    state.error = error.message;
                    state.error_code = error.code;
                    state.finished = true;
                    return;
                }
                continue;
            }
            const parser::NodePtr node = *found;
            state.current_node_id = node->id;

            if (++state.visits > state.budget) {
                state.status = "failed";
                state.error_code = "LIMIT_EXCEEDED";
                state.error = "execution budget of " + std::to_string(state.budget) + " node activations exceeded "
                              "(blueprint has a runaway loop; set max_iterations or max_node_visits)";
                state.error_node = node->id;
                state.finished = true;
                return;
            }
            state.visited.push_back(node->id);

            if (!node->enabled) {
                if (m_services.logger) {
                    m_services.logger->log(obs::LogLevel::Debug, "interpreter",
                                           "node '" + node->id + "' is disabled", json::object(), state.execution_id,
                                           node->id);
                }
                advanceAfterNode(workflow, state);
                continue;
            }

            ControlSignal signal = Continue{};
            try {
                switch (node->getType()) {
                    case parser::TaskType::Loop:
                        signal = stepLoop(workflow, state, static_cast<const parser::LoopNode &>(*node));
                        break;
                    case parser::TaskType::Try:
                        signal = stepTry(workflow, state, static_cast<const parser::TryNode &>(*node));
                        break;
                    case parser::TaskType::Condition:
                        signal = stepCondition(workflow, state, static_cast<const parser::ConditionNode &>(*node));
                        break;
                    case parser::TaskType::Parallel:
                        signal = runParallel(workflow, state, static_cast<const parser::ParallelNode &>(*node));
                        break;
                    default:
                        signal = activateNode(state, node);
                        break;
                }
            } catch (const SapoError &error) {
                const auto captured = CapturedError::from(error);
                if (!handleError(workflow, state, captured)) {
                    state.status = "failed";
                    state.error = captured.message;
                    state.error_code = captured.code;
                    state.error_node = captured.node.empty() ? node->id : captured.node;
                    state.error_data = captured.data;
                    state.finished = true;
                    return;
                }
                continue;
            }

            if (m_services.logger && m_services.logger->level() <= obs::LogLevel::Debug) {
                m_services.logger->log(obs::LogLevel::Debug, "interpreter",
                                       "node '" + node->id + "' (" + parser::toString(node->getType()) +
                                           ") → " + outcomeName(signal),
                                       json{{"frames", state.frames.size()}, {"locals", state.locals}},
                                       state.execution_id, node->id);
            }

            if (state.finished) return;

            if (const auto *jump = signal::get<JumpTo>(signal)) {
                resolveJump(state, jump->target);
                continue;
            }
            if (const auto *suspend = signal::get<SuspendRequest>(signal)) {
                state.suspend_request = json{{"reason", suspend->reason},
                                             {"node", suspend->node_id},
                                             {"input_variable", suspend->input_variable},
                                             {"prompt", suspend->prompt},
                                             {"data", suspend->data}};
                if (suspend->resume_at_ms.has_value()) {
                    state.suspend_request["resume_at_ms"] = *suspend->resume_at_ms;
                }
                if (suspend->timeout_ms.has_value()) state.suspend_request["timeout_ms"] = *suspend->timeout_ms;
                if (suspend->event_name.has_value()) state.suspend_request["event_name"] = *suspend->event_name;
                std::string resume_cursor = suspend->resume_node;
                if (resume_cursor.empty()) {
                    // A prompt is re-entered on resume: the reply still has to be
                    // validated against `input_validation`, and the binding written.
                    resume_cursor = suspend->reason == "input" ? node->id : node->next.value_or("");
                }
                // A wait that must re-check its condition, or a prompt whose reply
                // still has to be validated, re-enters the same node.
                if (resume_cursor.empty() && suspend->reason == "timer" &&
                    suspend->data.value("await_condition", "").empty() == false) {
                    resume_cursor = node->id;
                }
                if (resume_cursor.empty()) {
                    // no successor: the session ends after the prompt reply is stored
                    resume_cursor.clear();
                    state.status = suspend->reason == "input" ? "awaiting_input" : "suspended";
                    state.suspend_cursor.clear();
                    state.cursor.clear();
                    state.finished = true;
                    return;
                }
                state.status = suspend->reason == "input" ? "awaiting_input" : "suspended";
                state.suspend_cursor = resume_cursor;
                state.cursor = resume_cursor;
                state.finished = true;
                return;
            }
            if (const auto *terminate = signal::get<Terminate>(signal)) {
                state.terminate_status = terminate->status;
                state.terminate_payload = terminate->payload;
                if (terminate->message.has_value() && !terminate->message->empty()) {
                    state.terminate_payload["message"] = *terminate->message;
                }
                state.status = terminate->status == "success" ? "terminated" : "failed";
                if (terminate->status != "success") {
                    state.error_code = terminate->error_code.value_or("WORKFLOW_FAILED");
                    state.error = terminate->message.value_or("workflow terminated with status '" + terminate->status + "'");
                    state.error_node = node->id;
                }
                state.finished = true;
                return;
            }
            if (const auto *brk = signal::get<LoopBreak>(signal)) {
                if (!finishLoop(workflow, state, brk->loop.value_or(""))) {
                    state.status = "failed";
                    state.error_code = "ROUTING_ERROR";
                    state.error = "'break' node '" + node->id + "' is not inside a loop";
                    state.error_node = node->id;
                    state.finished = true;
                    return;
                }
                continue;
            }
            if (const auto *cont = signal::get<LoopContinue>(signal)) {
                if (!nextIterationFor(workflow, state, cont->loop.value_or(""))) {
                    state.status = "failed";
                    state.error_code = "ROUTING_ERROR";
                    state.error = "'continue' node '" + node->id + "' is not inside a loop";
                    state.error_node = node->id;
                    state.finished = true;
                    return;
                }
                continue;
            }

            advanceAfterNode(workflow, state);
        }
    }


    // =========================================================================
    // frame bookkeeping
    // =========================================================================
    void Interpreter::eraseIterationLocals(RunState &state, const json &frame_state) {
        if (!frame_state.is_object() || !state.locals.is_object()) return;
        for (const char *key : {"iterator", "index_variable"}) {
            const std::string name = frame_state.value(key, "");
            if (!name.empty()) state.locals.erase(name);
        }
        state.locals.erase("iteration");
    }

    void Interpreter::closeFrame(const parser::ParsedWorkflow &workflow, RunState &state) {
        if (state.frames.empty()) {
            state.cursor.clear();
            if (state.status == "running") state.status = "completed";
            state.finished = true;
            return;
        }
        const std::string owner = state.frames.back().node_id;
        const json frame_state = state.frames.back().state;
        state.frames.pop_back();
        if (state.context) state.context->setFrames(state.frames);
        eraseIterationLocals(state, frame_state);
        // Let the enclosing frame (or the node's `next` edge) decide what is next.
        state.current_node_id = owner;
        advanceAfterNode(workflow, state);
    }

    void Interpreter::advanceTryPhase(const parser::ParsedWorkflow &workflow, RunState &state) {
        if (state.frames.empty() || state.frames.back().kind != "try") return;
        const json snapshot = state.frames.back().state;
        const std::string phase = snapshot.value("phase", "body");
        const auto finally_ids = idList(snapshot.value("finally_ids", json::array()));
        const bool propagate = snapshot.contains("propagate");

        if (phase == "body" && !finally_ids.empty()) {
            auto &frame = state.frames.back();
            frame.state["phase"] = "finally";
            frame.state["ids"] = idArray(finally_ids);
            frame.position = 0;
            state.cursor = finally_ids.front();
            if (state.context) state.context->setFrames(state.frames);
            return;
        }
        if (phase == "catch" && !finally_ids.empty()) {
            auto &frame = state.frames.back();
            frame.state["phase"] = "finally";
            frame.state["ids"] = idArray(finally_ids);
            frame.position = 0;
            state.cursor = finally_ids.front();
            if (state.context) state.context->setFrames(state.frames);
            return;
        }
        if (propagate) {
            state.pending_error = snapshot["propagate"];
            state.locals["error"] = state.pending_error;
        }
        closeFrame(workflow, state);
    }

    bool Interpreter::nextIterationFor(const parser::ParsedWorkflow &workflow, RunState &state,
                                       const std::string &loop_id) {
        size_t depth = state.frames.size();
        for (; depth > 0; --depth) {
            const auto &frame = state.frames[depth - 1];
            if (frame.kind != "loop") continue;
            if (!loop_id.empty() && frame.state.value("loop_id", "") != loop_id && frame.node_id != loop_id) continue;
            break;
        }
        if (depth == 0) return false;

        state.frames.resize(depth);
        json fs = state.frames.back().state;
        const int64_t index = fs.value("index", 0LL) + 1;
        const int64_t total = fs.value("total", 0LL);
        const int64_t cap = fs.value("max_iterations", 1000LL);
        const json ids = fs.value("ids", json::array());
        const std::string owner = state.frames.back().node_id;
        const bool capped = index >= cap;
        if (capped && fs.value("mode", "") == "while") {
            // A while-loop that stops on its cap is a blueprint smell: say so.
            state.warnings.push_back("loop '" + owner + "' stopped after reaching max_iterations=" +
                                     std::to_string(cap) + " (guard may still be true)");
        }
        bool more = !capped && index < total;
        if (more && fs.value("mode", "") == "while") {
            const parser::NodePtr *found = workflow.find(owner);
            if (found != nullptr && (*found)->getType() == parser::TaskType::Loop) {
                auto execution = makeExecution(state, *found);
                more = tasks::LoopTask::guard(static_cast<const parser::LoopNode &>(**found), execution);
            } else {
                more = false;
            }
        }

        if (!more) {
            state.frames.pop_back();
            if (state.context) state.context->setFrames(state.frames);
            eraseIterationLocals(state, fs);
            state.current_node_id = owner;
            advanceAfterNode(workflow, state);
            return true;
        }

        auto &frame = state.frames.back();
        frame.state["index"] = index;
        frame.position = 0;
        bindIteration(state, frame);
        if (state.context) state.context->setFrames(state.frames);
        state.cursor = ids.is_array() && !ids.empty() ? ids.front().get<std::string>() : std::string();
        return true;
    }

    void Interpreter::startNextIteration(const parser::ParsedWorkflow &workflow, RunState &state) {
        nextIterationFor(workflow, state, "");
    }

    bool Interpreter::finishLoop(const parser::ParsedWorkflow &workflow, RunState &state, const std::string &loop_id) {
        size_t depth = state.frames.size();
        for (; depth > 0; --depth) {
            const auto &frame = state.frames[depth - 1];
            if (frame.kind != "loop") continue;
            if (!loop_id.empty() && frame.state.value("loop_id", "") != loop_id && frame.node_id != loop_id) continue;
            break;
        }
        if (depth == 0) return false;
        state.frames.resize(depth);
        closeFrame(workflow, state);
        return true;
    }

    // =========================================================================
    // error routing
    // =========================================================================
    bool Interpreter::handleError(const parser::ParsedWorkflow &workflow, RunState &state,
                                  const CapturedError &error) {
        const json error_json = error.toJson();
        const parser::NodePtr *current =
            state.current_node_id.empty() ? nullptr : workflow.find(state.current_node_id);

        // 1) the node's own `on_error` handler wins.
        if (current != nullptr && (*current)->on_error.has_value() && !(*current)->on_error->empty()) {
            state.locals["error"] = error_json;
            resolveJump(state, *(*current)->on_error);
            if (m_services.metrics) m_services.metrics->increment("sapo.nodes.error_handled");
            return true;
        }

        // 2) the nearest frame that can recover (nearest handler wins).
        for (size_t depth = state.frames.size(); depth > 0; --depth) {
            auto &frame = state.frames[depth - 1];
            if (frame.kind == "try" && frame.state.value("phase", "body") == "body") {
                const auto catch_ids = idList(frame.state.value("catch_ids", json::array()));
                // `catch: {when: {"HTTP_STATUS_ERROR": "node"}}` routes by error class.
                std::string routed;
                const json when = frame.state.value("catch_when", json::object());
                if (when.is_object()) {
                    const auto route = when.find(error.code);
                    if (route != when.end() && route->is_string()) routed = route->get<std::string>();
                }
                if (!routed.empty() && workflow.find(routed) == nullptr) {
                    routed.clear();   // a stale handler id must not mask the catch body
                }
                std::vector<std::string> handler_ids = catch_ids;
                if (routed.empty() && catch_ids.empty() && !when.is_null() && when.is_object() && !when.empty()) {
                    continue;   // this catch only handles listed codes
                }
                if (!routed.empty()) handler_ids = {routed};
                if (!handler_ids.empty()) {
                    state.frames.resize(depth);
                    auto &try_frame = state.frames.back();
                    try_frame.state["phase"] = "catch";
                    try_frame.state["ids"] = idArray(handler_ids);
                    try_frame.position = 0;
                    state.cursor = handler_ids.front();
                    state.locals["error"] = error_json;
                    const std::string variable = try_frame.state.value("error_variable", "error");
                    if (!variable.empty() && variable != "error") state.locals[variable] = error_json;
                    if (state.context) {
                        state.context->setFrames(state.frames);
                        if (!variable.empty()) state.context->setVariable(variable, error_json);
                    }
                    if (m_services.metrics) m_services.metrics->increment("sapo.nodes.error_caught");
                    if (m_services.logger) {
                        m_services.logger->warn("interpreter",
                                                 "error caught by try node '" + try_frame.node_id + "': [" + error.code +
                                                     "] " + error.message);
                    }
                    return true;
                }
                const auto finally_ids = idList(frame.state.value("finally_ids", json::array()));
                state.frames.resize(depth);
                auto &try_frame = state.frames.back();
                if (!finally_ids.empty()) {
                    // cleanup first, then the error keeps travelling outwards
                    try_frame.state["phase"] = "finally";
                    try_frame.state["ids"] = idArray(finally_ids);
                    try_frame.position = 0;
                    try_frame.state["propagate"] = error_json;
                    state.cursor = finally_ids.front();
                    if (state.context) state.context->setFrames(state.frames);
                    return true;
                }
                state.pending_error = error_json;
                closeFrame(workflow, state);
                return true;
            }
            if (frame.kind == "loop" && frame.state.value("on_item_error", "fail") != "fail") {
                state.warnings.push_back("loop '" + frame.node_id + "' skipped iteration " +
                                         std::to_string(frame.state.value("index", 0LL)) + " after error [" + error.code +
                                         "] " + error.message);
                if (m_services.metrics) m_services.metrics->increment("sapo.nodes.loop_item_errors");
                nextIterationFor(workflow, state, "");
                return true;
            }
        }
        return false;
    }

    // =========================================================================
    // run / resume
    // =========================================================================
    ExecutionReport Interpreter::run(const parser::ParsedWorkflow &workflow, const json &input, RunOptions options) {
        ExecutionReport report;
        RunState state;
        state.context = std::make_shared<RuntimeContext>();
        state.session_id = options.session_id.empty() ? util::uuidV4() : options.session_id;
        state.execution_id = options.execution_id.empty() ? util::uuidV4() : options.execution_id;
        state.workflow_id = workflow.metadata.name;
        state.budget = options.max_node_visits != 0 ? options.max_node_visits : m_services.limits.max_node_visits;
        state.depth = options.depth;
        state.started_ms = m_services.clock ? m_services.clock->now().count() : 0LL;
        state.data_sources = buildDataSources(workflow);
        report.session_id = state.session_id;
        report.execution_id = state.execution_id;

        if (m_services.data_sources != nullptr && state.data_sources.is_object() && !state.data_sources.empty()) {
            std::vector<data::DataSourceConfig> configs;
            for (const auto &item : state.data_sources.items()) {
                const json &declared = item.value();
                data::DataSourceConfig config;
                config.name = declared.value("name", item.key());
                config.scope = declared.value("scope", "internal");
                config.provider = declared.value("provider", "");
                config.config = declared.value("config", json::object());
                config.origin = declared.value("origin", "blueprint");
                configs.push_back(std::move(config));
            }
            const auto problems = m_services.data_sources->validate(configs);
            if (!problems.empty()) {
                json details = json::array();
                for (const auto &problem : problems) details.push_back(problem);
                state.status = "failed";
                state.error_code = "VALIDATION_ERROR";
                state.error = "data source configuration is invalid: " + details.dump();
                state.finished = true;
                finishRun(workflow, state, options, report);
                return report;
            }
        }

        if (workflow.metadata.defaults.is_object()) {
            for (const auto &item : workflow.metadata.defaults.items()) {
                state.context->setByPath(item.key(), item.value());
            }
        }
        if (input.is_object()) {
            for (const auto &item : input.items()) state.context->setByPath(item.key(), item.value());
        } else if (!input.is_null()) {
            state.context->setVariable("input", input);
        }
        state.context->setVariable("session", json{{"id", state.session_id},
                                                    {"execution_id", state.execution_id},
                                                    {"workflow", state.workflow_id},
                                                    {"started_at_ms", state.started_ms},
                                                    {"correlation_id", options.correlation_id},
                                                    {"depth", state.depth}});
        if (m_services.state_store != nullptr && options.persist) {
            auto parent = m_services.state_store->load(state.session_id);
            if (parent.has_value()) {
                state.workflow_id = parent->blueprint_id;
                if (!parent->correlation_id.empty()) report.correlation_id = parent->correlation_id;
            }
        }

        state.cursor = options.start_node.empty() ? workflow.entry_id : options.start_node;
        if (state.cursor.empty() || workflow.find(state.cursor) == nullptr) {
            if (!options.start_node.empty() && workflow.find(options.start_node) == nullptr) {
                state.status = "failed";
                state.error_code = "ROUTING_ERROR";
                state.error = "start node '" + options.start_node + "' is not part of workflow '" + state.workflow_id + "'";
                finishRun(workflow, state, options, report);
                return report;
            }
            state.status = "completed";   // empty blueprint
            state.finished = true;
            finishRun(workflow, state, options, report);
            return report;
        }

        if (m_services.metrics) m_services.metrics->increment("sapo.sessions.started");
        if (m_services.logger) {
            m_services.logger->debug("interpreter", "session " + state.session_id + " executing workflow '" +
                                                         state.workflow_id + "' from node '" + state.cursor + "'");
        }

        drive(workflow, state);
        finishRun(workflow, state, options, report);
        return report;
    }

    ExecutionReport Interpreter::resume(const parser::ParsedWorkflow &workflow, SessionCheckpoint checkpoint,
                                        const json &input, bool timed_out) {
        ExecutionReport report;
        RunOptions options;
        options.persist = true;
        options.correlation_id = checkpoint.correlation_id;
        options.max_node_visits = m_services.limits.max_node_visits;

        RunState state;
        state.context = std::make_shared<RuntimeContext>();
        if (checkpoint.context.is_object()) state.context->restore(checkpoint.context);
        state.frames = RuntimeContext::framesFromJson(checkpoint.frames);
        state.context->setFrames(state.frames);
        // Iteration locals are per-activation, so a resumed loop must re-bind its
        // iterator before the body runs again.
        for (const auto &frame : state.frames) {
            if (frame.kind == "loop") bindIteration(state, frame);
        }
        state.session_id = checkpoint.session_id;
        state.execution_id = checkpoint.execution_id.empty() ? util::uuidV4() : checkpoint.execution_id;
        state.workflow_id = checkpoint.blueprint_id;
        state.depth = checkpoint.depth;
        state.visits = checkpoint.node_visits;
        state.budget = m_services.limits.max_node_visits;
        state.started_ms = m_services.clock ? m_services.clock->now().count() : 0LL;
        state.data_sources = buildDataSources(workflow);
        report.session_id = state.session_id;
        report.execution_id = state.execution_id;

        disarmWakeup(state.session_id);
        if (m_services.scheduler) m_services.scheduler->cancelTimersForSession(state.session_id);

        const json pending = checkpoint.pending.is_object() ? checkpoint.pending : json::object();
        const std::string reason = pending.value("reason", "input");
        // An omitted answer (`{}`) is not an answer: it must not be written to the
        // prompt variable, or validation would judge an empty object.
        const bool has_input = !input.is_null() && !(input.is_object() && input.empty() && reason == "input");
        if (timed_out) {
            CapturedError error;
            error.code = "TIMEOUT";
            error.message = "no input received within the configured prompt timeout";
            error.node = pending.value("node", "");
            error.data = json{{"reason", "prompt_timeout"}, {"timeout_ms", pending.value("timeout_ms", 0)}};
            state.pending_error = error.toJson();
            // Resume at the node that was waiting, so its `on_error` handler (and any
            // enclosing try frame, which the checkpoint restored) gets the timeout.
            state.cursor = pending.value("node", checkpoint.cursor);
            if (state.cursor.empty() || workflow.find(state.cursor) == nullptr) state.cursor = checkpoint.cursor;
        } else if (has_input) {
            if (reason == "input") {
                const std::string variable = pending.value("input_variable", "");
                if (!variable.empty()) state.context->setByPath(variable, input);
                state.locals["input"] = input;
                state.resume_node_id = pending.value("node", "");
                state.cursor = state.resume_node_id;
                if (state.cursor.empty() || workflow.find(state.cursor) == nullptr) state.cursor = checkpoint.cursor;
            } else {
                state.cursor = checkpoint.cursor;
                if (state.cursor.empty() || workflow.find(state.cursor) == nullptr) {
                    state.cursor = pending.value("resume_node", "");
                }
                const std::string key = reason.empty() ? "event" : reason;
                state.locals[key] = input;
                if (key == "event") state.locals["event"] = input;
                // A timer/event wake-up can also carry facts the workflow is waiting
                // on (`{"ready": true}` from a webhook), which the suspended node must
                // see in its context when it re-runs.
                if (input.is_object()) {
                    for (const auto &item : input.items()) state.context->setByPath(item.key(), item.value());
                }
            }
        } else {
            state.cursor = checkpoint.cursor;
            if (state.cursor.empty() || workflow.find(state.cursor) == nullptr) {
                state.cursor = pending.value("resume_node", "");
            }
        }
        if (state.cursor.empty() || workflow.find(state.cursor) == nullptr) {
            state.status = "completed";
            state.finished = true;
            finishRun(workflow, state, options, report);
            return report;
        }

        // Error attribution (and `on_error` lookup) needs to know which node the
        // session is standing on, not only where the cursor points.
        if (state.current_node_id.empty()) state.current_node_id = state.cursor;
        if (m_services.metrics) m_services.metrics->increment("sapo.sessions.resumed");
        if (m_services.logger) {
            m_services.logger->debug("interpreter", "session " + state.session_id + " resumed at node '" +
                                                         state.cursor + "'" + (timed_out ? " (prompt timed out)" : ""));
        }
        drive(workflow, state);
        finishRun(workflow, state, options, report);
        return report;
    }

    ExecutionReport Interpreter::resumeSession(const std::string &session_id, const json &input) {
        ExecutionReport report;
        report.session_id = session_id;
        if (m_services.state_store == nullptr) {
            report.status = "failed";
            report.error_code = "SESSION_NOT_FOUND";
            report.error_message = "no state store is configured, so sessions cannot be resumed";
            return report;
        }
        const auto checkpoint = m_services.state_store->load(session_id);
        if (!checkpoint.has_value()) {
            report.status = "failed";
            report.error_code = "SESSION_NOT_FOUND";
            report.error_message = "no checkpoint stored for session '" + session_id + "'";
            return report;
        }
        if (checkpoint->status != SessionStatus::AwaitingInput && checkpoint->status != SessionStatus::Waiting) {
            report.status = "failed";
            report.error_code = "SESSION_NOT_SUSPENDED";
            report.error_message =
                "session '" + session_id + "' is " + toString(checkpoint->status) + ", not suspended";
            report.blueprint_id = checkpoint->blueprint_id;
            return report;
        }
        const parser::ParsedWorkflow *workflow =
            m_services.workflows != nullptr ? m_services.workflows->find(checkpoint->blueprint_id) : nullptr;
        if (workflow == nullptr) {
            report.status = "failed";
            report.error_code = "WORKFLOW_NOT_FOUND";
            report.error_message = "workflow '" + checkpoint->blueprint_id + "' needed to resume session '" +
                                   session_id + "' is not registered";
            return report;
        }
        return resume(*workflow, *checkpoint, input);
    }

    bool Interpreter::cancelSession(const std::string &session_id, const std::string &reason) {
        if (m_services.state_store == nullptr) return false;
        auto checkpoint = m_services.state_store->load(session_id);
        disarmWakeup(session_id);
        if (m_services.scheduler) m_services.scheduler->cancelTimersForSession(session_id);
        if (!checkpoint.has_value()) return false;
        // Only a live session can be cancelled; a finished one is left alone and a
        // second cancel is a no-op rather than a success.
        if (checkpoint->status != SessionStatus::Running && checkpoint->status != SessionStatus::AwaitingInput &&
            checkpoint->status != SessionStatus::Waiting) {
            return false;
        }
        checkpoint->status = SessionStatus::Cancelled;
        checkpoint->updated_ms = m_services.clock ? m_services.clock->now().count() : 0LL;
        checkpoint->pending = json{{"reason", "cancelled"}, {"detail", reason}};
        m_services.state_store->save(*checkpoint);
        if (m_services.metrics) m_services.metrics->increment("sapo.sessions.cancelled");
        return true;
    }

    // =========================================================================
    // checkpoints, wakeups and the service seams
    // =========================================================================
    void Interpreter::finishRun(const parser::ParsedWorkflow &workflow, RunState &state, const RunOptions &options,
                                ExecutionReport &report) {
        const int64_t now_ms = m_services.clock ? m_services.clock->now().count() : 0LL;
        report.elapsed_ms = std::max<int64_t>(0, now_ms - state.started_ms);
        report.blueprint_id = workflow.metadata.name;
        report.visited = state.visited;
        report.warnings = state.warnings;
        report.node_visits = state.visits;
        report.context = state.context ? state.context->snapshot() : json::object();
        report.output = report.context;

        if (state.status == "running") state.status = "completed";
        report.status = state.status;
        report.error_code = state.error_code;
        report.error_message = state.error;
        report.error_node = state.error_node;
        report.error_data = state.error_data;
        // An explicit `terminate.output` payload wins over the raw context.
        if (state.status == "terminated" && state.terminate_payload.is_object() && !state.terminate_payload.empty()) {
            report.output = state.terminate_payload;
        }
        if (state.status == "awaiting_input" || state.status == "suspended") {
            report.cursor = state.cursor;
            // Channels render `prompt`; the surrounding envelope (reason, resume
            // node, timers) belongs to the checkpoint rather than the reply.
            report.prompt = state.suspend_request.contains("prompt") ? state.suspend_request["prompt"]
                                                                     : state.suspend_request;
            report.output = json::object();
        }
        if (state.status == "failed" && report.error_message.empty()) {
            report.error_message = "execution failed";
        }

        if (m_services.metrics) {
            m_services.metrics->observe("sapo.session.node_visits", static_cast<double>(state.visits));
            m_services.metrics->observe("sapo.session.duration_ms", static_cast<double>(report.elapsed_ms));
            m_services.metrics->increment("sapo.sessions." + report.status);
        }
        if (m_services.logger) {
            const std::string line = "session " + state.session_id + " finished with status '" + report.status +
                                     "' after " + std::to_string(state.visits) + " node visits (" +
                                     std::to_string(report.elapsed_ms) + "ms)";
            if (report.failed()) m_services.logger->error("interpreter", line + ": " + report.error_message);
            else if (state.status == "suspended" || state.status == "awaiting_input") m_services.logger->info(
                "interpreter", line);
            else m_services.logger->debug("interpreter", line);
        }

        if (m_services.state_store == nullptr) return;
        const SessionCheckpoint checkpoint = checkpointFor(workflow, state, report);
        if (options.persist) m_services.state_store->save(checkpoint);
        if (state.status == "awaiting_input" || state.status == "suspended") armWakeup(state, checkpoint);
    }

    SessionCheckpoint Interpreter::checkpointFor(const parser::ParsedWorkflow &workflow, const RunState &state,
                                                  const ExecutionReport &report) const {
        SessionCheckpoint checkpoint;
        checkpoint.session_id = state.session_id;
        checkpoint.execution_id = state.execution_id;
        checkpoint.blueprint_id = workflow.metadata.name;
        checkpoint.correlation_id = report.correlation_id;
        checkpoint.depth = state.depth;
        checkpoint.node_visits = state.visits;
        checkpoint.created_ms = state.started_ms;
        checkpoint.updated_ms = state.started_ms + report.elapsed_ms;
        checkpoint.cursor = state.cursor;
        checkpoint.context = state.context ? state.context->snapshot() : json::object();
        checkpoint.frames = RuntimeContext::framesToJson(state.frames);
        if (state.status == "awaiting_input") checkpoint.status = SessionStatus::AwaitingInput;
        else if (state.status == "suspended") checkpoint.status = SessionStatus::Waiting;
        else if (state.status == "failed") checkpoint.status = SessionStatus::Failed;
        else if (state.status == "cancelled") checkpoint.status = SessionStatus::Cancelled;
        else checkpoint.status = SessionStatus::Completed;

        if (state.status == "awaiting_input" || state.status == "suspended") {
            checkpoint.pending = state.suspend_request;
            json pending = checkpoint.pending.is_object() ? checkpoint.pending : json::object();
            pending["resume_node"] = state.cursor;
            // Timers the wake-up path needs, persisted with the session so a
            // restart can re-arm them from the store.
            json timers = json::array();
            Timer timer;
            timer.session_id = state.session_id;
            timer.resume_node = state.cursor;
            timer.reason = pending.value("reason", "wait");
            if (pending.contains("resume_at_ms")) {
                timer.due_ms = pending["resume_at_ms"].get<int64_t>();
            } else if (pending.contains("timeout_ms")) {
                timer.due_ms = checkpoint.updated_ms + pending["timeout_ms"].get<int64_t>();
                if (timer.reason == "input") timer.reason = "prompt_timeout";
            }
            if (timer.due_ms > 0) {
                checkpoint.timers.push_back(timer);
                timers.push_back(timer.toJson());
            }
            pending["timers"] = timers;
            checkpoint.pending = std::move(pending);
        } else if (report.failed()) {
            checkpoint.error = "[" + report.error_code + "] " + report.error_message +
                               (report.error_node.empty() ? "" : " (node '" + report.error_node + "')");
            // A `terminate(status: failure, output: …)` payload is part of the contract
            // with the caller, so it survives; the raw context dump does not.
            checkpoint.result = state.terminate_payload.is_object() && !state.terminate_payload.empty()
                                    ? state.terminate_payload
                                    : json::object();
        } else {
            checkpoint.result = state.status == "terminated" ? state.terminate_payload : checkpoint.context;
        }
        return checkpoint;
    }

    void Interpreter::armWakeup(const RunState &state, const SessionCheckpoint &checkpoint) {
        const json pending = checkpoint.pending.is_object() ? checkpoint.pending : json::object();
        const std::string reason = pending.value("reason", "wait");
        if (m_services.scheduler != nullptr) {
            for (const auto &timer : checkpoint.timers) {
                Timer armed = timer;
                armed.session_id = state.session_id;
                if (armed.resume_node.empty()) armed.resume_node = checkpoint.cursor;
                if (armed.id.empty()) armed.id = state.session_id + "/" + reason;
                m_services.scheduler->addTimer(armed);
            }
        }
        if (m_services.events != nullptr && reason == "event" && pending.contains("event_name") &&
            pending["event_name"].is_string()) {
            const std::string event_name = pending["event_name"].get<std::string>();
            const std::string session_id = state.session_id;
            auto subscription = m_services.events->subscribe(
                event_name, [this, session_id](const Event &event) {
                    std::lock_guard<std::recursive_mutex> guard(m_mutex);
                    auto checkpoint_opt = m_services.state_store != nullptr ? m_services.state_store->load(session_id)
                                                                             : std::optional<SessionCheckpoint>{};
                    if (!checkpoint_opt.has_value()) return;
                    if (checkpoint_opt->status != SessionStatus::AwaitingInput &&
                        checkpoint_opt->status != SessionStatus::Waiting) {
                        return;
                    }
                    const parser::ParsedWorkflow *workflow = m_services.workflows != nullptr
                                                                  ? m_services.workflows->find(checkpoint_opt->blueprint_id)
                                                                  : nullptr;
                    if (workflow == nullptr) return;
                    resume(*workflow, *checkpoint_opt, event.payload);
                });
            std::lock_guard<std::recursive_mutex> guard(m_mutex);
            m_event_subscriptions[session_id].push_back(subscription);
        }
    }

    void Interpreter::disarmWakeup(const std::string &session_id) {
        std::vector<EventBus::SubscriptionId> subscriptions;
        {
            std::lock_guard<std::recursive_mutex> guard(m_mutex);
            auto it = m_event_subscriptions.find(session_id);
            if (it == m_event_subscriptions.end()) return;
            subscriptions = it->second;
            m_event_subscriptions.erase(it);
        }
        if (m_services.events != nullptr) {
            for (const auto id : subscriptions) m_services.events->unsubscribe(id);
        }
    }

    void Interpreter::installRunners() {
        if (m_runners_installed) return;
        m_runners_installed = true;
        if (m_services.limits.max_depth > 0 && m_services.workflows != nullptr) {
            m_services.workflows->setMaxDepth(m_services.limits.max_depth);
        }

        if (m_services.workflows != nullptr) {
            m_services.workflows->setRunner([this](const ChildRunRequest &request) {
                ChildRunResult result;
                const parser::ParsedWorkflow *workflow = m_services.workflows->find(request.workflow_id);
                if (workflow == nullptr) {
                    throw SapoError(ErrorCode::NotFound,
                                    "subflow references workflow '" + request.workflow_id + "', which is not registered",
                                    json{{"available", m_services.workflows->ids()}}, request.node_id);
                }
                RunOptions options;
                options.input = request.input;
                options.depth = request.depth;
                options.correlation_id = request.correlation_id;
                options.persist = false;   // children are folded into the parent's report
                if (!request.wait_for_completion && m_services.pool != nullptr) {
                    std::ignore = m_services.pool->submit([this, workflow, request, options] {
                        try {
                            run(*workflow, request.input, options);
                        } catch (const std::exception &e) {
                            if (m_services.logger) {
                                m_services.logger->error("interpreter",
                                                          "detached subflow '" + request.workflow_id + "' failed: " + e.what());
                            }
                        }
                    });
                    result.status = "launched";
                    return result;
                }
                const ExecutionReport report = run(*workflow, request.input, options);
                result.session_id = report.session_id;
                result.status = report.status;
                result.output = report.output;
                if (report.failed()) {
                    result.error = report.error_message.empty() ? report.error_code : report.error_message;
                }
                return result;
            });
        }

        if (m_services.scheduler != nullptr) {
            m_services.scheduler->onJobFired([this](const ScheduledJob &job, int64_t fired_at_ms) {
                startScheduledJob(job, fired_at_ms);
            });
            m_services.scheduler->onTimerDue([this](const Timer &timer) { resumeFromTimer(timer); });
        }

        if (m_services.workflows != nullptr && m_services.events != nullptr) {
            // A single wildcard subscription (instead of one per blueprint) means a
            // workflow registered after start() reacts to events too.
            const auto subscription = m_services.events->subscribe(
                "*", [this](const Event &event) {
                    if (m_services.workflows == nullptr) return;
                    for (const auto &workflow_id : m_services.workflows->workflowsTriggeredBy(event.name)) {
                        startTriggeredWorkflow(workflow_id, event);
                    }
                });
            std::lock_guard<std::recursive_mutex> guard(m_mutex);
            m_trigger_subscription = subscription;
        }
    }

    void Interpreter::startScheduledJob(const ScheduledJob &job, int64_t fired_at_ms) {
        if (m_services.workflows == nullptr) return;
        const parser::ParsedWorkflow *workflow = m_services.workflows->find(job.workflow);
        if (workflow == nullptr) {
            if (m_services.logger) {
                m_services.logger->warn("interpreter", "scheduled job '" + job.id + "' references unknown workflow '" +
                                                           job.workflow + "'");
            }
            return;
        }
        RunOptions options;
        options.input = job.input;
        options.correlation_id = "job:" + job.id;
        options.max_node_visits = m_services.limits.max_node_visits;
        if (!job.entry_node.empty() && workflow->find(job.entry_node) != nullptr) options.start_node = job.entry_node;
        (void) fired_at_ms;
        try {
            run(*workflow, job.input, options);
        } catch (const std::exception &e) {
            if (m_services.logger) {
                m_services.logger->error("interpreter", "scheduled workflow '" + job.workflow + "' failed: " + e.what());
            }
        }
    }

    void Interpreter::startTriggeredWorkflow(const std::string &workflow_id, const Event &event) {
        if (m_services.workflows == nullptr) return;
        const parser::ParsedWorkflow *workflow = m_services.workflows->find(workflow_id);
        if (workflow == nullptr) return;
        RunOptions options;
        options.input = event.payload.is_null() ? json::object() : event.payload;
        options.correlation_id = "event:" + event.name;
        try {
            run(*workflow, options.input, options);
        } catch (const std::exception &e) {
            if (m_services.logger) {
                m_services.logger->error("interpreter", "event-triggered workflow '" + workflow_id + "' failed: " + e.what());
            }
        }
    }

    void Interpreter::resumeFromTimer(const Timer &timer) {
        if (m_services.state_store == nullptr || timer.session_id.empty()) return;
        const auto checkpoint = m_services.state_store->load(timer.session_id);
        if (!checkpoint.has_value()) return;
        if (checkpoint->status != SessionStatus::AwaitingInput && checkpoint->status != SessionStatus::Waiting) return;
        const parser::ParsedWorkflow *workflow =
            m_services.workflows != nullptr ? m_services.workflows->find(checkpoint->blueprint_id) : nullptr;
        if (workflow == nullptr) {
            if (m_services.logger) {
                m_services.logger->warn("interpreter", "timer for session " + timer.session_id +
                                                            " fired but workflow '" + checkpoint->blueprint_id +
                                                            "' is not registered");
            }
            return;
        }
        const bool timed_out = timer.reason == "prompt_timeout" || timer.reason == "input_timeout";
        try {
            resume(*workflow, *checkpoint, json(), timed_out);
        } catch (const std::exception &e) {
            if (m_services.logger) {
                m_services.logger->error("interpreter", "resuming session " + timer.session_id + " failed: " + e.what());
            }
        }
    }

    ExecutionReport Interpreter::runFile(const std::string &path, const json &input, RunOptions options) {
        std::ifstream stream(path);
        if (!stream) {
            ExecutionReport report;
            report.status = "failed";
            report.error_code = "FILE_NOT_FOUND";
            report.error_message = "cannot read workflow file '" + path + "'";
            return report;
        }
        std::stringstream buffer;
        buffer << stream.rdbuf();
        if (m_services.workflows != nullptr) {
            try {
                m_services.workflows->loadFile(path);
            } catch (const std::exception &) {
                // already-registered blueprints are fine; the parse below reports problems
            }
        }
        parser::ParseOptions parse_options;
        parse_options.capabilities = m_services.capabilities.get();
        parser::ParsedWorkflow workflow;
        try {
            workflow = parser::WorkflowParser::parseWorkflow(buffer.str(), parse_options);
        } catch (const SapoError &error) {
            ExecutionReport report;
            report.status = "failed";
            report.error_code = error.codeString();
            report.error_message = error.message();
            report.error_node = error.nodeId();
            return report;
        } catch (const std::exception &e) {
            ExecutionReport report;
            report.status = "failed";
            report.error_code = "PARSE_ERROR";
            report.error_message = e.what();
            return report;
        }
        if (m_services.workflows != nullptr) m_services.workflows->add(workflow, workflow.metadata.name, path);
        if (options.session_id.empty()) options.session_id = options.session_id;
        return run(workflow, input, options);
    }

} // namespace sapo::runtime
