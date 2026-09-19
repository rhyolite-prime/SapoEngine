//
//  Sapo Engine — shared test fixtures.
//
//  Every test runs against `TaskServices` with a ManualClock, a MockTransport and
//  an in-memory state store, so the suite is deterministic and never touches the
//  network or the wall clock. Retry backoff and prompt timeouts are driven by
//  `advance()` / `tick()`.
//
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <catch2/catch_amalgamated.hpp>

#include "capabilities/CapabilityRegistry.hpp"
#include "data/DataSourceProvider.hpp"
#include "http/IHttpTransport.hpp"
#include "observability/Logger.hpp"
#include "parser/WorkflowParser.hpp"
#include "runtime/Clock.hpp"
#include "runtime/StateStore.hpp"
#include "runtime/VirtualMachine.hpp"
#include "tasks/Task.hpp"

namespace sapo::testing {

    /// A capability provider that records calls and replies with a canned result,
    /// so plugin-backed nodes can be tested without any real integration.
    class FakeProvider final : public capabilities::ICapabilityProvider {
    public:
        FakeProvider(std::string id, std::vector<std::string> namespaces)
            : m_id(std::move(id)), m_namespaces(std::move(namespaces)) {}

        [[nodiscard]] std::string providerId() const override { return m_id; }
        [[nodiscard]] std::vector<std::string> namespaces() const override { return m_namespaces; }

        [[nodiscard]] std::vector<capabilities::CapabilityDescriptor> capabilities() const override {
            std::vector<capabilities::CapabilityDescriptor> out;
            for (const auto &[name, result] : m_replies) {
                capabilities::CapabilityDescriptor descriptor;
                descriptor.name = name;
                descriptor.provider = m_id;
                descriptor.description = "test capability " + name;
                out.push_back(descriptor);
            }
            return out;
        }

        [[nodiscard]] capabilities::CapabilityResult execute(const capabilities::CapabilityCall &call) override {
            {
                std::lock_guard<std::mutex> guard(m_mutex);
                ++m_calls[call.name];
                m_last = call;
            }
            auto reply = m_replies.find(call.name);
            if (reply == m_replies.end()) {
                return capabilities::CapabilityResult::failure("NOT_IMPLEMENTED",
                                                                "FakeProvider has no reply for " + call.name);
            }
            if (reply->second.is_object() && reply->second.contains("__fail")) {
                return capabilities::CapabilityResult::failure(reply->second.value("__fail", "CAPABILITY_ERROR"),
                                                               reply->second.value("message", "failed"));
            }
            return capabilities::CapabilityResult::success(reply->second);
        }

        void reply(const std::string &name, nlohmann::json value) { m_replies[name] = std::move(value); }

        [[nodiscard]] size_t calls(const std::string &name) const {
            std::lock_guard<std::mutex> guard(m_mutex);
            auto it = m_calls.find(name);
            return it == m_calls.end() ? 0 : it->second;
        }
        [[nodiscard]] nlohmann::json lastInput() const {
            std::lock_guard<std::mutex> guard(m_mutex);
            return m_last.inputs;
        }

    private:
        std::string m_id;
        std::vector<std::string> m_namespaces;
        std::map<std::string, nlohmann::json> m_replies;
        std::map<std::string, size_t> m_calls;
        mutable std::mutex m_mutex;
        capabilities::CapabilityCall m_last;
    };

    /// Deterministic services plus the handles a test needs to poke time, HTTP and
    /// capability replies.
    struct Fixture {
        std::shared_ptr<runtime::ManualClock> clock;
        std::shared_ptr<sapo::http::MockTransport> transport;
        std::shared_ptr<sapo::testing::FakeProvider> provider;
        std::shared_ptr<sapo::obs::MemorySink> log_sink;
        std::vector<int64_t> delays;
        runtime::TaskServices services;

        Fixture() {
            clock = std::make_shared<runtime::ManualClock>(std::chrono::milliseconds(1700000000000LL));
            transport = std::make_shared<sapo::http::MockTransport>();
            provider = std::make_shared<sapo::testing::FakeProvider>("test", std::vector<std::string>{"test"});
            log_sink = std::make_shared<sapo::obs::MemorySink>();

            auto capabilities = std::make_shared<sapo::capabilities::CapabilityRegistry>();
            capabilities->addProvider(provider);

            services.logger = std::make_shared<sapo::obs::Logger>();
            services.logger->setLevel(sapo::obs::LogLevel::Warn);
            services.logger->addSink(log_sink);
            services.metrics = std::make_shared<sapo::obs::MetricsRegistry>();
            services.traces = std::make_shared<sapo::obs::TraceRecorder>(services.logger);
            services.clock = clock;
            services.transport = transport;
            services.capabilities = capabilities;
            services.data_sources = sapo::data::DataSourceRegistry::withBuiltIns(services.transport);
            services.events = std::make_shared<runtime::EventBus>();
            services.scheduler = std::make_shared<runtime::Scheduler>(clock);
            services.state_store = std::make_shared<runtime::InMemoryStateStore>();
            services.workflows = std::make_shared<runtime::WorkflowRegistry>(parser::ParseOptions{}, clock);
            services.pool = std::make_shared<runtime::WorkerPool>(4);
            delays.clear();
            services.delay_sink = [this](int64_t delay_ms) {
                delays.push_back(delay_ms);
                clock->advanceMs(delay_ms);   // backoff burns virtual time, not wall time
            };
        }

        /// A VM wired to the same services (so the test can inspect clock/transport).
        [[nodiscard]] std::unique_ptr<runtime::VirtualMachine> machine() const {
            auto vm = std::make_unique<runtime::VirtualMachine>(services);
            vm->start();
            return vm;
        }

        void advanceMs(int64_t ms) const { clock->advanceMs(ms); }
        [[nodiscard]] sapo::http::Response reply(int status, const std::string &body = {},
                                                 const std::string &transport_error = {}) const {
            sapo::http::Response response;
            response.status_code = status;
            response.body = body;
            response.transport_error = transport_error;
            if (status > 0) response.headers = {{"content-type", "application/json"}};
            return response;
        }
    };

    [[nodiscard]] inline nlohmann::json blueprint(const std::string &text) {
        return nlohmann::json::parse(text, nullptr, false);
    }

    [[nodiscard]] inline nlohmann::json objectOrThrow(const std::string &text) {
        auto parsed = nlohmann::json::parse(text, nullptr, false);
        if (parsed.is_discarded()) throw std::runtime_error("test blueprint is not valid JSON: " + text.substr(0, 60));
        return parsed;
    }

} // namespace sapo::testing
