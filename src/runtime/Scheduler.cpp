//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//
#include "runtime/Scheduler.hpp"
#include "runtime/SapoError.hpp"
#include "util/Crypto.hpp"
#include "util/TimeUtils.hpp"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

namespace sapo::runtime {

    namespace {

        std::string trim(const std::string &value) {
            size_t begin = 0;
            size_t end = value.size();
            while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
            while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
            return value.substr(begin, end - begin);
        }

        std::string lower(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        bool isAllStar(const std::set<int> &values, int min_value, int max_value) {
            if (static_cast<int>(values.size()) != max_value - min_value + 1) return false;
            return *values.begin() == min_value && *values.rbegin() == max_value;
        }

        int offsetFor(const std::string &timezone) {
            if (timezone.empty()) return 0;
            auto offset = util::parseZoneOffset(timezone);
            return offset.has_value() ? offset->seconds : 0;
        }

        const std::array<const char *, 12> kMonths = {"jan", "feb", "mar", "apr", "may", "jun",
                                                      "jul", "aug", "sep", "oct", "nov", "dec"};
        const std::array<const char *, 7> kWeekdays = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};

    } // namespace

    // -------------------------------------------------------------------------
    // CronExpression
    // -------------------------------------------------------------------------
    bool CronExpression::parseField(const std::string &field, int min_value, int max_value, bool is_dow,
                                    std::set<int> &out, std::string *error, bool *restricted) {
        auto named = [&](const std::string &token, int &value) -> bool {
            const std::string text = lower(token);
            if (is_dow) {
                for (size_t i = 0; i < kWeekdays.size(); ++i) {
                    if (text == kWeekdays[i]) {
                        value = static_cast<int>(i);
                        return true;
                    }
                }
            } else if (max_value == 12) {
                for (size_t i = 0; i < kMonths.size(); ++i) {
                    if (text == kMonths[i]) {
                        value = static_cast<int>(i) + 1;
                        return true;
                    }
                }
            }
            try {
                size_t consumed = 0;
                int parsed = std::stoi(token, &consumed);
                if (consumed != token.size()) return false;
                value = parsed;
                return true;
            } catch (...) {
                return false;
            }
        };

        const std::string text = trim(field);
        if (text.empty()) {
            if (error != nullptr) *error = "empty field";
            return false;
        }
        if (restricted != nullptr) *restricted = true;
        if (text == "*" || text == "?") {
            for (int v = min_value; v <= max_value; ++v) out.insert(v);
            if (restricted != nullptr) *restricted = false;
            return true;
        }

        for (const auto &raw_item : [&] {
                 std::vector<std::string> items;
                 std::stringstream stream(text);
                 std::string item;
                 while (std::getline(stream, item, ',')) items.push_back(trim(item));
                 return items;
             }()) {
            std::string item = raw_item;
            int step = 1;
            bool had_step = false;
            if (auto slash = item.find('/'); slash != std::string::npos) {
                had_step = true;
                const std::string step_text = trim(item.substr(slash + 1));
                item = trim(item.substr(0, slash));
                try {
                    step = std::stoi(step_text);
                } catch (...) {
                    if (error != nullptr) *error = "bad step in '" + raw_item + "'";
                    return false;
                }
                if (step <= 0) {
                    if (error != nullptr) *error = "step must be >= 1 in '" + raw_item + "'";
                    return false;
                }
            }
            if (item.empty()) {
                if (error != nullptr) *error = "empty range in '" + raw_item + "'";
                return false;
            }

            int begin = min_value;
            int end = max_value;
            if (item != "*" && item != "?") {
                if (auto dash = item.find('-'); dash != std::string::npos) {
                    if (!named(trim(item.substr(0, dash)), begin) || !named(trim(item.substr(dash + 1)), end)) {
                        if (error != nullptr) *error = "bad range '" + item + "'";
                        return false;
                    }
                } else if (!named(item, begin)) {
                    if (error != nullptr) *error = "bad value '" + item + "'";
                    return false;
                } else if (!had_step) {
                    end = begin;
                }
            }
            if (is_dow && end == 7) end = 0; // 7 == Sunday
            if (begin < min_value || end > max_value || begin < min_value) {
                if (error != nullptr) {
                    *error = "value out of range in '" + item + "' (allowed " + std::to_string(min_value) + "-" +
                             std::to_string(max_value) + ")";
                }
                return false;
            }
            // "0-0/5" style wrap (e.g. fri-mon) is not supported; reject loudly.
            if (begin > end) {
                if (error != nullptr) *error = "reversed range '" + item + "' is not supported";
                return false;
            }
            for (int value = begin; value <= end; value += step) out.insert(value);
        }
        if (out.empty()) {
            if (error != nullptr) *error = "field selects nothing";
            return false;
        }
        return true;
    }

    std::optional<CronExpression> CronExpression::parse(const std::string &text, std::string *error) {
        const std::string trimmed = trim(text);
        if (trimmed.empty()) {
            if (error != nullptr) *error = "empty schedule";
            return std::nullopt;
        }

        CronExpression expression;
        expression.m_text = trimmed;

        // Relative one-shots / intervals: "in 5 minutes", "every 30m", "5m".
        if (trimmed.front() == 'i' || trimmed.rfind("every ", 0) == 0) {
            const std::string tail = trimmed.rfind("every ", 0) == 0 ? lower(trimmed.substr(6))
                                                                     : lower(trimmed.substr(trimmed.find(' ') + 1));
            auto delay = util::parseRelativeDelay(trimmed);
            if (!delay.has_value()) delay = util::parseDuration(tail);
            if (!delay.has_value() || delay->count() <= 0) {
                if (error != nullptr) *error = "cannot read the delay in '" + trimmed + "'";
                return std::nullopt;
            }
            expression.m_kind = Kind::Relative;
            expression.m_interval_ms = delay->count();
            expression.m_one_shot = trimmed.rfind("in ", 0) == 0;
            return expression;
        }
        if (trimmed.front() == '@') {
            const std::string macro = lower(trimmed);
            if (macro == "@yearly" || macro == "@annually") {
                expression.m_minute = {0};
                expression.m_hour = {0};
                expression.m_dom = {1};
                expression.m_month = {1};
                expression.m_dow = {0, 1, 2, 3, 4, 5, 6};
                expression.m_dom_restricted = true;
                return expression;
            }
            if (macro == "@monthly") {
                expression.m_minute = {0};
                expression.m_hour = {0};
                expression.m_dom = {1};
                expression.m_month = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
                expression.m_dow = {0, 1, 2, 3, 4, 5, 6};
                expression.m_dom_restricted = true;
                return expression;
            }
            if (macro == "@weekly") {
                expression.m_minute = {0};
                expression.m_hour = {0};
                expression.m_dom = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
                                    23, 24, 25, 26, 27, 28, 29, 30, 31};
                expression.m_month = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
                expression.m_dow = {0};
                expression.m_dow_restricted = true;
                return expression;
            }
            if (macro == "@daily" || macro == "@midnight") {
                expression.m_minute = {0};
                expression.m_hour = {0};
                expression.m_dom = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
                                    23, 24, 25, 26, 27, 28, 29, 30, 31};
                expression.m_month = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
                expression.m_dow = {0, 1, 2, 3, 4, 5, 6};
                return expression;
            }
            if (macro == "@hourly") {
                expression.m_minute = {0};
                for (int hour = 0; hour < 24; ++hour) expression.m_hour.insert(hour);
                expression.m_dom = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
                                    23, 24, 25, 26, 27, 28, 29, 30, 31};
                expression.m_month = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
                expression.m_dow = {0, 1, 2, 3, 4, 5, 6};
                return expression;
            }
            if (macro == "@minutely" || macro == "@every_minute") {
                for (int minute = 0; minute < 60; ++minute) expression.m_minute.insert(minute);
                for (int hour = 0; hour < 24; ++hour) expression.m_hour.insert(hour);
                expression.m_dom = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
                                    23, 24, 25, 26, 27, 28, 29, 30, 31};
                expression.m_month = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
                expression.m_dow = {0, 1, 2, 3, 4, 5, 6};
                return expression;
            }
            if (error != nullptr) *error = "unknown schedule macro '" + trimmed + "'";
            return std::nullopt;
        }

        std::vector<std::string> fields;
        {
            std::stringstream stream(trimmed);
            std::string token;
            while (stream >> token) fields.push_back(token);
        }
        if (fields.size() < 5 || fields.size() > 6) {
            if (error != nullptr) {
                *error = "expected 5 (or 6 with year) cron fields, got " + std::to_string(fields.size());
            }
            return std::nullopt;
        }
        bool minute_restricted = false;
        if (!parseField(fields[0], 0, 59, false, expression.m_minute, error, &minute_restricted)) return std::nullopt;
        if (!parseField(fields[1], 0, 23, false, expression.m_hour, error)) return std::nullopt;
        if (!parseField(fields[2], 1, 31, false, expression.m_dom, error, &expression.m_dom_restricted))
            return std::nullopt;
        if (!parseField(fields[3], 1, 12, false, expression.m_month, error)) return std::nullopt;
        if (!parseField(fields[4], 0, 7, true, expression.m_dow, error, &expression.m_dow_restricted))
            return std::nullopt;
        // Normalise Sunday written as 7 into the same set as 0.
        if (expression.m_dow.count(7)) {
            expression.m_dow.erase(7);
            expression.m_dow.insert(0);
        }
        if (fields.size() == 6) {
            if (!parseField(fields[5], 1970, 9999, false, expression.m_year, error)) return std::nullopt;
        }
        (void) minute_restricted;
        if (isAllStar(expression.m_minute, 0, 59) && isAllStar(expression.m_hour, 0, 23)) {
            // Valid but degenerate: fires every minute. Allowed, deliberately.
        }
        return expression;
    }

    bool CronExpression::matches(int year, int month, int day, int weekday, int hour, int minute) const {
        if (!m_year.empty() && !m_year.count(year)) return false;
        if (!m_month.count(month)) return false;
        if (!m_hour.count(hour)) return false;
        if (!m_minute.count(minute)) return false;
        const bool dom_ok = m_dom.count(day) != 0;
        const bool dow_ok = m_dow.count(weekday) != 0;
        // Vixie-cron rule: when both day fields are restricted, either may match.
        if (m_dom_restricted && m_dow_restricted) return dom_ok || dow_ok;
        return dom_ok && dow_ok;
    }

    bool CronExpression::dayMatches(int year, int month, int day, int weekday) const {
        if (!m_month.count(month)) return false;
        if (!m_year.empty() && !m_year.count(year)) return false;
        const bool dom_ok = m_dom.count(day) != 0;
        const bool dow_ok = m_dow.count(weekday) != 0;
        // Vixie-cron rule: when both day fields are restricted, either may match.
        if (m_dom_restricted && m_dow_restricted) return dom_ok || dow_ok;
        return dom_ok && dow_ok;
    }

    std::optional<int64_t> CronExpression::nextAfter(int64_t epoch_ms, int offset_seconds) const {
        if (m_kind == Kind::Relative) return epoch_ms + m_interval_ms;

        constexpr int64_t kMinute = 60LL * 1000LL;
        constexpr int64_t kDay = 24LL * 60LL * 60LL * 1000LL;
        const auto start = util::breakDown(epoch_ms, offset_seconds);
        const int64_t day_start_ms =
            util::compose(util::BrokenDownTime{start.year, start.month, start.day, 0, 0, 0}, offset_seconds);
        const int64_t first_minute = (epoch_ms - day_start_ms) / kMinute;

        for (int day_offset = 0; day_offset <= 1500; ++day_offset) {
            const int64_t day_ms = day_start_ms + day_offset * kDay;
            const auto broken = util::breakDown(day_ms, offset_seconds);
            if (!m_year.empty() && broken.year > *m_year.rbegin()) return std::nullopt; // past the last legal year
            if (!dayMatches(broken.year, broken.month, broken.day, broken.weekday)) continue;

            for (int hour : m_hour) {
                for (int minute : m_minute) {
                    const int64_t minute_of_day = static_cast<int64_t>(hour) * 60 + minute;
                    if (day_offset == 0 && minute_of_day <= first_minute) continue;
                    return day_ms + minute_of_day * kMinute;
                }
            }
        }
        return std::nullopt;
    }

    bool isValidCronExpression(const std::string &text) {
        std::string error;
        return CronExpression::parse(text, &error).has_value();
    }

    std::optional<int64_t> nextScheduledTime(const std::string &text, int64_t from_ms, const std::string &timezone,
                                              std::string *error) {
        auto expression = CronExpression::parse(text, error);
        if (!expression.has_value()) return std::nullopt;
        auto next = expression->nextAfter(from_ms, offsetFor(timezone));
        if (!next.has_value() && error != nullptr) *error = "schedule '" + text + "' has no future occurrence";
        return next;
    }

    // -------------------------------------------------------------------------
    // ScheduledJob / Timer serialisation
    // -------------------------------------------------------------------------
    json ScheduledJob::toJson() const {
        return json{{"id", id},
                    {"schedule", schedule},
                    {"timezone", timezone},
                    {"workflow", workflow},
                    {"node_id", node_id},
                    {"entry_node", entry_node},
                    {"input", input},
                    {"enabled", enabled},
                    {"catch_up", catch_up},
                    {"max_catch_up", max_catch_up},
                    {"next_fire_ms", next_fire_ms},
                    {"last_fire_ms", last_fire_ms},
                    {"fire_count", fire_count}};
    }

    ScheduledJob ScheduledJob::fromJson(const json &json_value) {
        ScheduledJob job;
        job.id = json_value.value("id", "");
        job.schedule = json_value.value("schedule", "");
        job.timezone = json_value.value("timezone", "UTC");
        job.workflow = json_value.value("workflow", "");
        job.node_id = json_value.value("node_id", "");
        job.entry_node = json_value.value("entry_node", "");
        if (json_value.contains("input")) job.input = json_value["input"];
        job.enabled = json_value.value("enabled", true);
        job.catch_up = json_value.value("catch_up", false);
        job.max_catch_up = json_value.value("max_catch_up", 5);
        job.next_fire_ms = json_value.value("next_fire_ms", 0LL);
        job.last_fire_ms = json_value.value("last_fire_ms", 0LL);
        job.fire_count = json_value.value("fire_count", 0ULL);
        return job;
    }

    json Timer::toJson() const {
        return json{{"id", id},
                    {"session_id", session_id},
                    {"due_ms", due_ms},
                    {"resume_node", resume_node},
                    {"reason", reason},
                    {"context_checkpoint_key", context_checkpoint_key}};
    }

    Timer Timer::fromJson(const json &json_value) {
        Timer timer;
        timer.id = json_value.value("id", "");
        timer.session_id = json_value.value("session_id", "");
        timer.due_ms = json_value.value("due_ms", 0LL);
        timer.resume_node = json_value.value("resume_node", "");
        timer.reason = json_value.value("reason", "");
        timer.context_checkpoint_key = json_value.value("context_checkpoint_key", "");
        return timer;
    }

    // -------------------------------------------------------------------------
    // Scheduler
    // -------------------------------------------------------------------------
    Scheduler::Scheduler(ClockPtr clock) : m_clock(clock ? std::move(clock) : defaultClock()) {}

    Scheduler::~Scheduler() { stop(); }

    void Scheduler::rescheduleLocked(ScheduledJob &job, int64_t from_ms) {
        std::string error;
        auto expression = CronExpression::parse(job.schedule, &error);
        if (!expression.has_value()) {
            job.enabled = false;
            job.next_fire_ms = 0;
            return;
        }
        auto next = expression->nextAfter(from_ms, offsetFor(job.timezone));
        if (!next.has_value()) {
            job.enabled = false;
            job.next_fire_ms = 0;
            return;
        }
        job.next_fire_ms = *next;
    }

    std::string Scheduler::addJob(ScheduledJob job) {
        std::string error;
        if (!CronExpression::parse(job.schedule, &error).has_value()) {
            throw SapoError(ErrorCode::Parse, "invalid schedule '" + job.schedule + "': " + error,
                            json{{"schedule", job.schedule}}, job.node_id);
        }
        if (job.id.empty()) job.id = "job_" + util::randomHex(6);
        const int64_t now = m_clock->now().count();
        if (job.next_fire_ms <= now) rescheduleLocked(job, now);
        std::scoped_lock lock(m_mutex);
        m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(),
                                    [&](const ScheduledJob &existing) { return existing.id == job.id; }),
                     m_jobs.end());
        m_jobs.push_back(std::move(job));
        return m_jobs.back().id;
    }

    bool Scheduler::removeJob(const std::string &id) {
        std::scoped_lock lock(m_mutex);
        auto before = m_jobs.size();
        m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(),
                                    [&](const ScheduledJob &job) { return job.id == id; }),
                     m_jobs.end());
        return m_jobs.size() != before;
    }

    bool Scheduler::enableJob(const std::string &id, bool enabled) {
        std::scoped_lock lock(m_mutex);
        for (auto &job : m_jobs) {
            if (job.id != id) continue;
            job.enabled = enabled;
            if (enabled) rescheduleLocked(job, m_clock->now().count());
            return true;
        }
        return false;
    }

    std::optional<ScheduledJob> Scheduler::job(const std::string &id) const {
        std::scoped_lock lock(m_mutex);
        for (const auto &job : m_jobs) {
            if (job.id == id) return job;
        }
        return std::nullopt;
    }

    std::vector<ScheduledJob> Scheduler::jobs() const {
        std::scoped_lock lock(m_mutex);
        return m_jobs;
    }

    std::string Scheduler::addTimer(Timer timer) {
        if (timer.id.empty()) timer.id = "timer_" + util::randomHex(6);
        std::scoped_lock lock(m_mutex);
        m_timers.erase(std::remove_if(m_timers.begin(), m_timers.end(),
                                      [&](const Timer &existing) { return existing.id == timer.id; }),
                       m_timers.end());
        m_timers.push_back(std::move(timer));
        std::sort(m_timers.begin(), m_timers.end(), [](const Timer &a, const Timer &b) { return a.due_ms < b.due_ms; });
        return m_timers.back().id;
    }

    bool Scheduler::cancelTimer(const std::string &id) {
        std::scoped_lock lock(m_mutex);
        auto before = m_timers.size();
        m_timers.erase(std::remove_if(m_timers.begin(), m_timers.end(),
                                      [&](const Timer &timer) { return timer.id == id; }),
                       m_timers.end());
        return m_timers.size() != before;
    }

    bool Scheduler::cancelTimersForSession(const std::string &session_id) {
        std::scoped_lock lock(m_mutex);
        auto before = m_timers.size();
        m_timers.erase(std::remove_if(m_timers.begin(), m_timers.end(),
                                      [&](const Timer &timer) { return timer.session_id == session_id; }),
                       m_timers.end());
        return m_timers.size() != before;
    }

    std::optional<Timer> Scheduler::timer(const std::string &id) const {
        std::scoped_lock lock(m_mutex);
        for (const auto &timer : m_timers) {
            if (timer.id == id) return timer;
        }
        return std::nullopt;
    }

    std::vector<Timer> Scheduler::timers() const {
        std::scoped_lock lock(m_mutex);
        return m_timers;
    }

    std::vector<Timer> Scheduler::timersForSession(const std::string &session_id) const {
        std::scoped_lock lock(m_mutex);
        std::vector<Timer> out;
        for (const auto &timer : m_timers) {
            if (timer.session_id == session_id) out.push_back(timer);
        }
        return out;
    }

    size_t Scheduler::tick() { return tickUntil(m_clock->now().count()); }

    size_t Scheduler::tickUntil(int64_t now_ms) {
        std::vector<std::pair<ScheduledJob, int64_t>> due_jobs;
        std::vector<Timer> due_timers;
        {
            std::scoped_lock lock(m_mutex);
            for (auto &job : m_jobs) {
                if (!job.enabled || job.next_fire_ms <= 0 || job.next_fire_ms > now_ms) continue;
                auto expression = CronExpression::parse(job.schedule);
                const int offset = offsetFor(job.timezone);
                if (!expression.has_value()) {
                    job.enabled = false;
                    continue;
                }
                ScheduledJob fired = job;
                int64_t cursor = job.next_fire_ms;
                const int limit = job.catch_up ? std::max(1, job.max_catch_up) : 1;
                for (int count = 0; count < limit; ++count) {
                    due_jobs.emplace_back(fired, cursor);
                    auto next = expression->nextAfter(cursor, offset);
                    if (!next.has_value()) break;
                    cursor = *next;
                    if (cursor > now_ms) break;
                }
                job.last_fire_ms = now_ms;
                ++job.fire_count;
                rescheduleLocked(job, now_ms);
            }

            std::vector<Timer> remaining;
            remaining.reserve(m_timers.size());
            for (auto &timer : m_timers) {
                if (timer.due_ms <= now_ms) due_timers.push_back(timer);
                else remaining.push_back(timer);
            }
            m_timers.swap(remaining);
        }

        // "how many things came due", independent of whether a listener exists:
        // a job with no subscriber still fired (and was rescheduled).
        const size_t fired = due_jobs.size() + due_timers.size();
        for (auto &[job, fired_at] : due_jobs) {
            if (m_job_callback) m_job_callback(job, fired_at);
        }
        for (const auto &timer : due_timers) {
            if (m_timer_callback) m_timer_callback(timer);
        }
        return fired;
    }

    int64_t Scheduler::nextWakeMs() const {
        std::scoped_lock lock(m_mutex);
        int64_t next = 0;
        for (const auto &job : m_jobs) {
            if (!job.enabled || job.next_fire_ms <= 0) continue;
            if (next == 0 || job.next_fire_ms < next) next = job.next_fire_ms;
        }
        for (const auto &timer : m_timers) {
            if (next == 0 || timer.due_ms < next) next = timer.due_ms;
        }
        return next;
    }

    size_t Scheduler::pendingCount() const {
        std::scoped_lock lock(m_mutex);
        size_t count = m_timers.size();
        for (const auto &job : m_jobs) {
            if (job.enabled) ++count;
        }
        return count;
    }

    void Scheduler::start(std::chrono::milliseconds poll_interval) {
        bool expected = false;
        if (!m_running.compare_exchange_strong(expected, true)) return;
        m_thread = std::make_unique<std::thread>([this, poll_interval] {
            while (m_running.load()) {
                tick();
                std::this_thread::sleep_for(poll_interval);
            }
        });
    }

    void Scheduler::stop() {
        if (!m_running.exchange(false)) {
            if (m_thread && m_thread->joinable()) m_thread->join();
            m_thread.reset();
            return;
        }
        if (m_thread && m_thread->joinable()) m_thread->join();
        m_thread.reset();
    }

    bool Scheduler::running() const { return m_running.load(); }

    json Scheduler::toJson() const {
        std::scoped_lock lock(m_mutex);
        json jobs = json::array();
        for (const auto &job : m_jobs) jobs.push_back(job.toJson());
        json timers = json::array();
        for (const auto &timer : m_timers) timers.push_back(timer.toJson());
        return json{{"version", 1}, {"jobs", jobs}, {"timers", timers}};
    }

    bool Scheduler::saveToFile(const std::string &path) const {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << toJson().dump(2);
        return static_cast<bool>(out);
    }

    bool Scheduler::loadFromFile(const std::string &path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::stringstream buffer;
        buffer << in.rdbuf();
        json document;
        try {
            document = json::parse(buffer.str());
        } catch (const std::exception &) {
            return false;
        }
        std::scoped_lock lock(m_mutex);
        m_jobs.clear();
        m_timers.clear();
        if (document.contains("jobs") && document["jobs"].is_array()) {
            for (const auto &entry : document["jobs"]) m_jobs.push_back(ScheduledJob::fromJson(entry));
        }
        if (document.contains("timers") && document["timers"].is_array()) {
            for (const auto &entry : document["timers"]) m_timers.push_back(Timer::fromJson(entry));
        }
        const int64_t now = m_clock->now().count();
        for (auto &job : m_jobs) {
            if (job.enabled && job.next_fire_ms <= 0) rescheduleLocked(job, now);
        }
        return true;
    }

} // namespace sapo::runtime
