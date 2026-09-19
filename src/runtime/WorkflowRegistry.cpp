//
//  Sapo Engine — workflow registry implementation.
//
#include "runtime/WorkflowRegistry.hpp"
#include "observability/Logger.hpp"
#include "runtime/SapoError.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

namespace sapo::runtime {

    WorkflowRegistry::WorkflowRegistry(parser::ParseOptions options, ClockPtr clock)
        : m_options(std::move(options)), m_clock(clock ? std::move(clock) : defaultClock()) {}

    std::string WorkflowRegistry::add(parser::ParsedWorkflow workflow, std::string id, std::string origin) {
        if (id.empty()) id = workflow.metadata.name;
        if (id.empty()) id = "workflow_" + std::to_string(m_entries.size() + 1);
        // A registered workflow must be resolvable by id inside its own graph, so
        // metadata carries it (subflows report the same name they are called by).
        workflow.metadata.name = id;
        WorkflowEntry entry;
        entry.id = id;
        entry.origin = origin.empty() ? std::string("<inline>") : std::move(origin);
        entry.loaded_ms = m_clock->now().count();
        entry.workflow = std::move(workflow);

        const bool replacing = m_entries.count(id) != 0;
        m_entries[id] = std::move(entry);
        if (replacing) {
            obs::defaultLogger()->info("registry", "workflow '" + id + "' was reloaded (definition replaced)");
        }
        return id;
    }

    std::string WorkflowRegistry::loadJsonText(const std::string &text, std::string id, std::string origin) {
        auto workflow = parser::WorkflowParser::parseWorkflow(text, m_options);
        return add(std::move(workflow), std::move(id), std::move(origin));
    }

    std::string WorkflowRegistry::loadFile(const std::string &path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw runtime::SapoError(runtime::ErrorCode::Parse, "cannot read blueprint '" + path + "'");
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        std::string id = std::filesystem::path(path).stem().string();
        return loadJsonText(buffer.str(), std::move(id), path);
    }

    size_t WorkflowRegistry::loadDirectory(const std::string &directory, std::vector<std::string> &problems) {
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error)) {
            problems.push_back("'" + directory + "' is not a directory");
            return 0;
        }
        std::vector<std::string> files;
        for (const auto &item : std::filesystem::directory_iterator(directory, error)) {
            if (!item.is_regular_file()) continue;
            if (item.path().extension() != ".json") continue;
            files.push_back(item.path().string());
        }
        std::sort(files.begin(), files.end());
        size_t loaded = 0;
        for (const auto &file : files) {
            try {
                loadFile(file);
                ++loaded;
            } catch (const std::exception &e) {
                problems.push_back(file + ": " + e.what());
            }
        }
        return loaded;
    }

    bool WorkflowRegistry::remove(const std::string &id) { return m_entries.erase(id) != 0; }

    void WorkflowRegistry::clear() { m_entries.clear(); }

    const parser::ParsedWorkflow *WorkflowRegistry::find(const std::string &id) const {
        auto it = m_entries.find(id);
        return it == m_entries.end() ? nullptr : &it->second.workflow;
    }

    const WorkflowEntry *WorkflowRegistry::entry(const std::string &id) const {
        auto it = m_entries.find(id);
        return it == m_entries.end() ? nullptr : &it->second;
    }

    std::vector<std::string> WorkflowRegistry::ids() const {
        std::vector<std::string> out;
        out.reserve(m_entries.size());
        for (const auto &[id, entry] : m_entries) out.push_back(id);
        return out;
    }

    std::vector<std::string> WorkflowRegistry::workflowsTriggeredBy(const std::string &event) const {
        std::vector<std::string> out;
        for (const auto &[id, entry] : m_entries) {
            const auto &trigger = entry.workflow.metadata.trigger_event;
            if (!trigger.has_value() || trigger->empty()) continue;
            if (*trigger == event) {
                out.push_back(id);
                continue;
            }
            if (!trigger->empty() && trigger->back() == '*' && event.rfind(trigger->substr(0, trigger->size() - 1), 0) == 0) {
                out.push_back(id);
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    std::vector<EventBus::SubscriptionId> WorkflowRegistry::installEventTriggers(
        EventBus &bus, const std::function<void(const std::string &, const Event &)> &start) {
        std::vector<EventBus::SubscriptionId> subscriptions;
        for (const auto &[id, entry] : m_entries) {
            const auto &trigger = entry.workflow.metadata.trigger_event;
            if (!trigger.has_value() || trigger->empty()) continue;
            const std::string workflow_id = id;
            subscriptions.push_back(bus.subscribe(*trigger, [start, workflow_id](const Event &event) {
                if (start) start(workflow_id, event);
            }));
        }
        return subscriptions;
    }

    ChildRunResult WorkflowRegistry::runChild(const ChildRunRequest &request) const {
        ChildRunResult result;
        if (request.workflow_id.empty()) {
            throw runtime::SapoError(runtime::ErrorCode::Validation, "subflow node has no 'workflow' id", json::object(),
                                    request.node_id);
        }
        if (request.depth >= m_max_depth) {
            throw runtime::SapoError(runtime::ErrorCode::Limit,
                                     "subflow recursion limit reached (" + std::to_string(m_max_depth) +
                                         " levels) while starting '" + request.workflow_id + "'",
                                     json{{"workflow", request.workflow_id}, {"depth", request.depth}}, request.node_id);
        }
        const WorkflowEntry *target = entry(request.workflow_id);
        if (target == nullptr) {
            std::string available;
            for (const auto &id : ids()) {
                if (!available.empty()) available += ", ";
                available += id;
            }
            throw runtime::SapoError(runtime::ErrorCode::NotFound,
                                     "subflow references workflow '" + request.workflow_id + "' which is not registered" +
                                         (available.empty() ? std::string(" (no workflows are registered)")
                                                            : "; registered: " + available),
                                     json{{"workflow", request.workflow_id}}, request.node_id);
        }
        if (!m_runner) {
            throw runtime::SapoError(runtime::ErrorCode::Internal,
                                     "no workflow runner is installed on the registry (subflows need a VM)",
                                     json::object(), request.node_id);
        }
        return m_runner(request);
    }

    std::vector<std::string> WorkflowRegistry::unresolvedReferences() const {
        std::vector<std::string> problems;
        for (const auto &[id, entry] : m_entries) {
            for (const auto &node : entry.workflow.nodes) {
                if (node->getType() != parser::TaskType::Subflow) continue;
                const auto &subflow = static_cast<const parser::SubflowNode &>(*node);
                if (m_entries.count(subflow.workflow) == 0) {
                    problems.push_back("workflow '" + id + "' node '" + node->id + "' calls unregistered workflow '" +
                                       subflow.workflow + "'");
                }
            }
            for (const auto &node : entry.workflow.nodes) {
                if (node->getType() != parser::TaskType::Schedule) continue;
                const auto &schedule = static_cast<const parser::ScheduleNode &>(*node);
                if (schedule.workflow.has_value() && m_entries.count(*schedule.workflow) == 0) {
                    problems.push_back("workflow '" + id + "' node '" + node->id + "' schedules unregistered workflow '" +
                                       *schedule.workflow + "'");
                }
            }
        }
        std::sort(problems.begin(), problems.end());
        problems.erase(std::unique(problems.begin(), problems.end()), problems.end());
        return problems;
    }

} // namespace sapo::runtime
