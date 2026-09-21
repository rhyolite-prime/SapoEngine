//
//  Sapo Engine — Redis-backed session state store.
//
#include "redis/RedisStateStore.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "runtime/SapoError.hpp"

using ::sapo::runtime::ErrorCode;
using ::sapo::runtime::SapoError;
using ::sapo::runtime::SaveOutcome;
using ::sapo::runtime::SaveResult;
using ::sapo::runtime::SessionCheckpoint;
using ::sapo::runtime::SessionStatus;

namespace sapo::redis {

    namespace {
        /// Must match the `SessionStatus` enumerator order — the Lua script
        /// indexes KEYS by it, and `indexKey()` names the ZSET from
        /// `runtime::toString(SessionStatus)`.
        constexpr size_t kStatusCount = 6;
        const char *const kStatusNames[kStatusCount] = {"running", "awaiting_input", "waiting",
                                                        "completed", "failed",        "cancelled"};

        [[nodiscard]] std::optional<SessionStatus> statusFromIndex(int64_t index) {
            if (index < 0 || index >= static_cast<int64_t>(kStatusCount)) return std::nullopt;
            return runtime::sessionStatusFromString(kStatusNames[index]);
        }

        [[nodiscard]] int64_t statusIndex(SessionStatus status) {
            const auto value = static_cast<int64_t>(status);
            return (value >= 0 && value < static_cast<int64_t>(kStatusCount)) ? value : 0;
        }

        [[noreturn]] void fail(const std::string &message) { throw SapoError(ErrorCode::Store, message); }

        /// Size of a page when walking an index for maintenance.
        constexpr int64_t kPrunePage = 500;
    } // namespace

    // =========================================================================
    // Scripts
    //
    // Both are kept deliberately O(1) per session and read only small hash
    // fields: a script runs on Redis's single command thread, so anything that
    // loops or decodes the blob would stall every other client
    // (docs/STATE-STORE-REDIS-VS-TARANTOOL.md §4.4).
    // =========================================================================
    const char *RedisStateStore::saveScript() {
        // The first line is a marker: MockRedisClient dispatches on it so the
        // store's command layout is exercised without a live server. Keep it in
        // sync with MockRedisClient::installSapoScripts().
        return R"LUA(-- sapo:session.save v1
-- KEYS[1]     session hash
-- KEYS[2]     idx:all              (present only when atomic_index is true)
-- KEYS[3..8]  idx:<status>         (enum order; present only when atomic_index)
-- ARGV[1] blob JSON   ARGV[2] expected version (-1 = unconditional)
-- ARGV[3] new status index          ARGV[4] updated_ms (index score)
-- ARGV[5] ttl seconds (0 = none)    ARGV[6] session id (index member)
-- returns {code, version, old_status}; code 1=written 0=conflict -1=gone
local raw = redis.call('HGET', KEYS[1], 'version')
local exists = raw ~= false
local current = 0
if exists then current = tonumber(raw) end
local expected = tonumber(ARGV[2])
if expected >= 0 and current ~= expected then
  if exists then return {0, current, -1} end
  return {-1, 0, -1}
end
local old_status = redis.call('HGET', KEYS[1], 'status')
local previous = -1
if old_status ~= false then previous = tonumber(old_status) end
local next_version = current + 1
redis.call('HSET', KEYS[1], 'blob', ARGV[1], 'version', next_version, 'status', ARGV[3], 'updated_ms', ARGV[4])
local ttl = tonumber(ARGV[5])
if ttl > 0 then redis.call('EXPIRE', KEYS[1], ttl) end
if #KEYS >= 8 then
  local wanted = tonumber(ARGV[3])
  if previous >= 0 and previous ~= wanted then redis.call('ZREM', KEYS[3 + previous], ARGV[6]) end
  redis.call('ZADD', KEYS[3 + wanted], ARGV[4], ARGV[6])
  redis.call('ZADD', KEYS[2], ARGV[4], ARGV[6])
end
return {1, next_version, previous}
)LUA";
    }

    const char *RedisStateStore::releaseClaimScript() {
        return R"LUA(-- sapo:claim.release v1
-- KEYS[1] claim key, ARGV[1] token. Deletes only if the token still owns it,
-- so a late release cannot drop a claim another caller took after expiry.
if redis.call('GET', KEYS[1]) == ARGV[1] then return redis.call('DEL', KEYS[1]) end
return 0
)LUA";
    }

    // =========================================================================
    // options
    // =========================================================================
    std::string RedisStateStoreOptions::describe() const {
        std::ostringstream out;
        out << key_prefix << "* ttl=" << ttl_seconds << "s atomic_index=" << (atomic_index ? "yes" : "no")
            << " max_list=" << max_list;
        return out.str();
    }

    // =========================================================================
    // RedisStateStore
    // =========================================================================
    RedisStateStore::RedisStateStore(RedisClientPtr client, RedisStateStoreOptions options)
        : m_client(std::move(client)), m_options(std::move(options)) {
        if (m_client == nullptr) fail("RedisStateStore requires an IRedisClient (got null)");
        if (m_options.key_prefix.empty()) fail("RedisStateStoreOptions::key_prefix must not be empty");
        if (m_options.index_prefix.empty()) fail("RedisStateStoreOptions::index_prefix must not be empty");
        if (m_options.claim_prefix.empty()) fail("RedisStateStoreOptions::claim_prefix must not be empty");
        if (m_options.ttl_seconds < 0) fail("RedisStateStoreOptions::ttl_seconds must be >= 0 (0 disables expiry)");
    }

    std::string RedisStateStore::sessionKey(const std::string &session_id) const {
        if (session_id.empty()) fail("cannot build a redis key for an empty session id");
        return m_options.key_prefix + session_id;
    }

    std::string RedisStateStore::indexKey(std::optional<SessionStatus> status) const {
        if (!status.has_value()) return m_options.index_prefix + "all";
        return m_options.index_prefix + runtime::toString(*status);
    }

    std::vector<std::string> RedisStateStore::statusIndexKeys() const {
        std::vector<std::string> keys;
        keys.reserve(kStatusCount);
        for (const char *name : kStatusNames) keys.push_back(m_options.index_prefix + name);
        return keys;
    }

    std::vector<std::vector<std::string>> RedisStateStore::removeFromEveryIndex(const std::string &session_id) const {
        std::vector<std::vector<std::string>> batch;
        batch.reserve(1 + kStatusCount);
        batch.push_back({"ZREM", indexKey(std::nullopt), session_id});
        for (const auto &key : statusIndexKeys()) batch.push_back({"ZREM", key, session_id});
        return batch;
    }

    std::string RedisStateStore::claimKey(const std::string &session_id) const {
        if (session_id.empty()) fail("cannot build a claim key for an empty session id");
        return m_options.claim_prefix + session_id;
    }

    std::string RedisStateStore::serialize(const SessionCheckpoint &checkpoint) const {
        // Compact on the wire: 1 539 vs 1 976 bytes measured on a real
        // checkpoint, paid on every write and every reply.
        return m_options.compact_json ? checkpoint.toJson().dump() : checkpoint.toJson().dump(2);
    }

    std::string RedisStateStore::kind() const {
        return "redis:" + m_options.key_prefix;
    }

    // ---------------------------------------------------------------------
    // write path
    // ---------------------------------------------------------------------
    SaveOutcome RedisStateStore::write(const SessionCheckpoint &checkpoint, int64_t expected_version) const {
        const std::string session_id = checkpoint.session_id;
        const std::string blob = serialize(checkpoint);

        std::vector<std::string> keys;
        keys.reserve(m_options.atomic_index ? 2 + kStatusCount : 1);
        keys.push_back(sessionKey(session_id));
        if (m_options.atomic_index) {
            keys.push_back(indexKey(std::nullopt));
            for (const auto &status_key : statusIndexKeys()) keys.push_back(status_key);
        }

        std::vector<std::string> args;
        args.reserve(3 + keys.size() + 6);
        args.push_back("EVAL");
        args.emplace_back(saveScript());
        args.push_back(std::to_string(keys.size()));
        for (const auto &key : keys) args.push_back(key);
        args.push_back(blob);
        args.push_back(std::to_string(expected_version));
        args.push_back(std::to_string(statusIndex(checkpoint.status)));
        args.push_back(std::to_string(checkpoint.updated_ms));
        args.push_back(std::to_string(m_options.ttl_seconds));
        args.push_back(session_id);

        RedisValue reply = m_client->command(args);
        reply.throwIfError("redis write of session '" + session_id + "' failed");

        const auto &items = reply.toArray();
        if (items.size() != 3) {
            fail("redis write of session '" + session_id + "' returned " + reply.describe() +
                 ", expected a 3-element array (is the save script intact on the server?)");
        }
        const int64_t code = items[0].toInt();
        const int64_t version = items[1].toInt();
        const int64_t previous = items[2].toInt(-1);

        switch (code) {
            case 1: break;
            case 0:
                // Nothing was written; `version` is the winner's, so the caller
                // can reload and retry instead of guessing.
                return SaveOutcome{SaveResult::VersionConflict, version};
            case -1: return SaveOutcome{SaveResult::Gone, 0};
            default:
                fail("redis write of session '" + session_id + "' returned an unknown code " + std::to_string(code));
        }

        if (!m_options.atomic_index) {
            updateIndex(session_id, statusFromIndex(previous), checkpoint.status, checkpoint.updated_ms);
        }
        return SaveOutcome{SaveResult::Ok, version};
    }

    void RedisStateStore::updateIndex(const std::string &session_id, std::optional<SessionStatus> previous,
                                      SessionStatus next, int64_t updated_ms) const {
        const std::string score = std::to_string(updated_ms);
        std::vector<std::vector<std::string>> batch;
        batch.reserve(3);
        if (previous.has_value() && *previous != next) batch.push_back({"ZREM", indexKey(*previous), session_id});
        batch.push_back({"ZADD", indexKey(next), score, session_id});
        batch.push_back({"ZADD", indexKey(std::nullopt), score, session_id});
        for (const auto &reply : m_client->pipeline(batch)) reply.throwIfError("redis index update failed");
    }

    void RedisStateStore::save(const SessionCheckpoint &checkpoint) {
        // expected_version < 0 ⇒ unconditional. The script still bumps the
        // stored version, so a later saveIf() has something to compare against.
        const auto outcome = write(checkpoint, -1);
        if (!outcome.ok()) fail("unconditional redis write of session '" + checkpoint.session_id + "' was refused");
    }

    SaveOutcome RedisStateStore::saveIf(const SessionCheckpoint &checkpoint, int64_t expected_version) {
        if (expected_version < 0) {
            fail("saveIf() needs an expected version >= 0; use save() for an unconditional write");
        }
        return write(checkpoint, expected_version);
    }

    // ---------------------------------------------------------------------
    // read path
    // ---------------------------------------------------------------------
    std::optional<SessionCheckpoint> RedisStateStore::load(const std::string &session_id) const {
        RedisValue reply = m_client->command({"HMGET", sessionKey(session_id), "blob", "version"});
        reply.throwIfError("redis load of session '" + session_id + "' failed");
        const auto &items = reply.toArray();
        if (items.size() != 2) fail("redis load of session '" + session_id + "' returned " + reply.describe());
        if (items[0].isNil()) return std::nullopt;
        const auto blob = items[0].toString();
        if (!blob.has_value()) fail("redis load of session '" + session_id + "' returned " + items[0].describe());

        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(*blob);
        } catch (const std::exception &error) {
            // Loud, not nullopt: a corrupt checkpoint must not look like an
            // absent session and silently restart the conversation.
            fail("session '" + session_id + "' holds a corrupt checkpoint: " + error.what());
        }
        auto checkpoint = SessionCheckpoint::fromJson(parsed);
        // The hash field is authoritative. The blob's own copy was serialized
        // before the script knew the new version, so it always trails by one.
        checkpoint.version = items[1].toInt(checkpoint.version);
        return checkpoint;
    }

    bool RedisStateStore::remove(const std::string &session_id) {
        const std::string key = sessionKey(session_id);
        RedisValue status_reply = m_client->command({"HGET", key, "status"});
        status_reply.throwIfError("redis remove of session '" + session_id + "' failed");

        std::optional<SessionStatus> previous;
        if (const auto text = status_reply.toString()) previous = runtime::sessionStatusFromString(*text);

        std::vector<std::vector<std::string>> batch;
        batch.push_back({"DEL", key});
        if (previous.has_value()) {
            batch.push_back({"ZREM", indexKey(std::nullopt), session_id});
            batch.push_back({"ZREM", indexKey(*previous), session_id});
        } else {
            // Hash already gone (expired, or a concurrent remove): clear every
            // index it could have been a member of. Seven O(log N) ZREMs, and
            // this is off the hot path.
            for (auto &command : removeFromEveryIndex(session_id)) batch.push_back(std::move(command));
        }

        const auto replies = m_client->pipeline(batch);
        if (replies.empty()) fail("redis remove of session '" + session_id + "' got no reply");
        replies.front().throwIfError("redis remove of session '" + session_id + "' failed");
        return replies.front().toInt() > 0;
    }

    size_t RedisStateStore::count(std::optional<SessionStatus> status) const {
        RedisValue reply = m_client->command({"ZCARD", indexKey(status)});
        reply.throwIfError("redis count failed");
        return static_cast<size_t>(std::max<int64_t>(0, reply.toInt()));
    }

    std::vector<SessionCheckpoint> RedisStateStore::list() const {
        if (m_options.max_list == 0) return {};
        RedisValue ids_reply = m_client->command({"ZREVRANGE", indexKey(std::nullopt), "0",
                                                 std::to_string(m_options.max_list - 1)});
        ids_reply.throwIfError("redis list failed");

        std::vector<std::string> ids;
        ids.reserve(ids_reply.toArray().size());
        for (const auto &item : ids_reply.toArray()) {
            if (auto id = item.toString()) ids.push_back(std::move(*id));
        }
        if (ids.empty()) return {};

        std::vector<std::vector<std::string>> batch;
        batch.reserve(ids.size());
        for (const auto &id : ids) batch.push_back({"HMGET", sessionKey(id), "blob", "version"});
        const auto replies = m_client->pipeline(batch);

        std::vector<SessionCheckpoint> out;
        out.reserve(replies.size());
        std::vector<std::vector<std::string>> prune;
        for (size_t index = 0; index < replies.size() && index < ids.size(); ++index) {
            const auto &items = replies[index].toArray();
            if (items.size() != 2 || items[0].isNil()) {
                // Indexed but expired: prune lazily so the index self-heals
                // instead of waiting for pruneExpired().
                for (auto &command : removeFromEveryIndex(ids[index])) prune.push_back(std::move(command));
                continue;
            }
            const auto blob = items[0].toString();
            if (!blob.has_value()) continue;
            try {
                auto checkpoint = SessionCheckpoint::fromJson(nlohmann::json::parse(*blob));
                checkpoint.version = items[1].toInt(checkpoint.version);
                out.push_back(std::move(checkpoint));
            } catch (const std::exception &) {
                continue; // one corrupt session must not hide the healthy ones
            }
        }
        if (!prune.empty()) {
            try {
                // Best-effort index cleanup: the replies are irrelevant here, but the
                // call is [[nodiscard]] — bind them to an ignored local instead of
                // discarding, which is what Clang's -Wunused-result wants to see.
                [[maybe_unused]] const auto prune_replies = m_client->pipeline(prune);
            } catch (const std::exception &) {
                // Never fail a read because of index maintenance.
            }
        }

        // Match InMemory/File ordering so callers see stable behaviour across stores.
        std::sort(out.begin(), out.end(),
                  [](const SessionCheckpoint &a, const SessionCheckpoint &b) { return a.session_id < b.session_id; });
        return out;
    }

    // ---------------------------------------------------------------------
    // claims (idempotent resume)
    // ---------------------------------------------------------------------
    bool RedisStateStore::tryClaim(const std::string &session_id, const std::string &token, int ttl_ms) {
        if (token.empty()) fail("tryClaim() needs a non-empty token (use the execution id)");
        if (ttl_ms <= 0) fail("tryClaim() needs a positive ttl_ms so a crashed holder cannot wedge the session");
        RedisValue reply = m_client->command({"SET", claimKey(session_id), token, "NX", "PX", std::to_string(ttl_ms)});
        reply.throwIfError("redis claim of session '" + session_id + "' failed");
        // SET … NX replies +OK when it took the key and Nil when the guard held.
        return reply.isOkStatus();
    }

    bool RedisStateStore::releaseClaim(const std::string &session_id, const std::string &token) {
        std::vector<std::string> args{"EVAL", releaseClaimScript(), "1", claimKey(session_id), token};
        RedisValue reply = m_client->command(args);
        reply.throwIfError("redis release of claim '" + session_id + "' failed");
        return reply.toInt() > 0;
    }

    // ---------------------------------------------------------------------
    // maintenance
    // ---------------------------------------------------------------------
    size_t RedisStateStore::pruneExpired() {
        size_t removed = 0;
        const std::string all = indexKey(std::nullopt);
        int64_t offset = 0;
        // Every pass either removes at least one member (so the index strictly
        // shrinks) or advances the offset, so this terminates on its own. The
        // cap is a belt-and-braces guard against a wedged server turning a
        // maintenance tick into a hang.
        for (int guard = 0; guard < 100000; ++guard) {
            RedisValue page =
                m_client->command({"ZRANGE", all, std::to_string(offset), std::to_string(offset + kPrunePage - 1)});
            page.throwIfError("redis pruneExpired failed");
            const auto &members = page.toArray();
            if (members.empty()) break;

            std::vector<std::string> ids;
            std::vector<std::vector<std::string>> probes;
            ids.reserve(members.size());
            probes.reserve(members.size());
            for (const auto &member : members) {
                auto id = member.toString();
                if (!id.has_value()) continue;
                probes.push_back({"EXISTS", sessionKey(*id)});
                ids.push_back(std::move(*id));
            }

            std::vector<std::vector<std::string>> prune;
            if (!probes.empty()) {
                const auto replies = m_client->pipeline(probes);
                for (size_t index = 0; index < replies.size() && index < ids.size(); ++index) {
                    if (replies[index].toInt() != 0) continue; // session hash still alive
                    for (auto &command : removeFromEveryIndex(ids[index])) prune.push_back(std::move(command));
                    ++removed;
                }
            }

            if (!prune.empty()) {
                for (const auto &reply : m_client->pipeline(prune)) reply.throwIfError("redis pruneExpired failed");
                continue; // the index shrank, so re-read this offset
            }
            offset += static_cast<int64_t>(ids.size());
            if (static_cast<int64_t>(members.size()) < kPrunePage) break; // last page, nothing stale on it
        }
        return removed;
    }

} // namespace sapo::redis
