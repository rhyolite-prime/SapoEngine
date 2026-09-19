//
//  Sapo Engine — durable session state (implementation_plan_2.md T4.1).
//
//  A suspended session is *data*, not a parked thread: the interpreter writes a
//  `SessionCheckpoint` (context, cursor, frame stack, pending timers, awaiting
//  prompt) to an `IStateStore`, and `resumeSession` rebuilds execution from it.
//  That is what makes 10 000 suspended sessions cost memory instead of 10 000
//  threads, and what lets a process restart mid-conversation.
//
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/Scheduler.hpp"

namespace sapo::runtime {

    enum class SessionStatus {
        Running,
        AwaitingInput,   // suspended on a prompt / `await_input` action
        Waiting,         // suspended on a timer (`wait`) or an awaited event
        Completed,
        Failed,
        Cancelled
    };

    [[nodiscard]] const char *toString(SessionStatus status);
    [[nodiscard]] std::optional<SessionStatus> sessionStatusFromString(const std::string &text);

    /// Everything needed to resume a session, and nothing more.
    struct SessionCheckpoint {
        std::string session_id;
        std::string execution_id;
        std::string blueprint_id;
        std::string blueprint_version;
        std::string correlation_id;

        SessionStatus status{SessionStatus::Running};
        std::string cursor;                  // node id to execute next
        nlohmann::json context = nlohmann::json::object();  // RuntimeContext payload
        nlohmann::json frames = nlohmann::json::array();    // interpreter frame stack
        nlohmann::json pending = nlohmann::json::object();  // suspend request (prompt/timeout/event)
        std::vector<Timer> timers;
        nlohmann::json result = nlohmann::json::object();   // terminate payload once finished
        std::string error;                    // formatted failure, when status == Failed

        std::string parent_session;
        size_t depth{0};
        std::vector<std::string> child_sessions;

        int64_t created_ms{0};
        int64_t updated_ms{0};
        size_t node_visits{0};                // execution budget / loop guard

        [[nodiscard]] nlohmann::json toJson() const;
        [[nodiscard]] static SessionCheckpoint fromJson(const nlohmann::json &json);
    };

    class IStateStore {
    public:
        virtual ~IStateStore() = default;

        virtual void save(const SessionCheckpoint &checkpoint) = 0;
        [[nodiscard]] virtual std::optional<SessionCheckpoint> load(const std::string &session_id) const = 0;
        virtual bool remove(const std::string &session_id) = 0;
        [[nodiscard]] virtual std::vector<SessionCheckpoint> list() const = 0;
        [[nodiscard]] virtual size_t count(std::optional<SessionStatus> status = std::nullopt) const = 0;
        [[nodiscard]] virtual std::string kind() const = 0;
    };

    using StateStorePtr = std::shared_ptr<IStateStore>;

    /// Process-lifetime store (tests, single-run CLI).
    class InMemoryStateStore final : public IStateStore {
    public:
        void save(const SessionCheckpoint &checkpoint) override;
        [[nodiscard]] std::optional<SessionCheckpoint> load(const std::string &session_id) const override;
        bool remove(const std::string &session_id) override;
        [[nodiscard]] std::vector<SessionCheckpoint> list() const override;
        [[nodiscard]] size_t count(std::optional<SessionStatus> status) const override;
        [[nodiscard]] std::string kind() const override { return "memory"; }

    private:
        mutable std::mutex m_mutex;
        std::unordered_map<std::string, SessionCheckpoint> m_sessions;
    };

    /// One JSON file per session (`<dir>/<id>.json`), written atomically. Good
    /// enough for a single-node deployment and easy to inspect with `jq`; swap in
    /// a Redis/SQL adapter by implementing `IStateStore`.
    class FileStateStore final : public IStateStore {
    public:
        explicit FileStateStore(std::string directory);

        void save(const SessionCheckpoint &checkpoint) override;
        [[nodiscard]] std::optional<SessionCheckpoint> load(const std::string &session_id) const override;
        bool remove(const std::string &session_id) override;
        [[nodiscard]] std::vector<SessionCheckpoint> list() const override;
        [[nodiscard]] size_t count(std::optional<SessionStatus> status) const override;
        [[nodiscard]] std::string kind() const override { return "file:" + m_directory; }
        [[nodiscard]] const std::string &directory() const { return m_directory; }

    private:
        [[nodiscard]] std::string pathFor(const std::string &session_id) const;

        std::string m_directory;
        mutable std::mutex m_mutex;
    };

} // namespace sapo::runtime
