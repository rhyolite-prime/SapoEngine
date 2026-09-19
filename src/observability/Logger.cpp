#include "observability/Logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace sapo::obs {

    namespace {
        std::string iso8601(int64_t epoch_ms) {
            const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
            std::tm tm_utc{};
#if defined(_WIN32)
            gmtime_s(&tm_utc, &seconds);
#else
            gmtime_r(&seconds, &tm_utc);
#endif
            std::ostringstream out;
            out << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3)
                << std::setfill('0') << (epoch_ms % 1000) << 'Z';
            return out.str();
        }
    } // namespace

    const char *toString(LogLevel level) {
        switch (level) {
            case LogLevel::Trace: return "trace";
            case LogLevel::Debug: return "debug";
            case LogLevel::Info: return "info";
            case LogLevel::Warn: return "warn";
            case LogLevel::Error: return "error";
            case LogLevel::Off: return "off";
        }
        return "info";
    }

    bool levelAtLeast(LogLevel configured, LogLevel message) {
        return message >= configured && configured != LogLevel::Off;
    }

    void ConsoleSink::write(const LogRecord &record) {
        std::ostream &out = (record.level >= LogLevel::Warn) ? std::cerr : std::cout;
        out << "[sapo][" << toString(record.level) << "]";
        if (!record.component.empty()) out << "[" << record.component << "]";
        out << " " << record.message;
        if (record.fields.is_object() && !record.fields.empty()) {
            out << "  " << record.fields.dump();
        }
        out << "\n";
        out.flush();
    }

    void JsonLineSink::write(const LogRecord &record) {
        if (m_stream == nullptr) return;
        nlohmann::json j;
        j["ts"] = iso8601(record.timestamp_ms);
        j["level"] = toString(record.level);
        if (!record.component.empty()) j["component"] = record.component;
        j["msg"] = record.message;
        if (!record.execution_id.empty()) j["execution_id"] = record.execution_id;
        if (!record.node_id.empty()) j["node_id"] = record.node_id;
        if (record.fields.is_object() && !record.fields.empty()) j["fields"] = record.fields;
        if (m_stream) {
            (*m_stream) << j.dump() << "\n";
            m_stream->flush();
        }
    }

    void MemorySink::write(const LogRecord &record) {
        std::scoped_lock lock(m_mutex);
        m_records.push_back(record);
    }

    std::vector<LogRecord> MemorySink::records() const {
        std::scoped_lock lock(m_mutex);
        return m_records;
    }

    size_t MemorySink::size() const {
        std::scoped_lock lock(m_mutex);
        return m_records.size();
    }

    void MemorySink::clear() {
        std::scoped_lock lock(m_mutex);
        m_records.clear();
    }

    bool MemorySink::contains(const std::string &needle) const {
        std::scoped_lock lock(m_mutex);
        return std::any_of(m_records.begin(), m_records.end(),
                           [&](const LogRecord &r) { return r.message.find(needle) != std::string::npos; });
    }

    std::shared_ptr<Logger> Logger::withComponent(std::string component) const {
        auto child = std::make_shared<Logger>();
        child->m_level = m_level;
        {
            std::scoped_lock lock(m_mutex);
            child->m_sinks = m_sinks;
            child->m_secrets = m_secrets;
        }
        if (!component.empty()) {
            // Component names are decorative: the child keeps the parent's sinks.
        }
        return child;
    }

    void Logger::addSink(std::shared_ptr<ILogSink> sink) {
        if (!sink) return;
        std::scoped_lock lock(m_mutex);
        m_sinks.push_back(std::move(sink));
    }

    void Logger::clearSinks() {
        std::scoped_lock lock(m_mutex);
        m_sinks.clear();
    }

    void Logger::addSecret(const std::string &value) {
        if (value.empty() || value.size() < 3) return; // too small → mass redaction
        std::scoped_lock lock(m_mutex);
        m_secrets.push_back(value);
    }

    void Logger::clearSecrets() {
        std::scoped_lock lock(m_mutex);
        m_secrets.clear();
    }

    std::string Logger::redact(std::string_view text) const {
        std::string out(text);
        std::vector<std::string> secrets;
        {
            std::scoped_lock lock(m_mutex);
            secrets = m_secrets;
        }
        for (const auto &secret : secrets) {
            static constexpr std::string_view kMask = "***REDACTED***";
            size_t pos = 0;
            while ((pos = out.find(secret, pos)) != std::string::npos) {
                out.replace(pos, secret.size(), kMask);
                pos += kMask.size();
            }
        }
        return out;
    }

    void Logger::log(LogLevel level, const std::string &component, const std::string &message,
                     const nlohmann::json &fields, const std::string &execution_id,
                     const std::string &node_id) const {
        if (!levelAtLeast(m_level, level)) return;

        LogRecord record;
        record.level = level;
        record.component = component;
        record.message = redact(message);
        record.execution_id = execution_id;
        record.node_id = node_id;
        record.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
        if (fields.is_object()) {
            nlohmann::json redacted = nlohmann::json::object();
            for (auto it = fields.begin(); it != fields.end(); ++it) {
                if (it.value().is_string()) {
                    redacted[it.key()] = redact(it.value().get<std::string>());
                } else {
                    redacted[it.key()] = it.value();
                }
            }
            record.fields = std::move(redacted);
        }

        std::vector<std::shared_ptr<ILogSink>> sinks;
        {
            std::scoped_lock lock(m_mutex);
            sinks = m_sinks;
        }
        if (sinks.empty()) {
            static ConsoleSink fallback;
            fallback.write(record);
            return;
        }
        for (const auto &sink : sinks) sink->write(record);
    }

    std::shared_ptr<Logger> Logger::defaultLogger() {
        static std::shared_ptr<Logger> logger = [] {
            auto l = std::make_shared<Logger>();
            l->addSink(std::make_shared<ConsoleSink>());
            return l;
        }();
        return logger;
    }

    static std::shared_ptr<Logger> g_global_logger;

    void Logger::setGlobal(const std::shared_ptr<Logger> &logger) { g_global_logger = logger; }

    std::shared_ptr<Logger> Logger::global() {
        static std::once_flag once;
        std::call_once(once, [] { g_global_logger = Logger::defaultLogger(); });
        return g_global_logger ? g_global_logger : Logger::defaultLogger();
    }

    LoggerPtr defaultLogger() { return Logger::global(); }

} // namespace sapo::obs
