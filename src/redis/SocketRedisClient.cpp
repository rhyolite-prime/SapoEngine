//
//  Sapo Engine — POSIX-socket RESP2 client.
//
//  Only the commands `RedisStateStore` needs are exercised, but the wire layer
//  is complete RESP2: any of the five reply types, nested arrays, nil, inline
//  error replies, socket timeouts, and a pool sized to the engine's worker
//  count. `encodeCommand` and the reply parser are pure enough to test without
//  a server; see tests/test_redis_state_store.cpp.
//
#include "redis/SocketRedisClient.hpp"

#include <chrono>
#include <exception>
#include <sstream>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "runtime/SapoError.hpp"

using ::sapo::runtime::ErrorCode;
using ::sapo::runtime::SapoError;

// macOS has no MSG_NOSIGNAL; it uses a per-socket option instead.
#ifndef MSG_NOSIGNAL
#define SAPO_MSG_NOSIGNAL 0
#else
#define SAPO_MSG_NOSIGNAL MSG_NOSIGNAL
#endif

namespace sapo::redis {

    namespace {
        constexpr size_t kReadChunk = 16 * 1024;
        constexpr size_t kBufferCompactThreshold = 64 * 1024;
        constexpr int kMaxReplyDepth = 32;          // nested-array guard
        constexpr int64_t kMaxArrayElements = 4 * 1024 * 1024;

        [[noreturn]] void fail(const std::string &message) { throw SapoError(ErrorCode::Store, message); }

        [[noreturn]] void timedOut(const std::string &what, int milliseconds, const RedisOptions &options) {
            throw SapoError(ErrorCode::Timeout, what + " timed out after " + std::to_string(milliseconds) +
                                                    "ms against " + options.describe());
        }

        [[nodiscard]] std::string lastErrorText() {
            const int code = errno;
            char buffer[256] = {0};
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
            const char *text = strerror_r(code, buffer, sizeof(buffer));
            return std::string(text != nullptr ? text : "unknown error") + " (errno " + std::to_string(code) + ")";
#else
            strerror_r(code, buffer, sizeof(buffer));
            return std::string(buffer) + " (errno " + std::to_string(code) + ")";
#endif
        }

        void applySocketTimeouts(int descriptor, int milliseconds) {
            timeval value{};
            value.tv_sec = milliseconds / 1000;
            value.tv_usec = (milliseconds % 1000) * 1000;
            ::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
            ::setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
        }
    } // namespace

    // =========================================================================
    // Connection — one socket, its read buffer, and the RESP2 codec
    // =========================================================================
    class SocketRedisClient::Connection {
    public:
        explicit Connection(const RedisOptions &options) : m_options(options) {
            open();
            handshake();
        }

        ~Connection() {
            if (m_descriptor >= 0) ::close(m_descriptor);
        }

        Connection(const Connection &) = delete;
        Connection &operator=(const Connection &) = delete;

        /// Writes every byte or throws; marks the connection broken so the pool
        /// discards it rather than handing a half-written stream to another caller.
        void sendAll(const std::string &bytes);
        [[nodiscard]] RedisValue readReply() { return readReplyAtDepth(0); }
        void markBroken() { m_broken = true; }
        [[nodiscard]] bool broken() const { return m_broken; }

    private:
        void open();
        void handshake();
        void expectStatus(const std::vector<std::string> &args, const char *what, bool tolerate_error);
        [[nodiscard]] RedisValue readReplyAtDepth(int depth);
        bool fill();
        bool readLine(std::string &out);
        bool readExact(size_t count, std::string &out);

        RedisOptions m_options;
        int m_descriptor{-1};
        std::string m_buffer;
        size_t m_pos{0}; // bytes of m_buffer already consumed
        bool m_broken{false};
    };

    void SocketRedisClient::Connection::open() {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *resolved = nullptr;
        const std::string port_text = std::to_string(m_options.port);
        const int lookup = ::getaddrinfo(m_options.host.c_str(), port_text.c_str(), &hints, &resolved);
        if (lookup != 0) {
            fail("could not resolve redis host '" + m_options.host + "': " + gai_strerror(lookup));
        }
        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(resolved, freeaddrinfo);

        std::string last_error = "no addresses returned";
        for (addrinfo *entry = addresses.get(); entry != nullptr; entry = entry->ai_next) {
            const int descriptor = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
            if (descriptor < 0) {
                last_error = "socket(): " + lastErrorText();
                continue;
            }

            // Non-blocking connect so connect_timeout_ms is actually enforced
            // (a blocking connect to an unroutable address can hang for minutes).
            const int flags = ::fcntl(descriptor, F_GETFL, 0);
            ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
            const int started = ::connect(descriptor, entry->ai_addr, entry->ai_addrlen);
            if (started != 0 && errno == EINPROGRESS) {
                pollfd watched{descriptor, POLLOUT, 0};
                int polled = 0;
                do {
                    polled = ::poll(&watched, 1, m_options.connect_timeout_ms);
                } while (polled < 0 && errno == EINTR);
                if (polled <= 0) {
                    ::close(descriptor);
                    last_error = polled == 0 ? "connect timed out" : "poll(): " + lastErrorText();
                    continue;
                }
                int socket_error = 0;
                socklen_t length = sizeof(socket_error);
                ::getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &socket_error, &length);
                if (socket_error != 0) {
                    ::close(descriptor);
                    last_error = std::strerror(socket_error);
                    continue;
                }
            } else if (started != 0) {
                const int code = errno;
                ::close(descriptor);
                last_error = std::strerror(code);
                continue;
            }

            ::fcntl(descriptor, F_SETFL, flags); // back to blocking + SO_*TIMEO
            applySocketTimeouts(descriptor, m_options.socket_timeout_ms);
            int one = 1;
            ::setsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); // a request is one small write
#ifndef MSG_NOSIGNAL
            ::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
            m_descriptor = descriptor;
            return;
        }
        fail("could not connect to redis at " + m_options.describe() + ": " + last_error);
    }

    void SocketRedisClient::Connection::expectStatus(const std::vector<std::string> &args, const char *what,
                                                     bool tolerate_error) {
        sendAll(SocketRedisClient::encodeCommand(args));
        RedisValue reply = readReply();
        if (reply.isError()) {
            if (tolerate_error) return; // cosmetic (CLIENT SETNAME on a very old server)
            // The context is a literal, never `args`: AUTH arguments carry the password.
            reply.throwIfError(std::string("redis ") + what + " failed");
        }
    }

    void SocketRedisClient::Connection::handshake() {
        if (!m_options.password.empty()) {
            std::vector<std::string> auth{"AUTH"};
            if (!m_options.username.empty()) auth.push_back(m_options.username); // ACL form (Redis 6+)
            auth.push_back(m_options.password);
            expectStatus(auth, "AUTH", false);
        }
        if (m_options.database != 0) {
            expectStatus({"SELECT", std::to_string(m_options.database)}, "SELECT", false);
        }
        if (!m_options.client_name.empty()) {
            expectStatus({"CLIENT", "SETNAME", m_options.client_name}, "CLIENT SETNAME", true);
        }
    }

    void SocketRedisClient::Connection::sendAll(const std::string &bytes) {
        size_t sent = 0;
        while (sent < bytes.size()) {
            const ssize_t written =
                ::send(m_descriptor, bytes.data() + sent, bytes.size() - sent, SAPO_MSG_NOSIGNAL);
            if (written > 0) {
                sent += static_cast<size_t>(written);
                continue;
            }
            if (written < 0 && errno == EINTR) continue;
            m_broken = true;
            if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                timedOut("redis write to " + m_options.describe(), m_options.socket_timeout_ms, m_options);
            }
            fail("redis write to " + m_options.describe() + " failed: " + lastErrorText());
        }
    }

    bool SocketRedisClient::Connection::fill() {
        // Drop the consumed prefix so a long-lived connection does not grow without bound.
        if (m_pos == m_buffer.size()) {
            m_buffer.clear();
            m_pos = 0;
        } else if (m_pos > kBufferCompactThreshold) {
            m_buffer.erase(0, m_pos);
            m_pos = 0;
        }
        const size_t start = m_buffer.size();
        m_buffer.resize(start + kReadChunk);
        for (;;) {
            const ssize_t received = ::recv(m_descriptor, m_buffer.data() + start, kReadChunk, 0);
            if (received > 0) {
                m_buffer.resize(start + static_cast<size_t>(received));
                return true;
            }
            if (received < 0 && errno == EINTR) continue;
            m_buffer.resize(start);
            m_broken = true;
            if (received == 0) return false; // peer closed
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                timedOut("redis read from " + m_options.describe(), m_options.socket_timeout_ms, m_options);
            }
            fail("redis read from " + m_options.describe() + " failed: " + lastErrorText());
        }
    }

    bool SocketRedisClient::Connection::readLine(std::string &out) {
        for (;;) {
            const size_t found = m_buffer.find("\r\n", m_pos);
            if (found != std::string::npos) {
                if (found - m_pos > m_options.max_command_bytes) {
                    m_broken = true;
                    fail("redis reply line exceeds " + std::to_string(m_options.max_command_bytes) + " bytes");
                }
                out.assign(m_buffer, m_pos, found - m_pos);
                m_pos = found + 2;
                return true;
            }
            if (!fill()) return false;
        }
    }

    bool SocketRedisClient::Connection::readExact(size_t count, std::string &out) {
        while (m_buffer.size() - m_pos < count) {
            if (!fill()) return false;
        }
        out.assign(m_buffer, m_pos, count);
        m_pos += count;
        return true;
    }

    RedisValue SocketRedisClient::Connection::readReplyAtDepth(int depth) {
        if (depth > kMaxReplyDepth) {
            m_broken = true;
            fail("redis reply nested deeper than " + std::to_string(kMaxReplyDepth) + " levels");
        }
        std::string line;
        if (!readLine(line)) {
            m_broken = true;
            fail("redis at " + m_options.describe() + " closed the connection mid-reply");
        }
        if (line.empty()) {
            m_broken = true;
            fail("redis at " + m_options.describe() + " sent an empty RESP line");
        }

        const char kind = line[0];
        const std::string payload = line.substr(1);
        switch (kind) {
            case '+': return RedisValue::status(payload);
            case '-': return RedisValue::error(payload);
            case ':': {
                try {
                    return RedisValue::integer(std::stoll(payload));
                } catch (const std::exception &) {
                    m_broken = true;
                    fail("redis sent a malformed integer reply ':" + payload + "'");
                }
            }
            case '$': {
                long long length = 0;
                try {
                    length = std::stoll(payload);
                } catch (const std::exception &) {
                    m_broken = true;
                    fail("redis sent a malformed bulk length '$" + payload + "'");
                }
                if (length == -1) return RedisValue::nil();
                if (length < -1) {
                    m_broken = true;
                    fail("redis sent a negative bulk length '$" + payload + "'");
                }
                if (static_cast<size_t>(length) > m_options.max_command_bytes) {
                    m_broken = true;
                    fail("redis reply of " + std::to_string(length) + " bytes exceeds the " +
                         std::to_string(m_options.max_command_bytes) + "-byte guard (runaway value?)");
                }
                std::string data;
                if (!readExact(static_cast<size_t>(length) + 2, data)) { // payload + trailing CRLF
                    m_broken = true;
                    fail("redis at " + m_options.describe() + " closed the connection inside a bulk reply");
                }
                data.resize(static_cast<size_t>(length));
                return RedisValue::bulk(std::move(data));
            }
            case '*': {
                long long count = 0;
                try {
                    count = std::stoll(payload);
                } catch (const std::exception &) {
                    m_broken = true;
                    fail("redis sent a malformed array length '*" + payload + "'");
                }
                if (count == -1) return RedisValue::nil();
                if (count < -1 || count > kMaxArrayElements) {
                    m_broken = true;
                    fail("redis sent an implausible array length '*" + payload + "'");
                }
                std::vector<RedisValue> items;
                items.reserve(static_cast<size_t>(count));
                for (long long index = 0; index < count; ++index) items.push_back(readReplyAtDepth(depth + 1));
                return RedisValue::array(std::move(items));
            }
            default:
                m_broken = true;
                fail(std::string("redis sent an unsupported RESP type byte '") + kind + "' (RESP3? inline reply?)");
        }
    }

    // =========================================================================
    // Lease — RAII pool checkout
    // =========================================================================
    class SocketRedisClient::Lease {
    public:
        Lease(SocketRedisClient *owner, std::unique_ptr<Connection> connection)
            : m_owner(owner), m_connection(std::move(connection)) {}

        Lease(Lease &&other) noexcept
            : m_owner(other.m_owner), m_connection(std::move(other.m_connection)), m_broken(other.m_broken) {
            other.m_owner = nullptr;
        }

        ~Lease() {
            if (m_owner != nullptr) m_owner->release(std::move(m_connection), m_broken);
        }

        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        Lease &operator=(Lease &&) = delete;

        [[nodiscard]] Connection &connection() const { return *m_connection; }
        void markBroken() {
            m_broken = true;
            if (m_connection) m_connection->markBroken();
        }

    private:
        SocketRedisClient *m_owner;
        std::unique_ptr<Connection> m_connection;
        bool m_broken{false};
    };

    // =========================================================================
    // SocketRedisClient — the pool
    // =========================================================================
    SocketRedisClient::SocketRedisClient(RedisOptions options) : m_options(std::move(options)) {
        if (m_options.pool_size == 0) m_options.pool_size = 1;
    }

    SocketRedisClient::~SocketRedisClient() {
        std::scoped_lock lock(m_mutex);
        m_shutdown = true;
        m_idle.clear(); // closes every idle socket
        m_available.notify_all();
    }

    SocketRedisClient::Lease SocketRedisClient::acquire() {
        const auto budget = std::chrono::milliseconds(m_options.connect_timeout_ms + m_options.socket_timeout_ms);
        const auto deadline = std::chrono::steady_clock::now() + budget;
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;) {
            if (m_shutdown) fail("redis client for " + m_options.describe() + " is shut down");
            if (!m_idle.empty()) {
                std::unique_ptr<Connection> connection = std::move(m_idle.front());
                m_idle.pop_front();
                return Lease(this, std::move(connection));
            }
            if (m_live < m_options.pool_size) {
                ++m_live; // reserve the slot before releasing the lock to connect
                lock.unlock();
                std::unique_ptr<Connection> connection;
                try {
                    connection = std::make_unique<Connection>(m_options);
                } catch (...) {
                    lock.lock();
                    --m_live;
                    m_available.notify_one();
                    throw;
                }
                lock.lock();
                return Lease(this, std::move(connection));
            }
            if (m_available.wait_until(lock, deadline) == std::cv_status::timeout) {
                fail("redis connection pool exhausted: all " + std::to_string(m_options.pool_size) +
                     " connections to " + m_options.describe() + " were busy for " +
                     std::to_string(budget.count()) +
                     "ms — raise pool_size to at least the engine's worker count");
            }
        }
    }

    void SocketRedisClient::release(std::unique_ptr<Connection> connection, bool broken) {
        std::scoped_lock lock(m_mutex);
        if (m_live > 0) --m_live;
        if (!broken && !m_shutdown && connection && !connection->broken()) m_idle.push_back(std::move(connection));
        m_available.notify_one();
    }

    RedisValue SocketRedisClient::withRetry(const std::function<RedisValue(Connection &)> &body) {
        std::exception_ptr last_error;
        for (int attempt = 0; attempt < 2; ++attempt) {
            Lease lease = acquire();
            try {
                return body(lease.connection());
            } catch (const SapoError &error) {
                lease.markBroken();
                last_error = std::current_exception();
                // A timeout already consumed the whole budget; retrying would
                // double it and blow the caller's deadline (the USSD window is
                // 2s). Only a connection-level failure is worth one more try.
                if (error.code() != ErrorCode::Store) break;
            } catch (...) {
                lease.markBroken();
                last_error = std::current_exception();
            }
        }
        if (last_error) std::rethrow_exception(last_error);
        fail("redis command failed with no recorded error"); // unreachable
    }

    std::string SocketRedisClient::encodeCommand(const std::vector<std::string> &args) {
        size_t capacity = 16;
        for (const auto &arg : args) capacity += arg.size() + 16;
        std::string out;
        out.reserve(capacity);
        out += '*';
        out += std::to_string(args.size());
        out += "\r\n";
        for (const auto &arg : args) {
            out += '$';
            out += std::to_string(arg.size());
            out += "\r\n";
            out += arg; // binary-safe: the length prefix frames it, no escaping needed
            out += "\r\n";
        }
        return out;
    }

    RedisValue SocketRedisClient::command(const std::vector<std::string> &args) {
        if (args.empty()) fail("cannot send an empty redis command");
        const std::string encoded = encodeCommand(args);
        return withRetry([&encoded](Connection &connection) {
            connection.sendAll(encoded);
            return connection.readReply();
        });
    }

    std::vector<RedisValue> SocketRedisClient::pipeline(const std::vector<std::vector<std::string>> &commands) {
        if (commands.empty()) return {};
        std::string encoded;
        for (const auto &args : commands) {
            if (args.empty()) fail("cannot send an empty redis command");
            encoded += encodeCommand(args);
        }
        std::vector<RedisValue> replies;
        replies.reserve(commands.size());
        withRetry([&](Connection &connection) -> RedisValue {
            replies.clear(); // a retry must not append to the previous attempt's partial results
            connection.sendAll(encoded);
            for (size_t index = 0; index < commands.size(); ++index) replies.push_back(connection.readReply());
            return RedisValue::nil();
        });
        return replies;
    }

    bool SocketRedisClient::healthy() {
        try {
            return command({"PING"}).isString();
        } catch (const std::exception &) {
            return false;
        }
    }

} // namespace sapo::redis
