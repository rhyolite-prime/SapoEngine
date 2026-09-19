#include "util/TimeUtils.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace sapo::util {

    namespace {

        std::string trim(std::string_view s) {
            size_t b = 0, e = s.size();
            while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
            while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
            return std::string(s.substr(b, e - b));
        }

        std::string lower(std::string s) {
            for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        /// Converts a `timegm`-style struct to epoch seconds portably.
        time_t toEpoch(const std::tm &tm_in) {
            std::tm tm_copy = tm_in;
#if defined(_WIN32)
            return _mkgmtime(&tm_copy);
#else
            return timegm(&tm_copy);
#endif
        }

    } // namespace

    std::optional<std::chrono::milliseconds> parseDuration(std::string_view raw_text) {
        const std::string text = lower(trim(raw_text));
        if (text.empty()) return std::nullopt;

        // Bare number => seconds (matches the BNF's friendly shorthand).
        bool all_digits = std::all_of(text.begin(), text.end(),
                                      [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.'; });
        if (all_digits && text.find_first_of('.') == std::string::npos) {
            try {
                return std::chrono::milliseconds(static_cast<int64_t>(std::stoll(text)) * 1000);
            } catch (...) {
                return std::nullopt;
            }
        }
        if (all_digits) {
            try {
                return std::chrono::milliseconds(static_cast<int64_t>(std::stod(text) * 1000.0));
            } catch (...) {
                return std::nullopt;
            }
        }

        std::chrono::milliseconds total{0};
        size_t i = 0;
        bool consumed_any = false;
        while (i < text.size()) {
            size_t start = i;
            while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '.')) ++i;
            if (start == i) return std::nullopt; // no digits where expected
            const double magnitude = std::stod(text.substr(start, i - start));

            size_t unit_start = i;
            while (i < text.size() && std::isalpha(static_cast<unsigned char>(text[i]))) ++i;
            const std::string unit = lower(trim(text.substr(unit_start, i - unit_start)));

            long long multiplier_ms = 0;
            if (unit == "ms") multiplier_ms = 1;
            else if (unit == "s" || unit == "sec" || unit == "secs" || unit == "second" || unit == "seconds")
                multiplier_ms = 1000;
            else if (unit == "m" || unit == "min" || unit == "mins" || unit == "minute" || unit == "minutes")
                multiplier_ms = 60LL * 1000;
            else if (unit == "h" || unit == "hr" || unit == "hrs" || unit == "hour" || unit == "hours")
                multiplier_ms = 3600LL * 1000;
            else if (unit == "d" || unit == "day" || unit == "days") multiplier_ms = 86400LL * 1000;
            else if (unit == "w" || unit == "week" || unit == "weeks") multiplier_ms = 7LL * 86400 * 1000;
            else return std::nullopt;

            total += std::chrono::milliseconds(static_cast<int64_t>(magnitude * static_cast<double>(multiplier_ms)));
            consumed_any = true;
            // Optional separators between components: "1h 30m", "1h,30m".
            while (i < text.size() && (text[i] == ' ' || text[i] == ',' || text[i] == ':')) ++i;
        }
        if (!consumed_any) return std::nullopt;
        return total;
    }

    std::optional<std::chrono::milliseconds> parseRelativeDelay(std::string_view raw_text) {
        std::string text = lower(trim(raw_text));
        constexpr std::string_view kIn = "in ";
        if (text.rfind(kIn, 0) == 0) text = text.substr(kIn.size());
        else if (text.rfind("after ", 0) == 0) text = text.substr(6);
        else if (text.rfind("in", 0) == 0) text = text.substr(2);

        // "in 5 minutes" (space separated) -> "5minutes"
        std::string compact;
        for (char c : text) {
            if (!std::isspace(static_cast<unsigned char>(c))) compact.push_back(c);
        }
        // Insert a unit letter so the duration parser can chew on it.
        static const std::array<std::pair<const char *, const char *>, 12> kWords = {{
            {"milliseconds", "ms"}, {"millisecond", "ms"}, {"milliseconds", "ms"}, {"ms", "ms"},
            {"seconds", "s"}, {"second", "s"}, {"secs", "s"}, {"minutes", "m"}, {"minute", "m"},
            {"hours", "h"}, {"hour", "h"}, {"days", "d"},
        }};
        for (const auto &[word, letter] : kWords) {
            size_t pos = 0;
            const std::string needle = word;
            while ((pos = compact.find(needle, pos)) != std::string::npos) {
                compact.replace(pos, needle.size(), letter);
                pos += needle.size();
            }
        }
        return parseDuration(compact);
    }

    std::optional<int64_t> parseIso8601(std::string_view raw_text) {
        const std::string text = trim(raw_text);
        std::tm tm{};
        int offset_seconds = 0;
        bool has_offset = false;

        int year = 1970, month = 1, day = 1, hour = 0, minute = 0;
        double second = 0.0;
        const char *scan = text.c_str();
        char tail[64] = {0};
        int matched = std::sscanf(scan, "%d-%d-%dT%d:%d:%lf%63s", &year, &month, &day, &hour, &minute, &second, tail);
        if (matched < 3) {
            matched = std::sscanf(scan, "%d-%d-%d %d:%d:%lf%63s", &year, &month, &day, &hour, &minute, &second, tail);
            if (matched < 3) return std::nullopt;
        }
        if (matched == 3) {
            hour = minute = 0;
            second = 0.0;
        }

        const std::string zone = tail[0] != '\0' ? lower(trim(tail)) : "";
        if (!zone.empty()) {
            if (zone == "z") {
                has_offset = true;
            } else if (zone.size() >= 5 && (zone[0] == '+' || zone[0] == '-')) {
                int zh = 0, zm = 0;
                const char sign = zone[0];
                if (zone.size() >= 6 && zone[3] == ':') {
                    zh = std::atoi(zone.substr(1, 2).c_str());
                    zm = std::atoi(zone.substr(4, 2).c_str());
                } else {
                    zh = std::atoi(zone.substr(1, 2).c_str());
                    zm = zone.size() >= 5 ? std::atoi(zone.substr(3, 2).c_str()) : 0;
                }
                offset_seconds = (sign == '-' ? -1 : 1) * (zh * 3600 + zm * 60);
                has_offset = true;
            } else {
                return std::nullopt; // named zone: caller resolves via ZoneOffset
            }
        }

        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = minute;
        tm.tm_sec = static_cast<int>(second);
        if (!has_offset) offset_seconds = 0; // assume UTC when unqualified

        const time_t epoch = toEpoch(tm);
        if (epoch == static_cast<time_t>(-1)) return std::nullopt;
        int64_t ms = static_cast<int64_t>(epoch) * 1000 + static_cast<int64_t>((second - std::floor(second)) * 1000.0);
        return ms - offset_seconds * 1000;
    }

    std::string formatIso8601(int64_t epoch_ms) {
        const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
        std::tm tm_utc{};
#if defined(_WIN32)
        gmtime_s(&tm_utc, &seconds);
#else
        gmtime_r(&seconds, &tm_utc);
#endif
        std::ostringstream out;
        out << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
            << (epoch_ms % 1000) << 'Z';
        return out.str();
    }

    std::string ZoneOffset::toString() const {
        const char sign = seconds < 0 ? '-' : '+';
        const int abs_seconds = seconds < 0 ? -seconds : seconds;
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%c%02d:%02d", sign, abs_seconds / 3600, (abs_seconds / 60) % 60);
        return buffer;
    }

    std::optional<ZoneOffset> parseZoneOffset(std::string_view raw_zone) {
        const std::string zone = lower(trim(raw_zone));
        if (zone.empty() || zone == "utc" || zone == "gmt" || zone == "z") return ZoneOffset{0};
        if (zone == "local") return std::nullopt; // host-local is resolved by the caller
        if ((zone[0] == '+' || zone[0] == '-') && zone.size() >= 3) {
            std::string body = zone.substr(1);
            const int colon = static_cast<int>(body.find(':'));
            int hours = 0, minutes = 0;
            if (colon > 0) {
                hours = std::atoi(body.substr(0, colon).c_str());
                minutes = std::atoi(body.substr(colon + 1).c_str());
            } else if (body.size() >= 4 && std::all_of(body.begin(), body.end(),
                                                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
                hours = std::atoi(body.substr(0, 2).c_str());
                minutes = std::atoi(body.substr(2, 2).c_str());
            } else {
                hours = std::atoi(body.c_str());
            }
            const int total = hours * 3600 + minutes * 60;
            return ZoneOffset{zone[0] == '-' ? -total : total};
        }
        return std::nullopt;
    }

    BrokenDownTime breakDown(int64_t epoch_ms, int offset_seconds) {
        const std::time_t adjusted = static_cast<std::time_t>((epoch_ms + offset_seconds * 1000) / 1000);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &adjusted);
#else
        gmtime_r(&adjusted, &tm);
#endif
        BrokenDownTime out;
        out.year = tm.tm_year + 1900;
        out.month = tm.tm_mon + 1;
        out.day = tm.tm_mday;
        out.hour = tm.tm_hour;
        out.minute = tm.tm_min;
        out.second = tm.tm_sec;
        out.weekday = tm.tm_wday;
        return out;
    }

    int64_t compose(const BrokenDownTime &tm, int offset_seconds) {
        std::tm std_tm{};
        std_tm.tm_year = tm.year - 1900;
        std_tm.tm_mon = tm.month - 1;
        std_tm.tm_mday = tm.day;
        std_tm.tm_hour = tm.hour;
        std_tm.tm_min = tm.minute;
        std_tm.tm_sec = tm.second;
        return static_cast<int64_t>(toEpoch(std_tm) - offset_seconds) * 1000;
    }

} // namespace sapo::util
