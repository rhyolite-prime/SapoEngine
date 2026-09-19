//
//  Sapo Engine — VirtualMachine facade.
//
#include "runtime/VirtualMachine.hpp"

#include "config/ProviderConfig.hpp"
#include "runtime/SapoError.hpp"
#include "util/Crypto.hpp"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <utility>

using json = nlohmann::json;

namespace sapo::runtime {

    namespace {

        obs::LogLevel levelFromName(const std::string &name, obs::LogLevel fallback) {
            if (name == "trace") return obs::LogLevel::Trace;
            if (name == "debug") return obs::LogLevel::Debug;
            if (name == "info") return obs::LogLevel::Info;
            if (name == "warn" || name == "warning") return obs::LogLevel::Warn;
            if (name == "error") return obs::LogLevel::Error;
            if (name == "off" || name == "none") return obs::LogLevel::Off;
            return fallback;
        }

    } // namespace

    // =========================================================================
    json VirtualMachine::capabilitiesSummary() const {
        if (m_services.capabilities == nullptr) return json::object();
        json deferred = json::array();
        for (const auto &descriptor : m_services.capabilities->all()) {
            if (descriptor.deferred) deferred.push_back(descriptor.name);
        }
        return json{{"providers", m_services.capabilities->providerIds()},
                    {"capabilities", m_services.capabilities->all().size()},
                    {"deferred", deferred}};
    }

    json VirtualMachine::dataSourceSummary() const {
        if (m_services.data_sources == nullptr) return json::array();
        return json{{"providers", m_services.data_sources->providerIds()},
                    {"deferred", m_services.data_sources->deferredProviders()}};
    }

    // =========================================================================
    ExecutionOutcome ExecutionOutcome::from(const ExecutionReport &report) {
        ExecutionOutcome outcome;
        outcome.session_id = report.session_id;
        outcome.execution_id = report.execution_id;
        outcome.workflow_id = report.blueprint_id;
        outcome.status = report.status;
        outcome.ok = report.succeeded();
        outcome.output = report.output;
        outcome.context = report.context;
        outcome.prompt = report.prompt;
        outcome.error = report.error_message;
        outcome.error_code = report.error_code;
        outcome.error_node = report.error_node;
        outcome.error_data = report.error_data;
        outcome.cursor = report.cursor;
        outcome.warnings = report.warnings;
        outcome.node_visits = report.node_visits;
        outcome.elapsed_ms = report.elapsed_ms;
        return outcome;
    }

    json ExecutionOutcome::toJson() const {
        json out{{"session_id", session_id},
                 {"execution_id", execution_id},
                 {"workflow_id", workflow_id},
                 {"status", status},
                 {"ok", ok},
                 {"node_visits", node_visits},
                 {"elapsed_ms", elapsed_ms}};
        if (!output.is_null() && !output.empty()) out["output"] = output;
        if (!context.is_null() && !context.empty()) out["context"] = context;
        if (!prompt.is_null() && !prompt.empty()) out["prompt"] = prompt;
        if (!cursor.empty()) out["cursor"] = cursor;
        if (!error.empty()) {
            out["error"] = json{{"code", error_code}, {"message", error}};
            if (!error_node.empty()) out["error"]["node"] = error_node;
            if (!error_data.is_null() && !error_data.empty()) out["error"]["data"] = error_data;
        }
        if (!warnings.empty()) out["warnings"] = warnings;
        return out;
    }

    SessionSnapshot SessionSnapshot::from(const SessionCheckpoint &checkpoint) {
        SessionSnapshot snapshot;
        snapshot.session_id = checkpoint.session_id;
        snapshot.execution_id = checkpoint.execution_id;
        snapshot.blueprint_id = checkpoint.blueprint_id;
        snapshot.status = toString(checkpoint.status);
        snapshot.cursor = checkpoint.cursor;
        snapshot.pending = checkpoint.pending;
        snapshot.context = checkpoint.context;
        snapshot.output = checkpoint.result;
        snapshot.created_ms = checkpoint.created_ms;
        snapshot.updated_ms = checkpoint.updated_ms;
        snapshot.node_visits = checkpoint.node_visits;
        snapshot.error = checkpoint.error;
        snapshot.resumable =
            checkpoint.status == SessionStatus::AwaitingInput || checkpoint.status == SessionStatus::Waiting;
        return snapshot;
    }

    json SessionSnapshot::toJson() const {
        json out{{"session_id", session_id},
                 {"execution_id", execution_id},
                 {"workflow_id", blueprint_id},
                 {"status", status},
                 {"node_visits", node_visits},
                 {"created_at_ms", created_ms},
                 {"updated_at_ms", updated_ms},
                 {"resumable", resumable}};
        if (!cursor.empty()) out["cursor"] = cursor;
        if (!pending.is_null() && !pending.empty()) out["pending"] = pending;
        if (!output.is_null() && !output.empty()) out["output"] = output;
        if (!context.is_null() && !context.empty()) out["context"] = context;
        if (!error.empty()) out["error"] = error;
        return out;
    }

    // =========================================================================
    VirtualMachine::VirtualMachine(TaskServices services) : m_services(std::move(services)) {
        if (m_services.workflows == nullptr) {
            m_services.workflows = std::make_shared<WorkflowRegistry>();
        }
        m_interpreter = std::make_unique<Interpreter>(m_services, &tasks::TaskRegistry::defaults());
    }

    VirtualMachine::~VirtualMachine() {
        try {
            stop();
        } catch (...) {
        }
    }

    VirtualMachine::VirtualMachine(VirtualMachine &&) noexcept = default;
    VirtualMachine &VirtualMachine::operator=(VirtualMachine &&) noexcept = default;

    std::vector<std::string> VirtualMachine::start() {
        m_problems.clear();

        // 1. provider configuration (T3.2)
        std::optional<config::ProviderConfigStore> loaded_config;
        try {
            if (!m_config_path.empty()) {
                loaded_config = config::ProviderConfigStore::load(m_config_path);
            } else if (std::filesystem::exists("sapo-config.json")) {
                loaded_config = config::ProviderConfigStore::load("sapo-config.json");
                m_config_path = "sapo-config.json";
            }
        } catch (const SapoError &error) {
            m_problems.push_back("config: " + error.message());
        } catch (const std::exception &e) {
            m_problems.push_back(std::string("config: ") + e.what());
        }

        if (loaded_config.has_value()) {
            m_services.provider_config = std::make_shared<config::ProviderConfigStore>(*loaded_config);
            const auto config_problems = m_services.provider_config->validate();
            m_problems.insert(m_problems.end(), config_problems.begin(), config_problems.end());

            // Engine tunables come from the config so a deployment can size the
            // pool without recompiling anything.
            const json engine = m_services.provider_config->engine();
            if (engine.is_object()) {
                if (engine.contains("log_level") && engine["log_level"].is_string() && m_services.logger) {
                    m_services.logger->setLevel(
                        levelFromName(engine["log_level"].get<std::string>(), m_services.logger->level()));
                }
                if (engine.contains("max_node_visits") && engine["max_node_visits"].is_number_integer()) {
                    m_services.limits.max_node_visits = std::max<size_t>(1, engine["max_node_visits"].get<size_t>());
                }
                if (engine.contains("max_depth") && engine["max_depth"].is_number_integer()) {
                    m_services.limits.max_depth = std::max<size_t>(1, engine["max_depth"].get<size_t>());
                }
                if (engine.contains("max_branch_visits") && engine["max_branch_visits"].is_number_integer()) {
                    m_services.limits.max_branch_visits =
                        std::max<size_t>(1, engine["max_branch_visits"].get<size_t>());
                }
                if (engine.contains("inline_wait_limit_ms") && engine["inline_wait_limit_ms"].is_number_integer()) {
                    m_services.limits.inline_wait_limit_ms = engine["inline_wait_limit_ms"].get<int64_t>();
                }
                if (engine.contains("default_timeout_ms") && engine["default_timeout_ms"].is_number_integer()) {
                    const auto timeout = engine["default_timeout_ms"].get<int64_t>();
                    if (timeout > 0) m_services.limits.default_timeout_ms = timeout;
                }
                if (engine.contains("state_dir") && engine["state_dir"].is_string()) {
                    const auto directory = engine["state_dir"].get<std::string>();
                    const auto file_store = std::dynamic_pointer_cast<FileStateStore>(m_services.state_store);
                    const bool wants_disk = !directory.empty() &&
                                            (m_services.state_store == nullptr || file_store != nullptr ||
                                             file_store == nullptr);
                    if (wants_disk && m_services.state_store != nullptr) {
                        m_services.state_store = std::make_shared<FileStateStore>(directory);
                    }
                }
            }
            m_services.applySecretRedaction();
        }

        // 2. blueprints
        if (!m_workflow_directory.empty()) addBlueprintDirectory(m_workflow_directory);

        // 3. wire the interpreter into the services (subflows, events, timers)
        m_interpreter = std::make_unique<Interpreter>(m_services, &tasks::TaskRegistry::defaults());
        m_interpreter->installRunners();

        // 4. startup audit: capabilities, providers, dangling subflow references
        const auto audit = m_services.startupCheck();
        m_problems.insert(m_problems.end(), audit.begin(), audit.end());

        if (m_services.workflows != nullptr) {
            for (const auto &id : m_services.workflows->ids()) {
                const auto *entry = m_services.workflows->entry(id);
                if (entry == nullptr) continue;
                for (const auto &warning : entry->workflow.warnings) {
                    m_problems.push_back("workflow '" + id + "': " + warning);
                }
            }
        }

        m_started = true;
        if (m_services.logger) {
            m_services.logger->info("vm", "engine ready: " + std::to_string(m_services.workflows->size()) +
                                              " workflow(s), " + std::to_string(m_problems.size()) + " problem(s)");
        }
        if (m_services.metrics) {
            m_services.metrics->setGauge("sapo.workflows.registered",
                                        static_cast<double>(m_services.workflows->size()));
        }
        return m_problems;
    }

    void VirtualMachine::stop() {
        if (m_services.scheduler != nullptr) m_services.scheduler->stop();
        if (m_services.pool != nullptr) m_services.pool->shutdown();
        m_started = false;
    }

    void VirtualMachine::ensureStarted() {
        if (!m_started) start();
    }

    std::string VirtualMachine::addBlueprintText(const std::string &text, const std::string &origin) {
        ensureStarted();
        return m_services.workflows->loadJsonText(text, {}, origin);
    }

    std::string VirtualMachine::addBlueprintFile(const std::string &path) {
        ensureStarted();
        return m_services.workflows->loadFile(path);
    }

    size_t VirtualMachine::addBlueprintDirectory(const std::string &directory) {
        ensureStarted();
        std::vector<std::string> problems;
        const size_t loaded = m_services.workflows->loadDirectory(directory, problems);
        for (auto &problem : problems) m_problems.push_back("workflow: " + problem);
        return loaded;
    }

    std::vector<std::string> VirtualMachine::validateAll() const {
        std::vector<std::string> problems = m_problems;
        if (m_services.workflows != nullptr) {
            for (const auto &dangling : m_services.workflows->unresolvedReferences()) {
                problems.push_back("dangling subflow reference: '" + dangling + "' is not registered");
            }
        }
        return problems;
    }

    ExecutionOutcome VirtualMachine::startSession(const std::string &workflow_id, const json &input,
                                                  StartSessionOptions options) {
        ensureStarted();
        const parser::ParsedWorkflow *workflow = m_services.workflows->find(workflow_id);
        if (workflow == nullptr) {
            ExecutionOutcome outcome;
            outcome.status = "failed";
            outcome.ok = false;
            outcome.workflow_id = workflow_id;
            outcome.error_code = "WORKFLOW_NOT_FOUND";
            json available = json::array();
            if (m_services.workflows != nullptr) {
                for (const auto &id : m_services.workflows->ids()) available.push_back(id);
            }
            outcome.error = "no workflow named '" + workflow_id + "' is registered" +
                            (available.empty() ? std::string(" (the registry is empty)")
                                               : "; available: " + available.dump());
            return outcome;
        }
        RunOptions run_options;
        run_options.session_id = options.session_id;
        run_options.correlation_id = options.correlation_id;
        run_options.start_node = options.start_node;
        run_options.input = input;
        run_options.persist = options.persist;
        run_options.max_node_visits = options.max_node_visits;
        return ExecutionOutcome::from(m_interpreter->run(*workflow, input, run_options));
    }

    ExecutionOutcome VirtualMachine::runBlueprint(const json &blueprint, const json &input,
                                                  StartSessionOptions options) {
        ensureStarted();
        parser::ParseOptions parse_options;
        if (m_services.capabilities != nullptr) parse_options.capabilities = m_services.capabilities.get();
        try {
            auto workflow = parser::WorkflowParser::parseWorkflow(blueprint.dump(), parse_options);
            // Registering under the blueprint's own name is what makes examples
            // copy-paste into a real deployment; a collision gets a private id so
            // a test run never rewrites a deployed workflow.
            std::string id = workflow.metadata.name;
            if (m_services.workflows->find(id) != nullptr) {
                id = "inline/" + util::uuidV4();
                m_services.workflows->add(std::move(workflow), id, "<inline>");
            } else {
                m_services.workflows->add(std::move(workflow));
            }
            return startSession(id, input, options);
        } catch (const SapoError &error) {
            ExecutionOutcome outcome;
            outcome.status = "failed";
            outcome.ok = false;
            outcome.error_code = error.codeString();
            outcome.error = error.message();
            outcome.error_node = error.nodeId();
            return outcome;
        } catch (const std::exception &e) {
            ExecutionOutcome outcome;
            outcome.status = "failed";
            outcome.ok = false;
            outcome.error_code = "PARSE_ERROR";
            outcome.error = e.what();
            return outcome;
        }
    }

    ExecutionOutcome VirtualMachine::resumeSession(const std::string &session_id, const json &input) {
        ensureStarted();
        return ExecutionOutcome::from(m_interpreter->resumeSession(session_id, input));
    }

    bool VirtualMachine::cancelSession(const std::string &session_id, const std::string &reason) {
        ensureStarted();
        return m_interpreter->cancelSession(session_id, reason);
    }

    std::optional<SessionSnapshot> VirtualMachine::session(const std::string &session_id) const {
        if (m_services.state_store == nullptr) return std::nullopt;
        const auto checkpoint = m_services.state_store->load(session_id);
        if (!checkpoint.has_value()) return std::nullopt;
        return SessionSnapshot::from(*checkpoint);
    }

    std::vector<SessionSnapshot> VirtualMachine::sessions() const {
        std::vector<SessionSnapshot> out;
        if (m_services.state_store == nullptr) return out;
        for (const auto &checkpoint : m_services.state_store->list()) out.push_back(SessionSnapshot::from(checkpoint));
        std::sort(out.begin(), out.end(), [](const SessionSnapshot &a, const SessionSnapshot &b) {
            return a.updated_ms > b.updated_ms;
        });
        return out;
    }

    size_t VirtualMachine::suspendedSessionCount() const {
        size_t count = 0;
        for (const auto &snapshot : sessions()) {
            if (snapshot.resumable) ++count;
        }
        return count;
    }

    void VirtualMachine::publishEvent(const std::string &name, const json &payload) {
        if (m_services.events == nullptr) return;
        m_services.events->publish(name, payload);
    }

    void VirtualMachine::publishEvent(Event event) {
        if (m_services.events == nullptr) return;
        m_services.events->publish(std::move(event));
    }

    size_t VirtualMachine::tick() {
        if (m_services.scheduler == nullptr) return 0;
        return m_services.scheduler->tick();
    }

    size_t VirtualMachine::tickUntil(int64_t now_ms) {
        if (m_services.scheduler == nullptr) return 0;
        return m_services.scheduler->tickUntil(now_ms);
    }

    void VirtualMachine::startBackgroundTick(std::chrono::milliseconds interval) {
        if (m_services.scheduler == nullptr) return;
        m_services.scheduler->start(interval);
    }

    json VirtualMachine::metrics() const {
        json out = m_services.metrics != nullptr ? m_services.metrics->toJson() : json::object();
        const auto all = sessions();
        size_t suspended = 0;
        for (const auto &snapshot : all) {
            if (snapshot.resumable) ++suspended;
        }
        out["sessions"] = json{{"total", all.size()}, {"suspended", suspended}};
        if (m_services.pool != nullptr) {
            out["workers"] = json{{"threads", m_services.pool->size()},
                                  {"queued", m_services.pool->queued()},
                                  {"submitted", m_services.pool->submitted()}};
        }
        if (m_services.scheduler != nullptr) {
            out["scheduler"] = json{{"jobs", m_services.scheduler->jobs().size()},
                                    {"timers", m_services.scheduler->timers().size()}};
        }
        if (m_services.events != nullptr) {
            json recent = json::array();
            for (const auto &event : m_services.events->recent(5)) recent.push_back(event.toJson());
            out["events"] = json{{"subscribers", m_services.events->subscriberCount()},
                                 {"patterns", m_services.events->patterns()},
                                 {"recent", std::move(recent)}};
        }
        return out;
    }

    json VirtualMachine::traceFor(const std::string &execution_id) const {
        json out = json::array();
        if (m_services.traces == nullptr) return out;
        for (const auto &span : m_services.traces->spansFor(execution_id)) {
            out.push_back(json{{"node_id", span.node_id},
                               {"node_type", span.node_type},
                               {"outcome", span.outcome},
                               {"duration_ms", span.duration_ms},
                               {"depth", span.depth},
                               {"error_code", span.error_code},
                               {"error_message", span.error_message}});
        }
        return out;
    }

    json VirtualMachine::describe() const {
        json workflows_json = json::array();
        if (m_services.workflows != nullptr) {
            for (const auto &id : m_services.workflows->ids()) {
                const auto *entry = m_services.workflows->entry(id);
                json item{{"id", id}, {"nodes", entry != nullptr ? entry->workflow.nodes.size() : 0}};
                if (entry != nullptr) {
                    item["name"] = entry->workflow.metadata.name;
                    item["version"] = entry->workflow.metadata.version;
                    if (!entry->origin.empty()) item["origin"] = entry->origin;
                    if (entry->workflow.metadata.trigger_event.has_value()) {
                        item["trigger_event"] = *entry->workflow.metadata.trigger_event;
                    }
                }
                workflows_json.push_back(std::move(item));
            }
        }
        json problems = json::array();
        for (const auto &problem : validateAll()) problems.push_back(problem);
        return json{{"engine", "sapo"},
                    {"ready", m_started},
                    {"blueprints", std::move(workflows_json)},
                    {"capabilities", capabilitiesSummary()},
                    {"data_sources", dataSourceSummary()},
                    {"limits", json{{"max_node_visits", m_services.limits.max_node_visits},
                                    {"max_depth", m_services.limits.max_depth},
                                    {"max_branch_visits", m_services.limits.max_branch_visits},
                                    {"inline_wait_limit_ms", m_services.limits.inline_wait_limit_ms}}},
                    {"problems", std::move(problems)}};
    }

} // namespace sapo::runtime
