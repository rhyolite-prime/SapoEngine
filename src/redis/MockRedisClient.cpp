//
//  Sapo Engine — in-memory Redis double.
//
#include "redis/MockRedisClient.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

#include "redis/RedisStateStore.hpp"
#include "runtime/SapoError.hpp"

using ::sapo::runtime::ErrorCode;
using ::sapo::runtime::SapoError;

namespace sapo::redis {

    namespace {
        [[nodiscard]] int64_t parseInt(const std::string &text, int64_t fallback = 0) {
            if (text.empty()) return fallback;
            try {
                size_t consumed = 0;
                const long long value = std::stoll(text, &consumed);
                return consumed == text.size() ? static_cast<int64_t>(value) : fallback;
            } catch (const std::exception &) {
                return fallback;
            }
        }

        [[nodiscard]] double parseDouble(const std::string &text, double fallback = 0.0) {
            if (text.empty()) return fallback;
            try {
                size_t consumed = 0;
                const double value = std::stod(text, &consumed);
                return consumed == text.size() ? value : fallback;
            } catch (const std::exception &) {
                return fallback;
            }
        }

        [[nodiscard]] RedisValue wrongType() {
            return RedisValue::error("WRONGTYPE Operation against a key holding the wrong kind of value");
        }

        [[nodiscard]] RedisValue syntaxError() { return RedisValue::error("ERR syntax error"); }

        /// `glob` supports only `*` and `?`, which is all SCAN MATCH needs here.
        [[nodiscard]] bool globMatch(const std::string &pattern, const std::string &text) {
            size_t p = 0;
            size_t t = 0;
            size_t star = std::string::npos;
            size_t mark = 0;
            while (t < text.size()) {
                if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
                    ++p;
                    ++t;
                } else if (p < pattern.size() && pattern[p] == '*') {
                    star = p++;
                    mark = t;
                } else if (star != std::string::npos) {
                    p = star + 1;
                    t = ++mark;
                } else {
                    return false;
                }
            }
            while (p < pattern.size() && pattern[p] == '*') ++p;
            return p == pattern.size();
        }

        /// ZRANGE index semantics, including negative offsets.
        void resolveRange(int64_t start, int64_t stop, size_t size, size_t &from, size_t &to) {
            const auto normalise = [size](int64_t index) -> int64_t {
                if (index < 0) index += static_cast<int64_t>(size);
                return index;
            };
            int64_t first = normalise(start);
            int64_t last = normalise(stop);
            if (first < 0) first = 0;
            if (last >= static_cast<int64_t>(size)) last = static_cast<int64_t>(size) - 1;
            from = (first > last || first >= static_cast<int64_t>(size)) ? 0 : static_cast<size_t>(first);
            to = (first > last) ? 0 : static_cast<size_t>(last) + 1;
        }
    } // namespace

    MockRedisClient::MockRedisClient() { installSapoScripts(); }

    MockRedisClient::~MockRedisClient() = default;

    // =========================================================================
    // keyspace helpers
    // =========================================================================
    MockRedisClient::Entry *MockRedisClient::findLive(const std::string &key) {
        const auto it = m_data.find(key);
        if (it == m_data.end()) return nullptr;
        if (it->second.expires_at_ms != 0 && m_now_ms >= it->second.expires_at_ms) {
            m_data.erase(it); // lazy expiry, as Redis does on access
            return nullptr;
        }
        return &it->second;
    }

    const MockRedisClient::Entry *MockRedisClient::findLive(const std::string &key) const {
        return const_cast<MockRedisClient *>(this)->findLive(key);
    }

    MockRedisClient::Entry &MockRedisClient::entryFor(const std::string &key, Entry::Kind kind) {
        Entry &entry = m_data[key];
        entry.kind = kind;
        return entry;
    }

    void MockRedisClient::hashSet(Entry &entry, const std::string &field, const std::string &value) {
        for (auto &pair : entry.hash) {
            if (pair.first == field) {
                pair.second = value;
                return;
            }
        }
        entry.hash.emplace_back(field, value);
    }

    std::optional<std::string> MockRedisClient::hashGet(const Entry &entry, const std::string &field) const {
        for (const auto &pair : entry.hash) {
            if (pair.first == field) return pair.second;
        }
        return std::nullopt;
    }

    void MockRedisClient::zadd(const std::string &key, double score, const std::string &member) {
        Entry *entry = findLive(key);
        if (entry == nullptr || entry->kind != Entry::Kind::ZSet) {
            entry = &entryFor(key, Entry::Kind::ZSet);
            entry->zset.clear();
        }
        entry->zset[member] = score;
    }

    void MockRedisClient::zrem(const std::string &key, const std::string &member) {
        Entry *entry = findLive(key);
        if (entry == nullptr || entry->kind != Entry::Kind::ZSet) return;
        entry->zset.erase(member);
        if (entry->zset.empty() && entry->expires_at_ms == 0) m_data.erase(key);
    }

    // =========================================================================
    // command dispatch
    // =========================================================================
    RedisValue MockRedisClient::command(const std::vector<std::string> &args) {
        std::scoped_lock lock(m_mutex);
        m_recorded.push_back(args);
        if (m_down) throw SapoError(ErrorCode::Store, "mock redis is down (simulated transport failure)");
        if (m_failures_left > 0) {
            --m_failures_left;
            return RedisValue::error(m_failure_message);
        }
        return dispatch(args);
    }

    std::vector<RedisValue> MockRedisClient::pipeline(const std::vector<std::vector<std::string>> &commands) {
        std::vector<RedisValue> replies;
        replies.reserve(commands.size());
        for (const auto &args : commands) replies.push_back(command(args));
        return replies;
    }

    bool MockRedisClient::healthy() { return !m_down; }

    RedisValue MockRedisClient::dispatch(const std::vector<std::string> &args) {
        if (args.empty()) return RedisValue::error("ERR empty command");
        std::string verb = args[0];
        std::transform(verb.begin(), verb.end(), verb.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        const auto at = [&args](size_t index) -> std::string { return index < args.size() ? args[index] : ""; };

        if (verb == "PING") return RedisValue::status(at(1).empty() ? "PONG" : at(1));
        if (verb == "AUTH" || verb == "SELECT") return RedisValue::status("OK");
        if (verb == "CLIENT") return RedisValue::status("OK");
        if (verb == "FLUSHDB" || verb == "FLUSHALL") {
            m_data.clear();
            return RedisValue::status("OK");
        }
        if (verb == "DBSIZE") return RedisValue::integer(static_cast<int64_t>(m_data.size()));

        if (verb == "EXISTS") {
            int64_t total = 0;
            for (size_t index = 1; index < args.size(); ++index) {
                if (findLive(args[index]) != nullptr) ++total;
            }
            return RedisValue::integer(total);
        }

        if (verb == "DEL" || verb == "UNLINK") {
            int64_t total = 0;
            for (size_t index = 1; index < args.size(); ++index) {
                if (findLive(args[index]) != nullptr && m_data.erase(args[index]) > 0) ++total;
            }
            return RedisValue::integer(total);
        }

        if (verb == "TTL" || verb == "PTTL") {
            Entry *entry = findLive(at(1));
            if (entry == nullptr) return RedisValue::integer(-2);
            if (entry->expires_at_ms == 0) return RedisValue::integer(-1);
            const int64_t remaining = entry->expires_at_ms - m_now_ms;
            return RedisValue::integer(verb == "TTL" ? (remaining + 999) / 1000 : remaining);
        }

        if (verb == "EXPIRE" || verb == "PEXPIRE") {
            Entry *entry = findLive(at(1));
            if (entry == nullptr) return RedisValue::integer(0);
            const int64_t amount = parseInt(at(2), -1);
            if (amount < 0) {
                m_data.erase(at(1));
                return RedisValue::integer(1);
            }
            entry->expires_at_ms = m_now_ms + (verb == "EXPIRE" ? amount * 1000 : amount);
            return RedisValue::integer(1);
        }

        if (verb == "GET") {
            Entry *entry = findLive(at(1));
            if (entry == nullptr) return RedisValue::nil();
            if (entry->kind != Entry::Kind::String) return wrongType();
            return RedisValue::bulk(entry->string_value);
        }

        if (verb == "SET") {
            if (args.size() < 3) return syntaxError();
            Entry *existing = findLive(at(1));
            bool only_if_absent = false;
            bool only_if_present = false;
            int64_t expiry_ms = 0;
            for (size_t index = 3; index < args.size(); ++index) {
                std::string flag = args[index];
                std::transform(flag.begin(), flag.end(), flag.begin(),
                               [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                if (flag == "NX") only_if_absent = true;
                else if (flag == "XX") only_if_present = true;
                else if (flag == "EX") { expiry_ms = parseInt(at(index + 1)) * 1000; ++index; }
                else if (flag == "PX") { expiry_ms = parseInt(at(index + 1)); ++index; }
                else return syntaxError();
            }
            if (only_if_absent && existing != nullptr) return RedisValue::nil();
            if (only_if_present && existing == nullptr) return RedisValue::nil();
            Entry &entry = entryFor(at(1), Entry::Kind::String);
            entry.string_value = at(2);
            entry.hash.clear();
            entry.zset.clear();
            entry.expires_at_ms = expiry_ms;
            return RedisValue::status("OK");
        }

        if (verb == "HGET") {
            Entry *entry = findLive(at(1));
            if (entry == nullptr) return RedisValue::nil();
            if (entry->kind != Entry::Kind::Hash) return wrongType();
            const auto value = hashGet(*entry, at(2));
            return value.has_value() ? RedisValue::bulk(*value) : RedisValue::nil();
        }

        if (verb == "HMGET") {
            Entry *entry = findLive(at(1));
            if (entry != nullptr && entry->kind != Entry::Kind::Hash) return wrongType();
            std::vector<RedisValue> items;
            for (size_t index = 2; index < args.size(); ++index) {
                const auto value = entry != nullptr ? hashGet(*entry, args[index]) : std::nullopt;
                items.push_back(value.has_value() ? RedisValue::bulk(*value) : RedisValue::nil());
            }
            return RedisValue::array(std::move(items));
        }

        if (verb == "HSET") {
            if (args.size() < 4 || ((args.size() - 2) % 2) != 0) return syntaxError();
            Entry *existing = findLive(at(1));
            if (existing != nullptr && existing->kind != Entry::Kind::Hash) return wrongType();
            Entry &entry = entryFor(at(1), Entry::Kind::Hash);
            int64_t added = 0;
            for (size_t index = 2; index + 1 < args.size(); index += 2) {
                if (!hashGet(entry, args[index]).has_value()) ++added;
                hashSet(entry, args[index], args[index + 1]);
            }
            return RedisValue::integer(added);
        }

        if (verb == "HGETALL") {
            Entry *entry = findLive(at(1));
            if (entry == nullptr) return RedisValue::array({});
            if (entry->kind != Entry::Kind::Hash) return wrongType();
            std::vector<RedisValue> items;
            for (const auto &pair : entry->hash) {
                items.push_back(RedisValue::bulk(pair.first));
                items.push_back(RedisValue::bulk(pair.second));
            }
            return RedisValue::array(std::move(items));
        }

        if (verb == "HDEL") {
            Entry *entry = findLive(at(1));
            if (entry == nullptr || entry->kind != Entry::Kind::Hash) return RedisValue::integer(0);
            int64_t removed = 0;
            for (size_t index = 2; index < args.size(); ++index) {
                const auto before = entry->hash.size();
                entry->hash.erase(std::remove_if(entry->hash.begin(), entry->hash.end(),
                                                 [&](const auto &pair) { return pair.first == args[index]; }),
                                  entry->hash.end());
                removed += static_cast<int64_t>(before - entry->hash.size());
            }
            if (entry->hash.empty()) m_data.erase(at(1));
            return RedisValue::integer(removed);
        }

        if (verb == "ZADD") {
            if (args.size() < 4 || ((args.size() - 2) % 2) != 0) return syntaxError();
            Entry *existing = findLive(at(1));
            if (existing != nullptr && existing->kind != Entry::Kind::ZSet) return wrongType();
            Entry &entry = entryFor(at(1), Entry::Kind::ZSet);
            int64_t added = 0;
            for (size_t index = 2; index + 1 < args.size(); index += 2) {
                if (entry.zset.find(args[index + 1]) == entry.zset.end()) ++added;
                entry.zset[args[index + 1]] = parseDouble(args[index]);
            }
            return RedisValue::integer(added);
        }

        if (verb == "ZREM") {
            Entry *entry = findLive(at(1));
            if (entry == nullptr || entry->kind != Entry::Kind::ZSet) return RedisValue::integer(0);
            int64_t removed = 0;
            for (size_t index = 2; index < args.size(); ++index) removed += entry->zset.erase(args[index]);
            if (entry->zset.empty() && entry->expires_at_ms == 0) m_data.erase(at(1));
            return RedisValue::integer(removed);
        }

        if (verb == "ZCARD" || verb == "ZCOUNT") {
            Entry *entry = findLive(at(1));
            if (entry == nullptr) return RedisValue::integer(0);
            if (entry->kind != Entry::Kind::ZSet) return wrongType();
            return RedisValue::integer(static_cast<int64_t>(entry->zset.size()));
        }

        if (verb == "ZSCORE") {
            Entry *entry = findLive(at(1));
            if (entry == nullptr || entry->kind != Entry::Kind::ZSet) return RedisValue::nil();
            const auto it = entry->zset.find(at(2));
            if (it == entry->zset.end()) return RedisValue::nil();
            std::ostringstream out;
            out << static_cast<int64_t>(it->second);
            return RedisValue::bulk(out.str());
        }

        if (verb == "ZRANGE" || verb == "ZREVRANGE") {
            Entry *entry = findLive(at(1));
            if (entry == nullptr) return RedisValue::array({});
            if (entry->kind != Entry::Kind::ZSet) return wrongType();
            // Redis orders a ZSET by (score, member). The mock keys its map by
            // member, so sort explicitly rather than relying on map order.
            std::vector<std::pair<double, std::string>> ordered;
            ordered.reserve(entry->zset.size());
            for (const auto &pair : entry->zset) ordered.emplace_back(pair.second, pair.first);
            std::sort(ordered.begin(), ordered.end());
            if (verb == "ZREVRANGE") std::reverse(ordered.begin(), ordered.end());
            size_t from = 0;
            size_t to = 0;
            resolveRange(parseInt(at(2)), parseInt(at(3)), ordered.size(), from, to);
            std::vector<RedisValue> items;
            for (size_t index = from; index < to && index < ordered.size(); ++index) {
                items.push_back(RedisValue::bulk(ordered[index].second));
            }
            return RedisValue::array(std::move(items));
        }

        if (verb == "SCAN") {
            const std::string pattern = args.size() > 3 && args[2] == "MATCH" ? args[3] : "*";
            std::vector<RedisValue> keys;
            for (const auto &pair : m_data) {
                if (findLive(pair.first) == nullptr) continue;
                if (globMatch(pattern, pair.first)) keys.push_back(RedisValue::bulk(pair.first));
            }
            std::sort(keys.begin(), keys.end(), [](const RedisValue &a, const RedisValue &b) {
                return a.toString().value_or("") < b.toString().value_or("");
            });
            // A real SCAN is incremental; returning cursor 0 with everything is
            // the single-pass case and is all the store's tests need.
            return RedisValue::array({RedisValue::bulk("0"), RedisValue::array(std::move(keys))});
        }

        if (verb == "EVAL" || verb == "EVALSHA") return evaluate(args);

        return RedisValue::error("ERR unknown command '" + verb + "' (MockRedisClient)");
    }

    RedisValue MockRedisClient::evaluate(const std::vector<std::string> &args) {
        if (args.size() < 3) return RedisValue::error("ERR wrong number of arguments for 'eval' command");
        const std::string &script = args[1];
        const int64_t numkeys = parseInt(args[2], -1);
        if (numkeys < 0 || static_cast<size_t>(numkeys) + 3 > args.size()) {
            return RedisValue::error("ERR invalid number of keys for 'eval' command");
        }
        std::vector<std::string> keys(args.begin() + 3, args.begin() + 3 + numkeys);
        std::vector<std::string> argv(args.begin() + 3 + numkeys, args.end());
        for (const auto &registered : m_scripts) {
            if (script.find(registered.first) != std::string::npos) return registered.second(keys, argv);
        }
        return RedisValue::error("ERR MockRedisClient has no mirror for this script; call onScript() or "
                                 "installSapoScripts(). Real Lua execution is covered by the opt-in "
                                 "live-server tests (SAPO_REDIS_URL).");
    }

    // =========================================================================
    // mirrors of RedisStateStore's Lua
    // =========================================================================
    void MockRedisClient::installSapoScripts() {
        std::scoped_lock lock(m_mutex);
        m_scripts.clear();

        // "-- sapo:session.save v1" — see RedisStateStore::saveScript().
        // Lua KEYS[i] is keys[i-1] here.
        onScript("-- sapo:session.save v1",
                 [this](const std::vector<std::string> &keys, const std::vector<std::string> &argv) -> RedisValue {
                     if (keys.empty() || argv.size() < 6) {
                         return RedisValue::error("ERR sapo save script: bad KEYS/ARGV count");
                     }
                     const std::string &session_key = keys[0];
                     Entry *existing = findLive(session_key);
                     const bool exists = existing != nullptr;
                     int64_t current = 0;
                     if (exists) current = parseInt(hashGet(*existing, "version").value_or("0"));

                     const int64_t expected = parseInt(argv[1], -1);
                     if (expected >= 0 && current != expected) {
                         // {0, current, -1} on conflict, {-1, 0, -1} when absent.
                         return RedisValue::array({RedisValue::integer(exists ? 0 : -1),
                                                   RedisValue::integer(exists ? current : 0),
                                                   RedisValue::integer(-1)});
                     }

                     int64_t previous = -1;
                     if (exists) {
                         const auto status = hashGet(*existing, "status");
                         if (status.has_value()) previous = parseInt(*status, -1);
                     }
                     const int64_t next_version = current + 1;

                     Entry &entry = entryFor(session_key, Entry::Kind::Hash);
                     entry.string_value.clear();
                     entry.zset.clear();
                     hashSet(entry, "blob", argv[0]);
                     hashSet(entry, "version", std::to_string(next_version));
                     hashSet(entry, "status", argv[2]);
                     hashSet(entry, "updated_ms", argv[3]);
                     const int64_t ttl = parseInt(argv[4]);
                     entry.expires_at_ms = ttl > 0 ? m_now_ms + ttl * 1000 : 0;

                     if (keys.size() >= 8) { // atomic_index: KEYS[2]=all, KEYS[3..8]=per status
                         const int64_t wanted = parseInt(argv[2]);
                         const double score = parseDouble(argv[3]);
                         if (previous >= 0 && previous != wanted) zrem(keys[2 + previous], argv[5]);
                         zadd(keys[2 + wanted], score, argv[5]);
                         zadd(keys[1], score, argv[5]);
                     }
                     return RedisValue::array({RedisValue::integer(1), RedisValue::integer(next_version),
                                               RedisValue::integer(previous)});
                 });

        // "-- sapo:claim.release v1" — compare-and-delete.
        onScript("-- sapo:claim.release v1",
                 [this](const std::vector<std::string> &keys, const std::vector<std::string> &argv) -> RedisValue {
                     if (keys.empty() || argv.empty()) return RedisValue::error("ERR sapo claim script: bad args");
                     Entry *entry = findLive(keys[0]);
                     if (entry == nullptr || entry->kind != Entry::Kind::String) return RedisValue::integer(0);
                     if (entry->string_value != argv[0]) return RedisValue::integer(0);
                     return RedisValue::integer(static_cast<int64_t>(m_data.erase(keys[0])));
                 });
    }

    void MockRedisClient::onScript(const std::string &marker, ScriptHandler handler) {
        std::scoped_lock lock(m_mutex);
        for (auto &registered : m_scripts) {
            if (registered.first == marker) {
                registered.second = std::move(handler);
                return;
            }
        }
        m_scripts.emplace_back(marker, std::move(handler));
    }

    // =========================================================================
    // test hooks + inspection
    // =========================================================================
    void MockRedisClient::setDown(bool down) {
        std::scoped_lock lock(m_mutex);
        m_down = down;
    }

    void MockRedisClient::failNextCommands(size_t count, std::string message) {
        std::scoped_lock lock(m_mutex);
        m_failures_left = count;
        m_failure_message = std::move(message);
    }

    void MockRedisClient::advanceTime(int64_t milliseconds) {
        std::scoped_lock lock(m_mutex);
        m_now_ms += milliseconds;
    }

    void MockRedisClient::clear() {
        std::scoped_lock lock(m_mutex);
        m_data.clear();
        m_recorded.clear();
        m_now_ms = 0;
        m_down = false;
        m_failures_left = 0;
    }

    size_t MockRedisClient::keyCount() const {
        std::scoped_lock lock(m_mutex);
        // Snapshot first: findLive() erases expired entries, which would
        // invalidate a range-for over m_data.
        std::vector<std::string> keys;
        keys.reserve(m_data.size());
        for (const auto &pair : m_data) keys.push_back(pair.first);
        size_t total = 0;
        for (const auto &key : keys) {
            if (findLive(key) != nullptr) ++total;
        }
        return total;
    }

    bool MockRedisClient::hasKey(const std::string &key) const {
        std::scoped_lock lock(m_mutex);
        return findLive(key) != nullptr;
    }

    std::optional<std::string> MockRedisClient::hashField(const std::string &key, const std::string &field) const {
        std::scoped_lock lock(m_mutex);
        const Entry *entry = findLive(key);
        if (entry == nullptr || entry->kind != Entry::Kind::Hash) return std::nullopt;
        return hashGet(*entry, field);
    }

    std::optional<std::string> MockRedisClient::stringKey(const std::string &key) const {
        std::scoped_lock lock(m_mutex);
        const Entry *entry = findLive(key);
        if (entry == nullptr || entry->kind != Entry::Kind::String) return std::nullopt;
        return entry->string_value;
    }

    std::vector<std::string> MockRedisClient::sortedMembers(const std::string &key) const {
        std::scoped_lock lock(m_mutex);
        std::vector<std::string> out;
        const Entry *entry = findLive(key);
        if (entry == nullptr || entry->kind != Entry::Kind::ZSet) return out;
        std::vector<std::pair<double, std::string>> ordered;
        ordered.reserve(entry->zset.size());
        for (const auto &pair : entry->zset) ordered.emplace_back(pair.second, pair.first);
        std::sort(ordered.begin(), ordered.end()); // ascending by (score, member)
        out.reserve(ordered.size());
        for (const auto &pair : ordered) out.push_back(pair.second);
        return out;
    }

    std::optional<int64_t> MockRedisClient::ttlSeconds(const std::string &key) const {
        std::scoped_lock lock(m_mutex);
        const Entry *entry = findLive(key);
        if (entry == nullptr) return std::nullopt;
        if (entry->expires_at_ms == 0) return -1;
        return (entry->expires_at_ms - m_now_ms + 999) / 1000;
    }

    std::vector<std::vector<std::string>> MockRedisClient::recordedCommands() const {
        std::scoped_lock lock(m_mutex);
        return m_recorded;
    }

    size_t MockRedisClient::countCalls(const std::string &verb) const {
        std::scoped_lock lock(m_mutex);
        size_t total = 0;
        for (const auto &args : m_recorded) {
            if (!args.empty() && args[0] == verb) ++total;
        }
        return total;
    }

} // namespace sapo::redis
