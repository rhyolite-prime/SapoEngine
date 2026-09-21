//
//  Sapo Engine — session state stores.
//
#include "runtime/SapoError.hpp"
#include "runtime/StateStore.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace sapo::runtime {

    namespace {
        const char *kStatusNames[] = {"running", "awaiting_input", "waiting", "completed", "failed", "cancelled"};
    } // namespace

    const char *toString(SessionStatus status) {
        const auto index = static_cast<size_t>(status);
        return index < std::size(kStatusNames) ? kStatusNames[index] : "running";
    }

    std::optional<SessionStatus> sessionStatusFromString(const std::string &text) {
        for (size_t index = 0; index < std::size(kStatusNames); ++index) {
            if (text == kStatusNames[index]) return static_cast<SessionStatus>(index);
        }
        if (text == "suspended") return SessionStatus::AwaitingInput;
        if (text == "pending") return SessionStatus::Running;
        return std::nullopt;
    }

    nlohmann::json SessionCheckpoint::toJson() const {
        nlohmann::json timers = nlohmann::json::array();
        for (const auto &timer : this->timers) timers.push_back(timer.toJson());
        return nlohmann::json{{"session_id", session_id},
                              {"execution_id", execution_id},
                              {"blueprint_id", blueprint_id},
                              {"blueprint_version", blueprint_version},
                              {"correlation_id", correlation_id},
                              {"status", toString(status)},
                              {"cursor", cursor},
                              {"context", context},
                              {"frames", frames},
                              {"pending", pending},
                              {"timers", timers},
                              {"result", result},
                              {"error", error},
                              {"parent_session", parent_session},
                              {"depth", depth},
                              {"child_sessions", child_sessions},
                              {"created_ms", created_ms},
                              {"updated_ms", updated_ms},
                              {"node_visits", node_visits},
                              {"version", version}};
    }

    SessionCheckpoint SessionCheckpoint::fromJson(const nlohmann::json &value) {
        SessionCheckpoint checkpoint;
        checkpoint.session_id = value.value("session_id", "");
        checkpoint.execution_id = value.value("execution_id", "");
        checkpoint.blueprint_id = value.value("blueprint_id", "");
        checkpoint.blueprint_version = value.value("blueprint_version", "");
        checkpoint.correlation_id = value.value("correlation_id", "");
        checkpoint.status = sessionStatusFromString(value.value("status", "running")).value_or(SessionStatus::Running);
        checkpoint.cursor = value.value("cursor", "");
        checkpoint.context = value.value("context", nlohmann::json::object());
        checkpoint.frames = value.value("frames", nlohmann::json::array());
        checkpoint.pending = value.value("pending", nlohmann::json::object());
        if (value.contains("timers") && value["timers"].is_array()) {
            for (const auto &timer : value["timers"]) checkpoint.timers.push_back(Timer::fromJson(timer));
        }
        checkpoint.result = value.value("result", nlohmann::json::object());
        checkpoint.error = value.value("error", "");
        checkpoint.parent_session = value.value("parent_session", "");
        checkpoint.depth = value.value("depth", static_cast<size_t>(0));
        if (value.contains("child_sessions") && value["child_sessions"].is_array()) {
            for (const auto &child : value["child_sessions"]) {
                if (child.is_string()) checkpoint.child_sessions.push_back(child.get<std::string>());
            }
        }
        checkpoint.created_ms = value.value("created_ms", 0LL);
        checkpoint.updated_ms = value.value("updated_ms", 0LL);
        checkpoint.node_visits = value.value("node_visits", static_cast<size_t>(0));
        checkpoint.version = value.value("version", 0LL);
        return checkpoint;
    }

    const char *toString(SaveResult result) {
        switch (result) {
            case SaveResult::Ok: return "ok";
            case SaveResult::VersionConflict: return "version_conflict";
            case SaveResult::Gone: return "gone";
        }
        return "ok";
    }

    // ---------------------------------------------------------------------
    // In-memory
    // ---------------------------------------------------------------------
    void InMemoryStateStore::save(const SessionCheckpoint &checkpoint) {
        std::scoped_lock lock(m_mutex);
        // Unconditional write, but the version still advances so that a
        // following saveIf() has something to compare against. Every store must
        // behave identically here or a caller's expectations depend on which
        // adapter is wired up.
        const int64_t previous = m_sessions[checkpoint.session_id].version; // default-constructs at 0
        SessionCheckpoint next = checkpoint;
        next.version = previous + 1;
        m_sessions[checkpoint.session_id] = next;
    }

    SaveOutcome InMemoryStateStore::saveIf(const SessionCheckpoint &checkpoint, int64_t expected_version) {
        std::scoped_lock lock(m_mutex);
        const auto it = m_sessions.find(checkpoint.session_id);
        const bool exists = it != m_sessions.end();
        const int64_t current = exists ? it->second.version : 0;
        if (current != expected_version) {
            // Nothing is written: the winner's checkpoint stays intact.
            return SaveOutcome{exists ? SaveResult::VersionConflict : SaveResult::Gone, current};
        }
        SessionCheckpoint next = checkpoint;
        next.version = expected_version + 1;
        m_sessions[checkpoint.session_id] = next;
        return SaveOutcome{SaveResult::Ok, next.version};
    }

    std::optional<SessionCheckpoint> InMemoryStateStore::load(const std::string &session_id) const {
        std::scoped_lock lock(m_mutex);
        auto it = m_sessions.find(session_id);
        if (it == m_sessions.end()) return std::nullopt;
        return it->second;
    }

    bool InMemoryStateStore::remove(const std::string &session_id) {
        std::scoped_lock lock(m_mutex);
        return m_sessions.erase(session_id) != 0;
    }

    std::vector<SessionCheckpoint> InMemoryStateStore::list() const {
        std::scoped_lock lock(m_mutex);
        std::vector<SessionCheckpoint> out;
        out.reserve(m_sessions.size());
        for (const auto &[id, checkpoint] : m_sessions) out.push_back(checkpoint);
        std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.session_id < b.session_id; });
        return out;
    }

    size_t InMemoryStateStore::count(std::optional<SessionStatus> status) const {
        std::scoped_lock lock(m_mutex);
        if (!status.has_value()) return m_sessions.size();
        size_t total = 0;
        for (const auto &[id, checkpoint] : m_sessions) {
            if (checkpoint.status == *status) ++total;
        }
        return total;
    }

    // ---------------------------------------------------------------------
    // File-backed
    // ---------------------------------------------------------------------
    FileStateStore::FileStateStore(std::string directory) : m_directory(std::move(directory)) {
        std::error_code error;
        std::filesystem::create_directories(m_directory, error);
    }

    std::string FileStateStore::pathFor(const std::string &session_id) const {
        std::string safe;
        safe.reserve(session_id.size());
        for (const char character : session_id) {
            const bool alnum = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                               (character >= '0' && character <= '9') || character == '-' || character == '_';
            safe.push_back(alnum ? character : '_');
        }
        if (safe.empty()) safe = "session";
        return (std::filesystem::path(m_directory) / (safe + ".json")).string();
    }

    namespace {
        /// Reads and parses one checkpoint file. Assumes the caller already holds
        /// `FileStateStore::m_mutex` (the mutex is not recursive, so `load()`
        /// cannot be reused from `saveIf()`).
        std::optional<SessionCheckpoint> readCheckpointAt(const std::string &path) {
            std::ifstream in(path, std::ios::binary);
            if (!in) return std::nullopt;
            std::stringstream buffer;
            buffer << in.rdbuf();
            try {
                return SessionCheckpoint::fromJson(nlohmann::json::parse(buffer.str()));
            } catch (const std::exception &) {
                return std::nullopt;
            }
        }

        /// Write-to-temp + `rename`, so a reader never observes a partial file.
        /// Assumes the caller holds the mutex. Throws `SapoError` on failure.
        void writeCheckpointAtomically(const std::string &path, const SessionCheckpoint &checkpoint) {
            const std::string temp = path + ".tmp";
            {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                if (!out) {
                    throw SapoError(ErrorCode::Store, "could not open '" + temp + "' for writing");
                }
                out << checkpoint.toJson().dump(2);
            }
            std::error_code error;
            std::filesystem::rename(temp, path, error);
            if (error) {
                std::error_code ignored;
                std::filesystem::remove(temp, ignored);
                throw SapoError(ErrorCode::Store, "could not persist session '" + checkpoint.session_id + "' to " +
                                                      path);
            }
        }
    } // namespace

    void FileStateStore::save(const SessionCheckpoint &checkpoint) {
        const std::string path = pathFor(checkpoint.session_id);
        std::scoped_lock lock(m_mutex);
        const auto current = readCheckpointAt(path);
        SessionCheckpoint next = checkpoint;
        next.version = (current.has_value() ? current->version : 0) + 1;
        writeCheckpointAtomically(path, next);
    }

    SaveOutcome FileStateStore::saveIf(const SessionCheckpoint &checkpoint, int64_t expected_version) {
        const std::string path = pathFor(checkpoint.session_id);
        std::scoped_lock lock(m_mutex);
        const auto current = readCheckpointAt(path);
        const int64_t current_version = current.has_value() ? current->version : 0;
        if (current_version != expected_version) {
            return SaveOutcome{current.has_value() ? SaveResult::VersionConflict : SaveResult::Gone, current_version};
        }
        SessionCheckpoint next = checkpoint;
        next.version = expected_version + 1;
        writeCheckpointAtomically(path, next);
        return SaveOutcome{SaveResult::Ok, next.version};
    }

    std::optional<SessionCheckpoint> FileStateStore::load(const std::string &session_id) const {
        std::scoped_lock lock(m_mutex);
        return readCheckpointAt(pathFor(session_id));
    }

    bool FileStateStore::remove(const std::string &session_id) {
        std::scoped_lock lock(m_mutex);
        std::error_code error;
        return std::filesystem::remove(pathFor(session_id), error) && !error;
    }

    std::vector<SessionCheckpoint> FileStateStore::list() const {
        std::scoped_lock lock(m_mutex);
        std::vector<SessionCheckpoint> out;
        std::error_code error;
        if (!std::filesystem::exists(m_directory, error)) return out;
        for (const auto &entry : std::filesystem::directory_iterator(m_directory, error)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
            auto checkpoint = readCheckpointAt(entry.path().string());
            if (!checkpoint.has_value()) continue; // a corrupt file must not hide the healthy ones
            out.push_back(std::move(*checkpoint));
        }
        std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.session_id < b.session_id; });
        return out;
    }

    size_t FileStateStore::count(std::optional<SessionStatus> status) const {
        if (!status.has_value()) return list().size();
        size_t total = 0;
        for (const auto &checkpoint : list()) {
            if (checkpoint.status == *status) ++total;
        }
        return total;
    }

} // namespace sapo::runtime
