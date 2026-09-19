#include "observability/Metrics.hpp"
#include "observability/Logger.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace sapo::obs {

    void MetricsRegistry::increment(const std::string &name, double delta) {
        std::scoped_lock lock(m_mutex);
        m_counters[name] += delta;
    }

    void MetricsRegistry::setGauge(const std::string &name, double value) {
        std::scoped_lock lock(m_mutex);
        m_gauges[name] = value;
    }

    void MetricsRegistry::observe(const std::string &name, double value_ms) {
        std::scoped_lock lock(m_mutex);
        auto &samples = m_samples[name];
        samples.push_back(value_ms);
        if (samples.size() > kMaxSamples) {
            // Bounded reservoir: drop the oldest half rather than growing forever.
            samples.erase(samples.begin(), samples.begin() + samples.size() / 2);
        }
        m_counters[name + ".count"] += 1.0;
        m_counters[name + ".sum"] += value_ms;
    }

    double MetricsRegistry::counter(const std::string &name) const {
        std::scoped_lock lock(m_mutex);
        auto it = m_counters.find(name);
        return it == m_counters.end() ? 0.0 : it->second;
    }

    double MetricsRegistry::gauge(const std::string &name) const {
        std::scoped_lock lock(m_mutex);
        auto it = m_gauges.find(name);
        return it == m_gauges.end() ? 0.0 : it->second;
    }

    MetricsRegistry::Summary MetricsRegistry::summary(const std::string &name) const {
        std::scoped_lock lock(m_mutex);
        Summary out;
        auto it = m_samples.find(name);
        if (it == m_samples.end() || it->second.empty()) return out;
        const auto json = summarizeLocked(it->second);
        out.count = json["count"].get<size_t>();
        out.total = json["total_ms"].get<double>();
        out.p50 = json["p50_ms"].get<double>();
        out.p99 = json["p99_ms"].get<double>();
        out.max = json["max_ms"].get<double>();
        return out;
    }

    nlohmann::json MetricsRegistry::toJson() const {
        std::scoped_lock lock(m_mutex);
        nlohmann::json counters = nlohmann::json::object();
        for (const auto &[k, v] : m_counters) counters[k] = v;
        nlohmann::json gauges = nlohmann::json::object();
        for (const auto &[k, v] : m_gauges) gauges[k] = v;
        nlohmann::json histograms = nlohmann::json::object();
        for (const auto &[name, samples] : m_samples) {
            histograms[name] = summarizeLocked(samples);
        }
        return {{"counters", std::move(counters)},
                {"gauges", std::move(gauges)},
                {"histograms", std::move(histograms)}};
    }

    nlohmann::json MetricsRegistry::summarizeLocked(const std::vector<double> &raw) {
        nlohmann::json out = {{"count", 0}, {"total_ms", 0.0}, {"p50_ms", 0.0}, {"p99_ms", 0.0}, {"max_ms", 0.0}};
        if (raw.empty()) return out;
        std::vector<double> sorted = raw;
        std::sort(sorted.begin(), sorted.end());
        double total = 0.0;
        for (double v : sorted) total += v;
        auto pct = [&sorted](double p) {
            size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1) + 0.5);
            return sorted[std::min(idx, sorted.size() - 1)];
        };
        out["count"] = sorted.size();
        out["total_ms"] = total;
        out["p50_ms"] = pct(0.50);
        out["p99_ms"] = pct(0.99);
        out["max_ms"] = sorted.back();
        return out;
    }

    std::string MetricsRegistry::toPrometheus() const {
        nlohmann::json j = toJson();
        std::ostringstream out;
        auto sanitize = [](std::string s) {
            std::replace_if(
                s.begin(), s.end(),
                [](char c) { return !(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':'); },
                '_');
            return s;
        };
        for (auto it = j["counters"].begin(); it != j["counters"].end(); ++it) {
            out << "# TYPE " << sanitize(it.key()) << " counter\n"
                << sanitize(it.key()) << " " << it.value() << "\n";
        }
        for (auto it = j["gauges"].begin(); it != j["gauges"].end(); ++it) {
            out << "# TYPE " << sanitize(it.key()) << " gauge\n"
                << sanitize(it.key()) << " " << it.value() << "\n";
        }
        for (auto it = j["histograms"].begin(); it != j["histograms"].end(); ++it) {
            out << "# TYPE " << sanitize(it.key()) << " summary\n"
                << sanitize(it.key()) << "{quantile=\"0.5\"} " << it.value()["p50_ms"] << "\n"
                << sanitize(it.key()) << "{quantile=\"0.99\"} " << it.value()["p99_ms"] << "\n"
                << sanitize(it.key()) << "_count " << it.value()["count"] << "\n"
                << sanitize(it.key()) << "_sum " << it.value()["total_ms"] << "\n";
        }
        return out.str();
    }

    void MetricsRegistry::reset() {
        std::scoped_lock lock(m_mutex);
        m_counters.clear();
        m_gauges.clear();
        m_samples.clear();
    }

    std::shared_ptr<MetricsRegistry> MetricsRegistry::global() {
        static std::shared_ptr<MetricsRegistry> registry = std::make_shared<MetricsRegistry>();
        return registry;
    }

    TraceRecorder::TraceRecorder(std::shared_ptr<Logger> logger, size_t ring_capacity)
        : m_logger(std::move(logger)), m_capacity(ring_capacity) {}

    void TraceRecorder::begin(TraceSpan span) {
        std::scoped_lock lock(m_mutex);
        m_open.push_back(std::move(span));
    }

    void TraceRecorder::end(const std::string &execution_id, const std::string &node_id,
                            const std::string &outcome, const std::string &error_code,
                            const std::string &error_message) {
        TraceSpan completed;
        bool found = false;
        {
            std::scoped_lock lock(m_mutex);
            for (auto it = m_open.rbegin(); it != m_open.rend(); ++it) {
                if (it->execution_id == execution_id && it->node_id == node_id) {
                    completed = *it;
                    m_open.erase(std::next(it).base());
                    found = true;
                    break;
                }
            }
        }
        if (!found) return;

        if (m_logger) {
            completed.outcome = outcome;
            completed.error_code = error_code;
            completed.error_message = error_message;
            m_logger->log(LogLevel::Trace, "trace", "node:" + completed.node_type,
                          nlohmann::json{
                              {"node", completed.node_id},
                              {"type", completed.node_type},
                              {"outcome", completed.outcome},
                              {"duration_ms", completed.duration_ms},
                              {"depth", completed.depth},
                              {"error_code", error_code.empty() ? nullptr : nlohmann::json(error_code)},
                              {"error", error_message.empty() ? nullptr : nlohmann::json(error_message)},
                          },
                          execution_id, node_id);
        }

        std::scoped_lock lock(m_mutex);
        m_completed.push_back(std::move(completed));
        if (m_completed.size() > m_capacity) {
            m_completed.erase(m_completed.begin(), m_completed.begin() + m_capacity / 2);
        }
    }

    std::vector<TraceSpan> TraceRecorder::spans() const {
        std::scoped_lock lock(m_mutex);
        return m_completed;
    }

    std::vector<TraceSpan> TraceRecorder::spansFor(const std::string &execution_id) const {
        std::scoped_lock lock(m_mutex);
        std::vector<TraceSpan> out;
        for (const auto &span : m_completed) {
            if (span.execution_id == execution_id) out.push_back(span);
        }
        return out;
    }

    void TraceRecorder::clear() {
        std::scoped_lock lock(m_mutex);
        m_open.clear();
        m_completed.clear();
    }

} // namespace sapo::obs
