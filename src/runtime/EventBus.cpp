//
//  Sapo Engine — event bus implementation.
//
#include "runtime/EventBus.hpp"
#include "observability/Logger.hpp"
#include "runtime/SapoError.hpp"

#include <algorithm>

using json = nlohmann::json;

namespace sapo::runtime {

    json Event::toJson() const {
        json out = {{"name", name}, {"payload", payload}, {"timestamp_ms", timestamp_ms}};
        if (!session_id.empty()) out["session_id"] = session_id;
        if (!execution_id.empty()) out["execution_id"] = execution_id;
        if (!correlation_id.empty()) out["correlation_id"] = correlation_id;
        if (!source_node.empty()) out["source_node"] = source_node;
        return out;
    }

    Event Event::fromJson(const json &value) {
        Event event;
        event.name = value.value("name", "");
        if (value.contains("payload")) event.payload = value["payload"];
        event.session_id = value.value("session_id", "");
        event.execution_id = value.value("execution_id", "");
        event.correlation_id = value.value("correlation_id", "");
        event.source_node = value.value("source_node", "");
        event.timestamp_ms = value.value("timestamp_ms", 0LL);
        return event;
    }

    bool EventBus::matches(const std::string &pattern, const std::string &name) {
        if (pattern == "*" || pattern == name) return true;
        if (pattern.size() > 2 && pattern.back() == '*') {
            const std::string prefix = pattern.substr(0, pattern.size() - 1);
            return name.rfind(prefix, 0) == 0;
        }
        return false;
    }

    EventBus::SubscriptionId EventBus::subscribe(const std::string &pattern, Handler handler) {
        std::scoped_lock lock(m_mutex);
        const SubscriptionId id = m_next_id++;
        m_subscribers.push_back(Subscriber{id, pattern, std::move(handler)});
        return id;
    }

    bool EventBus::unsubscribe(SubscriptionId id) {
        std::scoped_lock lock(m_mutex);
        const auto before = m_subscribers.size();
        std::erase_if(m_subscribers, [id](const Subscriber &subscriber) { return subscriber.id == id; });
        return m_subscribers.size() != before;
    }

    void EventBus::publish(const std::string &name, json payload) {
        Event event;
        event.name = name;
        event.payload = std::move(payload);
        publish(std::move(event));
    }

    void EventBus::publish(Event event) {
        std::vector<Subscriber> targets;
        {
            std::scoped_lock lock(m_mutex);
            for (const auto &subscriber : m_subscribers) {
                if (matches(subscriber.pattern, event.name)) targets.push_back(subscriber);
            }
            m_recent.push_back(event);
            while (m_recent.size() > m_recent_capacity) m_recent.pop_front();
        }
        for (const auto &subscriber : targets) {
            try {
                subscriber.handler(event);
            } catch (const std::exception &error) {
                // A listener failing must never fail the publishing workflow.
                obs::defaultLogger()->error("events",
                                            "subscriber for '" + event.name + "' failed: " + error.what(),
                                            json{{"event", event.toJson()}});
            }
        }
    }

    size_t EventBus::subscriberCount() const {
        std::scoped_lock lock(m_mutex);
        return m_subscribers.size();
    }

    std::vector<std::string> EventBus::patterns() const {
        std::scoped_lock lock(m_mutex);
        std::vector<std::string> out;
        out.reserve(m_subscribers.size());
        for (const auto &subscriber : m_subscribers) out.push_back(subscriber.pattern);
        return out;
    }

    std::vector<Event> EventBus::recent(size_t limit) const {
        std::scoped_lock lock(m_mutex);
        const size_t start = m_recent.size() > limit ? m_recent.size() - limit : 0;
        return {m_recent.begin() + static_cast<std::ptrdiff_t>(start), m_recent.end()};
    }

    void EventBus::setRecentCapacity(size_t capacity) {
        std::scoped_lock lock(m_mutex);
        m_recent_capacity = capacity == 0 ? 1 : capacity;
        while (m_recent.size() > m_recent_capacity) m_recent.pop_front();
    }

    void EventBus::clearRecent() {
        std::scoped_lock lock(m_mutex);
        m_recent.clear();
    }

    EventBus &EventBus::global() {
        static EventBus bus;
        return bus;
    }

} // namespace sapo::runtime
