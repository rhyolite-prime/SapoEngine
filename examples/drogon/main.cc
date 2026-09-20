//
//  sapo_service — reference Drogon front-end for Sapo Engine.
//
//  This is the "service mode" deployment of the engine (implementation_plan_2.md
//  T4.3) built on the same embedding pattern any Drogon API can use: one
//  `sapo::runtime::VirtualMachine` owned by the process, driven by HTTP handlers.
//  Endpoint set:
//
//      POST   /workflows/{id}/runs      start a session            {"input": {...}}
//      POST   /sessions/{id}/input      resume a parked session    {"input": {...}}
//      GET    /sessions                 list sessions
//      GET    /sessions/{id}            one session snapshot
//      DELETE /sessions/{id}            cancel a session           {"reason": "..."}
//      POST   /events                   publish into the event bus {"name": ..., "payload": {...}}
//      GET    /workflows                registered workflow ids
//      GET    /healthz                  liveness + engine summary
//      GET    /metrics                  engine metrics (JSON)
//
//  Requires Drogon >= 1.9 (drogon::async_run + coroutine handlers) and a C++23
//  compiler. Engine calls are synchronous, so handlers hop off the IO threads
//  with `co_await drogon::async_run(...)` before touching the VM.
//
#include <drogon/drogon.h>
#include <drogon/utils/Coroutine.h>
#include <drogon/version.h>

#if !defined(DROGON_VERSION_MAJOR) || DROGON_VERSION_MAJOR < 1 || \
    (DROGON_VERSION_MAJOR == 1 && DROGON_VERSION_MINOR < 9)
#error "sapo_service needs Drogon >= 1.9 (drogon::async_run, coroutine handlers)"
#endif

#include <nlohmann/json.hpp>

#include "observability/Logger.hpp"
#include "runtime/SapoError.hpp"
#include "runtime/StateStore.hpp"
#include "runtime/VirtualMachine.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using nlohmann::json;
using sapo::runtime::ExecutionOutcome;
using sapo::runtime::SessionSnapshot;
using sapo::runtime::StartSessionOptions;
using sapo::runtime::TaskServices;
using sapo::runtime::VirtualMachine;

struct Settings {
    int port{8090};
    std::string config;                       // sapo-config.json
    std::string workflows;                    // blueprint directory
    std::string state_dir;                    // empty ⇒ in-memory store
    std::string log_level{"info"};
};

struct SapoHost {
    std::unique_ptr<VirtualMachine> vm;
};

SapoHost g_host;

// ---------------------------------------------------------------------------
// JSON bridging: the engine speaks nlohmann::json; Drogon responses are bytes.
// ---------------------------------------------------------------------------

drogon::HttpResponsePtr jsonResponse(const json &body, drogon::HttpStatusCode code = drogon::k200OK) {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(code);
    response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    response->setBody(body.dump());
    return response;
}

/// Engine statuses are outcomes, not transport errors: 200 for finished runs,
/// 202 while a session is parked on a prompt/timer, 4xx/5xx only for failures.
drogon::HttpResponsePtr outcomeResponse(const ExecutionOutcome &outcome) {
    drogon::HttpStatusCode code = drogon::k200OK;
    if (outcome.status == "awaiting_input" || outcome.status == "suspended") {
        code = drogon::k202Accepted;
    } else if (outcome.status == "failed") {
        const bool not_found = outcome.error_code == "WORKFLOW_NOT_FOUND" ||
                               outcome.error_code == "SESSION_NOT_FOUND";
        code = not_found ? drogon::k404NotFound : drogon::k500InternalServerError;
    }
    return jsonResponse(outcome.toJson(), code);
}

/// `SapoError` escapes only from blueprint registration/config paths — the
/// session API reports failures inside the outcome instead.
drogon::HttpResponsePtr errorResponse(const sapo::runtime::SapoError &error) {
    using sapo::runtime::ErrorCode;
    drogon::HttpStatusCode code = drogon::k500InternalServerError;
    switch (error.code()) {
        case ErrorCode::NotFound:
            code = drogon::k404NotFound;
            break;
        case ErrorCode::Parse:
        case ErrorCode::Validation:
        case ErrorCode::Routing:
            code = drogon::k400BadRequest;
            break;
        case ErrorCode::NotImplemented:
            code = drogon::k501NotImplemented;
            break;
        default:
            code = drogon::k500InternalServerError;
            break;
    }
    return jsonResponse(json{{"status", "failed"}, {"error", error.toJson()}}, code);
}

std::optional<json> parseBody(const drogon::HttpRequestPtr &request,
                              const std::function<void(const drogon::HttpResponsePtr &)> &callback) {
    if (request->getBody().empty()) return json::object();
    try {
        return json::parse(request->getBody());
    } catch (const json::exception &) {
        callback(jsonResponse({{"error", "request body is not valid JSON"}}, drogon::k400BadRequest));
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// Routes
// ---------------------------------------------------------------------------

void registerRoutes() {
    using namespace drogon;

    app().registerHandler(
        "/healthz",
        [](const HttpRequestPtr &, std::function<void(const HttpResponsePtr &)> &&callback) {
            callback(jsonResponse({{"status", "ok"},
                                   {"engine", "sapo-engine"},
                                   {"running", g_host.vm != nullptr && g_host.vm->running()},
                                   {"suspended_sessions", g_host.vm->suspendedSessionCount()}}));
        },
        {Get});

    // A mutex-guarded snapshot of the counters — cheap enough to build inline.
    app().registerHandler(
        "/metrics",
        [](const HttpRequestPtr &, std::function<void(const HttpResponsePtr &)> &&callback) {
            callback(jsonResponse(g_host.vm->metrics()));
        },
        {Get});

    app().registerHandler(
        "/workflows",
        [](const HttpRequestPtr &, std::function<void(const HttpResponsePtr &)> &&callback) {
            auto ids = g_host.vm->workflows().ids();
            callback(jsonResponse({{"workflows", ids}}));
        },
        {Get});

    // Start a session. Pass "session_id" to pin it (e.g. the telco's USSD
    // SESSIONID) so follow-ups can resume by that exact key.
    app().registerHandler(
        "/workflows/{1}/runs",
        [](const HttpRequestPtr &request, std::function<void(const HttpResponsePtr &)> &&callback,
           std::string workflow_id) -> Task<> {
            auto body = parseBody(request, callback);
            if (!body.has_value()) co_return;

            StartSessionOptions options;
            options.session_id = body->value("session_id", "");
            options.correlation_id = body->value("correlation_id", "");
            if (body->contains("start_node")) options.start_node = (*body)["start_node"].get<std::string>();
            const json input = body->value("input", json::object());
            const std::string workflow = workflow_id;   // locals survive suspension points

            ExecutionOutcome outcome;
            co_await drogon::async_run([&] { outcome = g_host.vm->startSession(workflow, input, options); });
            callback(outcomeResponse(outcome));
        },
        {Post});

    // Resume a parked session with the user's answer.
    app().registerHandler(
        "/sessions/{1}/input",
        [](const HttpRequestPtr &request, std::function<void(const HttpResponsePtr &)> &&callback,
           std::string session_id) -> Task<> {
            auto body = parseBody(request, callback);
            if (!body.has_value()) co_return;
            const json input = body->contains("input") ? (*body)["input"] : *body;
            const std::string session = session_id;   // locals survive suspension points

            ExecutionOutcome outcome;
            co_await drogon::async_run([&] { outcome = g_host.vm->resumeSession(session, input); });
            callback(outcomeResponse(outcome));
        },
        {Post});

    app().registerHandler(
        "/sessions",
        [](const HttpRequestPtr &, std::function<void(const HttpResponsePtr &)> &&callback) {
            json sessions = json::array();
            for (const SessionSnapshot &snapshot : g_host.vm->sessions()) {
                sessions.push_back(snapshot.toJson());
            }
            callback(jsonResponse({{"sessions", sessions}}));
        },
        {Get});

    app().registerHandler(
        "/sessions/{1}",
        [](const HttpRequestPtr &, std::function<void(const HttpResponsePtr &)> &&callback,
           std::string session_id) {
            const auto snapshot = g_host.vm->session(session_id);
            if (!snapshot.has_value()) {
                callback(jsonResponse({{"error", "session not found"}, {"session_id", session_id}},
                                      drogon::k404NotFound));
                return;
            }
            callback(jsonResponse(snapshot->toJson()));
        },
        {Get});

    app().registerHandler(
        "/sessions/{1}",
        [](const HttpRequestPtr &request, std::function<void(const HttpResponsePtr &)> &&callback,
           std::string session_id) {
            std::string reason = "cancelled via API";
            if (!request->getBody().empty()) {
                auto body = parseBody(request, callback);
                if (!body.has_value()) return;   // 400 already sent
                reason = body->value("reason", reason);
            }
            bool cancelled = false;
            try {
                cancelled = g_host.vm->cancelSession(session_id, reason);
            } catch (const sapo::runtime::SapoError &error) {
                callback(errorResponse(error));
                return;
            }
            if (!cancelled) {
                callback(jsonResponse({{"error", "nothing to cancel (unknown or finished session)"}},
                                      drogon::k404NotFound));
                return;
            }
            callback(jsonResponse({{"status", "cancelled"}, {"session_id", session_id}}));
        },
        {Delete});

    // Publish an event: wakes event-waiting sessions and fires workflow triggers.
    app().registerHandler(
        "/events",
        [](const HttpRequestPtr &request, std::function<void(const HttpResponsePtr &)> &&callback) -> Task<> {
            auto body = parseBody(request, callback);
            if (!body.has_value()) co_return;
            const std::string name = body->value("name", "");
            if (name.empty()) {
                callback(jsonResponse({{"error", "\"name\" is required"}}, drogon::k400BadRequest));
                co_return;
            }
            const json payload = body->value("payload", json::object());
            co_await drogon::async_run([&] { g_host.vm->publishEvent(name, payload); });
            callback(jsonResponse({{"published", name}, {"payload", payload}}, drogon::k202Accepted));
        },
        {Post});
}

sapo::obs::LogLevel levelFrom(const std::string &name) {
    if (name == "trace") return sapo::obs::LogLevel::Trace;
    if (name == "debug") return sapo::obs::LogLevel::Debug;
    if (name == "info") return sapo::obs::LogLevel::Info;
    if (name == "warn") return sapo::obs::LogLevel::Warn;
    if (name == "error") return sapo::obs::LogLevel::Error;
    return sapo::obs::LogLevel::Info;
}

void printUsage() {
    std::cout << "sapo_service — Sapo Engine over HTTP (Drogon)\n"
                 "  --port <n>          listen port (default 8090)\n"
                 "  --config <file>     sapo-config.json (providers, secrets, limits)\n"
                 "  --workflows <dir>   blueprint directory (also auto-registered on start)\n"
                 "  --state-dir <dir>   durable session store (default: in-memory)\n"
                 "  --log-level <lvl>   trace|debug|info|warn|error\n";
}

} // namespace

int main(int argc, char **argv) {
    Settings settings;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        auto value = [&](const char *flag) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << flag << " expects a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (argument == "--port") settings.port = std::atoi(value("--port").c_str());
        else if (argument == "--config") settings.config = value("--config");
        else if (argument == "--workflows") settings.workflows = value("--workflows");
        else if (argument == "--state-dir") settings.state_dir = value("--state-dir");
        else if (argument == "--log-level") settings.log_level = value("--log-level");
        else {
            printUsage();
            return argument == "--help" || argument == "-h" ? 0 : 2;
        }
    }

    // ---- compose the engine exactly like `sapoc` does ---------------------
    TaskServices services = TaskServices::defaults();

    auto logger = std::make_shared<sapo::obs::Logger>();
    logger->setLevel(levelFrom(settings.log_level));
    logger->addSink(std::make_shared<sapo::obs::ConsoleSink>());
    services.logger = logger;

    if (!settings.state_dir.empty()) {
        std::error_code error;
        std::filesystem::create_directories(settings.state_dir, error);
        if (error) {
            std::cerr << "cannot create state directory '" << settings.state_dir << "': " << error.message() << "\n";
            return 2;
        }
        services.state_store = std::make_shared<sapo::runtime::FileStateStore>(settings.state_dir);
    }

    auto vm = std::make_unique<VirtualMachine>(std::move(services));
    if (!settings.config.empty()) {
        if (!std::filesystem::exists(settings.config)) {
            std::cerr << "no such config file '" << settings.config << "'\n";
            return 2;
        }
        vm->setConfigPath(settings.config);
    }
    if (!settings.workflows.empty()) vm->setWorkflowDirectory(settings.workflows);

    try {
        const std::vector<std::string> problems = vm->start();
        for (const auto &problem : problems) std::cerr << "startup: " << problem << "\n";
        if (!problems.empty()) {
            std::cerr << "sapo_service: startup audit found " << problems.size()
                      << " problem(s); refusing to serve\n";
            return 1;
        }
    } catch (const sapo::runtime::SapoError &error) {
        std::cerr << "sapo_service: " << error.message() << "\n";
        return 1;
    }

    g_host.vm = std::move(vm);
    g_host.vm->startBackgroundTick(std::chrono::milliseconds(250));  // timers, cron, event wakeups

    registerRoutes();

    drogon::app().registerSyncAdvice([] {
        // Graceful shutdown: stop the scheduler thread before the process exits.
        if (g_host.vm) g_host.vm->stop();
    });

    std::cout << "sapo_service listening on 0.0.0.0:" << settings.port
              << " (" << g_host.vm->workflows().size() << " workflows registered)\n";
    drogon::app().addListener("0.0.0.0", static_cast<uint16_t>(settings.port)).run();
    return 0;
}
