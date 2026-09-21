//
//  Sapo Engine — Redis client seam (value type, options, null client).
//
#include "redis/IRedisClient.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>

#include "runtime/SapoError.hpp"

namespace sapo::redis {

    // ---------------------------------------------------------------------
    // RedisValue
    // ---------------------------------------------------------------------
    RedisValue::RedisValue(Type type, std::string text, int64_t number, std::vector<RedisValue> items)
        : m_type(type), m_text(std::move(text)), m_number(number), m_items(std::move(items)) {}

    RedisValue RedisValue::status(std::string text) {
        return RedisValue(Type::Status, std::move(text), 0, {});
    }

    RedisValue RedisValue::error(std::string text) {
        return RedisValue(Type::Error, std::move(text), 0, {});
    }

    RedisValue RedisValue::integer(int64_t value) {
        return RedisValue(Type::Integer, {}, value, {});
    }

    RedisValue RedisValue::bulk(std::string text) {
        return RedisValue(Type::Bulk, std::move(text), 0, {});
    }

    RedisValue RedisValue::array(std::vector<RedisValue> items) {
        return RedisValue(Type::Array, {}, 0, std::move(items));
    }

    int64_t RedisValue::toInt(int64_t fallback) const {
        if (m_type == Type::Integer) return m_number;
        // Some servers/commands answer with a numeric bulk string; accept it
        // rather than forcing every caller to branch.
        if (m_type == Type::Bulk || m_type == Type::Status) {
            if (!m_text.empty()) {
                try {
                    size_t consumed = 0;
                    const long long parsed = std::stoll(m_text, &consumed);
                    if (consumed == m_text.size()) return static_cast<int64_t>(parsed);
                } catch (const std::exception &) {
                    // fall through to the fallback
                }
            }
        }
        return fallback;
    }

    std::optional<std::string> RedisValue::toString() const {
        if (m_type == Type::Bulk || m_type == Type::Status || m_type == Type::Error) return m_text;
        return std::nullopt;
    }

    const std::vector<RedisValue> &RedisValue::toArray() const {
        static const std::vector<RedisValue> kEmpty;
        return m_type == Type::Array ? m_items : kEmpty;
    }

    bool RedisValue::isOkStatus() const {
        return m_type == Type::Status && (m_text == "OK" || m_text == "ok");
    }

    void RedisValue::throwIfError(const std::string &context) const {
        if (m_type != Type::Error) return;
        throw ::sapo::runtime::SapoError(::sapo::runtime::ErrorCode::Store,
                                         context + ": redis replied '" + m_text + "'");
    }

    std::string RedisValue::describe(size_t max_length) const {
        auto clip = [max_length](const std::string &text) {
            if (text.size() <= max_length) return text;
            return text.substr(0, max_length) + "…(" + std::to_string(text.size()) + " bytes)";
        };
        switch (m_type) {
            case Type::Nil: return "<nil>";
            case Type::Status: return "+" + clip(m_text);
            case Type::Error: return "-" + clip(m_text);
            case Type::Integer: return ":" + std::to_string(m_number);
            case Type::Bulk: return "$" + clip(m_text);
            case Type::Array: {
                std::ostringstream out;
                out << '*' << m_items.size() << '[';
                for (size_t index = 0; index < m_items.size() && index < 8; ++index) {
                    if (index != 0) out << ", ";
                    out << m_items[index].describe(40);
                }
                if (m_items.size() > 8) out << ", …";
                out << ']';
                return out.str();
            }
        }
        return "<nil>";
    }

    // ---------------------------------------------------------------------
    // RedisOptions
    // ---------------------------------------------------------------------
    namespace {
        /// Percent-decodes a userinfo component (`p%40ss` → `p@ss`).
        std::string percentDecode(const std::string &text) {
            std::string out;
            out.reserve(text.size());
            for (size_t index = 0; index < text.size(); ++index) {
                if (text[index] == '%' && index + 2 < text.size()) {
                    const auto hex = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return -1;
                    };
                    const int high = hex(text[index + 1]);
                    const int low = hex(text[index + 2]);
                    if (high >= 0 && low >= 0) {
                        out.push_back(static_cast<char>((high << 4) | low));
                        index += 2;
                        continue;
                    }
                }
                out.push_back(text[index]);
            }
            return out;
        }
    } // namespace

    std::optional<RedisOptions> RedisOptions::fromUrl(const std::string &url, std::string *error) {
        auto fail = [error](const std::string &message) -> std::optional<RedisOptions> {
            if (error != nullptr) *error = message;
            return std::nullopt;
        };

        const std::string kScheme = "redis://";
        const std::string kSecureScheme = "rediss://";
        std::string remainder;
        if (url.compare(0, kScheme.size(), kScheme) == 0) {
            remainder = url.substr(kScheme.size());
        } else if (url.compare(0, kSecureScheme.size(), kSecureScheme) == 0) {
            // Refuse rather than silently connect in plaintext to a TLS endpoint.
            return fail("rediss:// (TLS) is not supported by the built-in socket client; "
                        "terminate TLS with a proxy or inject an IRedisClient that supports it");
        } else {
            return fail("expected a redis:// URL, got '" + url + "'");
        }

        RedisOptions options;
        // [userinfo@]host[:port][/db]
        const size_t at = remainder.rfind('@');
        std::string authority = remainder;
        if (at != std::string::npos) {
            const std::string userinfo = remainder.substr(0, at);
            authority = remainder.substr(at + 1);
            const size_t colon = userinfo.find(':');
            if (colon == std::string::npos) {
                // "password@" — no user component.
                options.password = percentDecode(userinfo);
            } else {
                options.username = percentDecode(userinfo.substr(0, colon));
                options.password = percentDecode(userinfo.substr(colon + 1));
            }
        }

        std::string hostport = authority;
        const size_t slash = authority.find('/');
        if (slash != std::string::npos) {
            hostport = authority.substr(0, slash);
            const std::string db = authority.substr(slash + 1);
            if (!db.empty()) {
                try {
                    options.database = std::stoi(db);
                } catch (const std::exception &) {
                    return fail("invalid database index '/" + db + "' in '" + url + "'");
                }
                if (options.database < 0) return fail("database index cannot be negative in '" + url + "'");
            }
        }

        // An IPv6 literal arrives bracketed: [::1]:6379
        if (!hostport.empty() && hostport.front() == '[') {
            const size_t close = hostport.find(']');
            if (close == std::string::npos) return fail("unterminated '[' in host '" + hostport + "'");
            options.host = hostport.substr(1, close - 1);
            const std::string after = hostport.substr(close + 1);
            if (!after.empty()) {
                if (after.front() != ':') return fail("expected ':' after ']' in '" + hostport + "'");
                try {
                    options.port = std::stoi(after.substr(1));
                } catch (const std::exception &) {
                    return fail("invalid port in '" + hostport + "'");
                }
            }
        } else {
            const size_t colon = hostport.rfind(':');
            if (colon == std::string::npos) {
                options.host = hostport;
            } else {
                options.host = hostport.substr(0, colon);
                const std::string port_text = hostport.substr(colon + 1);
                if (!port_text.empty()) {
                    try {
                        options.port = std::stoi(port_text);
                    } catch (const std::exception &) {
                        return fail("invalid port '" + port_text + "' in '" + url + "'");
                    }
                }
            }
        }

        if (options.host.empty()) return fail("no host in '" + url + "'");
        if (options.port <= 0 || options.port > 65535) return fail("port out of range in '" + url + "'");
        return options;
    }

    std::string RedisOptions::describe() const {
        std::ostringstream out;
        out << host << ':' << port << '/' << database;
        if (!username.empty()) out << " user=" << username;
        if (!password.empty()) out << " auth=yes"; // never the secret itself
        return out.str();
    }

    // ---------------------------------------------------------------------
    // NullRedisClient
    // ---------------------------------------------------------------------
    namespace {
        /// Shared by `command` and `pipeline` so both fail identically, and
        /// marked [[noreturn]] so the compiler can see no reply is discarded.
        [[noreturn]] void refuseWithoutClient(const std::string &verb) {
            throw ::sapo::runtime::SapoError(
                ::sapo::runtime::ErrorCode::NotImplemented,
                "no Redis client is available (tried '" + verb + "'): build with -DSAPO_ENABLE_REDIS=ON, "
                "or inject an IRedisClient (e.g. a hiredis/redis-plus-plus adapter) into RedisStateStore");
        }
    } // namespace

    RedisValue NullRedisClient::command(const std::vector<std::string> &args) {
        refuseWithoutClient(args.empty() ? "<empty>" : args[0]);
    }

    std::vector<RedisValue> NullRedisClient::pipeline(const std::vector<std::vector<std::string>> &commands) {
        if (commands.empty()) return {}; // nothing to do is not an error
        refuseWithoutClient(commands.front().empty() ? "<empty>" : commands.front()[0]);
    }

} // namespace sapo::redis
