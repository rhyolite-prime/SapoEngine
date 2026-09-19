//
//  Sapo Engine — the `sapoc` command line.
//
//  Everything the CLI can do, the embeddable API can do: this binary is a thin
//  driver over `sapo::runtime::VirtualMachine`, so the semantics proven by the
//  test suite are exactly the semantics a host application gets.
//
//      sapoc run    examples/hello.json --var msisdn=233201234567
//      sapoc resume 0f2c… --var otp=123456 --state-dir .sapo-state
//      sapoc validate schemas examples --strict
//      sapoc describe --json
//
//  Exit codes: 0 success · 1 execution failed · 2 usage or blueprint error ·
//  3 session parked (awaiting input / timer).
//
#include "config/ProviderConfig.hpp"
#include "observability/Logger.hpp"
#include "runtime/StateStore.hpp"
#include "runtime/VirtualMachine.hpp"
#include "util/TimeUtils.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using nlohmann::json;
using sapo::runtime::ExecutionOutcome;
using sapo::runtime::SessionSnapshot;
using sapo::runtime::StartSessionOptions;
using sapo::runtime::TaskServices;
using sapo::runtime::VirtualMachine;

constexpr const char *kVersion = "0.4.0";

struct Options {
    std::string command;
    std::vector<std::string> positional;
    std::map<std::string, std::string> values;
    std::vector<std::pair<std::string, std::string>> vars;
    std::map<std::string, bool> flags;

    [[nodiscard]] bool has(const std::string &flag) const {
        auto it = flags.find(flag);
        return it != flags.end() && it->second;
    }
    [[nodiscard]] std::string value(const std::string &key, const std::string &fallback = "") const {
        auto it = values.find(key);
        return it == values.end() ? fallback : it->second;
    }
    [[nodiscard]] bool empty() const { return values.empty() && vars.empty(); }
};

[[noreturn]] void fail(const std::string &message, int code = 2) {
    std::cerr << "sapoc: " << message << "\n";
    std::exit(code);
}

void printUsage() {
    std::cout <<
        R"(sapoc — the Sapo Engine CLI (v)" << "\n"
R"(
Usage:
  sapoc run <blueprint.json> [options]         start a session from a blueprint file
  sapoc resume <session-id> [options]          answer a parked session and continue it
  sapoc sessions [options]                     list sessions in the state store
  sapoc session <session-id> [options]         show one session (checkpoint + context)
  sapoc cancel <session-id> [options]          cancel a parked session
  sapoc emit <event.name> [options]            publish an event into a running engine
  sapoc validate <path...> [options]           parse + statically validate blueprints
  sapoc describe [options]                     tasks, capabilities, providers, limits
  sapoc version | help

Options:
  --var <name>=<json>      seed the session input (repeatable); bare words are strings
  --input <file|->         merge a JSON object into the session input ('-' reads stdin)
  --config <file>          sapo-config.json (providers, secrets, engine limits)
  --state-dir <dir>        durable session store directory (default: in-memory only)
  --session <id>           fixed session id for `run`
  --start-node <id>        enter the blueprint at a specific node
  --correlation-id <id>    tag the execution for logs/traces
  --max-visits <n>         node-activation budget for this run
  --blueprint <file>       register an extra blueprint (repeatable; needed by `resume`)
  --wait                   drive timers/cron until the session finishes or --max-time passes
  --max-time <duration>    bound for --wait (e.g. 30s, 2m; default 10s)
  --tick-ms <n>            scheduler tick interval while waiting (default 25)
  --json                   machine-readable output
  --trace                  print the per-node trace spans of the execution
  --log-level <lvl>        trace|debug|info|warn|error|off (default warn)
  --log-file <file>        append JSON log lines to a file instead of stderr
  --strict                 validation warnings become errors
  --no-persist             do not write the checkpoint (single-shot runs)
  --answer <json>          shorthand input for `resume`
)";
}

/// `--var key=value` keeps scripts readable: anything that parses as JSON wins,
/// otherwise the raw text is used (so `--var msisdn=233201…` does not need quotes).
json parseVarValue(const std::string &text) {
    try {
        return json::parse(text);
    } catch (const json::exception &) {
        return text;
    }
}

json mergeVars(const std::vector<std::pair<std::string, std::string>> &vars, json input) {
    if (!input.is_object()) input = json::object();
    for (const auto &[key, raw] : vars) {
        if (key.empty()) fail("--var expects name=json");
        input[key] = parseVarValue(raw);
    }
    return input;
}

json readFile(const std::string &path) {
    if (path == "-") {
        std::stringstream buffer;
        buffer << std::cin.rdbuf();
        try {
            return json::parse(buffer.str());
        } catch (const json::exception &error) {
            fail("stdin is not valid JSON: " + std::string(error.what()));
        }
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) fail("cannot open '" + path + "'");
    std::stringstream buffer;
    buffer << in.rdbuf();
    try {
        return json::parse(buffer.str());
    } catch (const json::exception &error) {
        fail("'" + path + "' is not valid JSON: " + std::string(error.what()));
    }
}

Options parseArguments(int argc, char **argv) {
    Options options;
    if (argc < 2) {
        printUsage();
        std::exit(2);
    }
    options.command = argv[1];
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument.rfind("--", 0) != 0) {
            options.positional.emplace_back(argument);
            continue;
        }
        const std::string body = argument.substr(2);
        const auto equals = body.find('=');
        if (equals != std::string::npos) {
            options.values[body.substr(0, equals)] = body.substr(equals + 1);
            continue;
        }
        if (body == "var" || body == "input" || body == "config" || body == "state-dir" || body == "session" ||
            body == "start-node" || body == "correlation-id" || body == "max-visits" || body == "blueprint" ||
            body == "max-time" || body == "tick-ms" || body == "log-level" || body == "log-file" || body == "answer" ||
            body == "reason") {
            if (index + 1 >= argc) fail("--" + body + " expects a value");
            const std::string value = argv[++index];
            if (body == "var") {
                const auto split = value.find('=');
                if (split == std::string::npos) fail("--var expects name=json, got '" + value + "'");
                options.vars.emplace_back(value.substr(0, split), value.substr(split + 1));
            } else {
                options.values[body] = value;
            }
            continue;
        }
        if (body == "json" || body == "wait" || body == "trace" || body == "strict" || body == "no-persist") {
            options.flags[body] = true;
            continue;
        }
        if (!body.empty() && body.front() == 'n') {   // --no-trace, --no-wait …
            options.flags[body.substr(4)] = false;
            continue;
        }
        fail("unknown option '--" + body + "' (see `sapoc help`)");
    }
    return options;
}

sapo::obs::LogLevel levelFrom(const std::string &name, sapo::obs::LogLevel fallback) {
    if (name == "trace") return sapo::obs::LogLevel::Trace;
    if (name == "debug") return sapo::obs::LogLevel::Debug;
    if (name == "info") return sapo::obs::LogLevel::Info;
    if (name == "warn") return sapo::obs::LogLevel::Warn;
    if (name == "error") return sapo::obs::LogLevel::Error;
    if (name == "off") return sapo::obs::LogLevel::Off;
    return fallback;
}

/// Assembles the service graph from flags + `sapo-config.json`. Everything that a
/// host would inject is injected here, so the CLI never has private behaviour.
TaskServices buildServices(const Options &options) {
    TaskServices services = TaskServices::defaults();

    auto logger = std::make_shared<sapo::obs::Logger>();
    logger->setLevel(levelFrom(options.value("log-level", "warn"), sapo::obs::LogLevel::Warn));
    if (options.has("trace")) logger->setLevel(sapo::obs::LogLevel::Debug);
    if (const std::string file = options.value("log-file"); !file.empty()) {
        auto stream = std::make_shared<std::ofstream>(file, std::ios::app);
        if (!stream->is_open()) fail("cannot open log file '" + file + "'");
        logger->addSink(std::make_shared<sapo::obs::JsonLineSink>(std::static_pointer_cast<std::ostream>(stream)));
    } else {
        logger->addSink(std::make_shared<sapo::obs::ConsoleSink>());
    }
    services.logger = logger;

    const std::string config_path = options.value("config");
    if (!config_path.empty()) {
        try {
            services.provider_config =
                std::make_shared<sapo::config::ProviderConfigStore>(sapo::config::ProviderConfigStore::load(config_path));
        } catch (const std::exception &error) {
            fail("cannot load '" + config_path + "': " + error.what());
        }
    } else if (auto discovered = sapo::config::ProviderConfigStore::discover("."); discovered.has_value()) {
        services.provider_config = std::make_shared<sapo::config::ProviderConfigStore>(*discovered);
    }
    if (services.provider_config != nullptr) {
        for (const auto &problem : services.provider_config->validate()) {
            std::cerr << "sapoc: config: " << problem << "\n";
        }
        const json engine = services.provider_config->engine();
        if (engine.contains("max_node_visits") && engine["max_node_visits"].is_number()) {
            services.limits.max_node_visits = engine["max_node_visits"].get<size_t>();
        }
        if (engine.contains("max_depth") && engine["max_depth"].is_number()) {
            services.limits.max_depth = engine["max_depth"].get<size_t>();
        }
        if (engine.contains("default_timeout_ms") && engine["default_timeout_ms"].is_number()) {
            services.limits.default_timeout_ms = engine["default_timeout_ms"].get<int64_t>();
        }
    }
    if (const std::string max_visits = options.value("max-visits"); !max_visits.empty()) {
        services.limits.max_node_visits = static_cast<size_t>(std::strtoull(max_visits.c_str(), nullptr, 10));
    }

    if (const std::string state_dir = options.value("state-dir"); !state_dir.empty()) {
        std::error_code error;
        std::filesystem::create_directories(state_dir, error);
        if (error) fail("cannot create state directory '" + state_dir + "': " + error.message());
        services.state_store = std::make_shared<sapo::runtime::FileStateStore>(state_dir);
    }

    if (services.provider_config != nullptr) services.applySecretRedaction();
    return services;
}

/// Registers the blueprint under test plus anything the flags added.
std::string registerBlueprint(VirtualMachine &vm, const Options &options) {
    if (options.positional.empty()) fail("`" + options.command + "` needs a blueprint path");
    const std::string path = options.positional.front();
    return vm.addBlueprintFile(path);
}

void printOutcome(const Options &options, const ExecutionOutcome &outcome, VirtualMachine &vm) {
    if (options.has("json")) {
        json out = outcome.toJson();
        if (options.has("trace")) out["trace"] = vm.traceFor(outcome.execution_id);
        std::cout << out.dump(2) << "\n";
        return;
    }
    std::cout << "status:   " << outcome.status << "\n";
    std::cout << "workflow: " << outcome.workflow_id << "\n";
    std::cout << "session:  " << outcome.session_id << "\n";
    std::cout << "exec:     " << outcome.execution_id << "\n";
    std::cout << "nodes:    " << outcome.node_visits << " visits in " << outcome.elapsed_ms << "ms\n";
    if (!outcome.cursor.empty()) std::cout << "cursor:   " << outcome.cursor << "\n";
    if (!outcome.error.empty()) {
        std::cout << "error:    " << outcome.error;
        if (!outcome.error_code.empty()) std::cout << " [" << outcome.error_code << "]";
        if (!outcome.error_node.empty()) std::cout << " at node '" << outcome.error_node << "'";
        std::cout << "\n";
    }
    if (!outcome.warnings.empty()) {
        std::cout << "warnings:\n";
        for (const auto &warning : outcome.warnings) std::cout << "  - " << warning << "\n";
    }
    if (outcome.prompt.is_object() && !outcome.prompt.empty()) {
        std::cout << "prompt:   " << outcome.prompt.dump() << "\n";
    }
    if (outcome.output.is_object() && !outcome.output.empty()) {
        std::cout << "output:   " << outcome.output.dump() << "\n";
    } else if (outcome.context.is_object() && !outcome.context.empty()) {
        std::cout << "context:  " << outcome.context.dump() << "\n";
    }
    if (outcome.status == "awaiting_input") {
        std::cout << "\nresume with:  sapoc resume " << outcome.session_id << " --answer <value>"
                  << (options.value("state-dir").empty() ? " --state-dir <dir>" : "") << "\n";
    }
    if (options.has("trace")) {
        std::cout << "trace:\n" << vm.traceFor(outcome.execution_id).dump(2) << "\n";
    }
}

int statusExitCode(const std::string &status) {
    if (status == "failed") return 1;
    if (status == "awaiting_input" || status == "suspended") return 3;
    return 0;
}

/// `--wait` drives the scheduler in real time until the session leaves the parked
/// set — the same call loop a server would run on its own thread.
void pumpUntilIdle(VirtualMachine &vm, const Options &options, const std::string &session_id) {
    const int64_t tick_ms = std::max<int64_t>(1, std::atoll(options.value("tick-ms", "25").c_str()));
    const auto budget = sapo::util::parseDuration(options.value("max-time", "10s"))
                            .value_or(std::chrono::milliseconds(10000));
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(tick_ms));
        vm.tick();
        const auto snapshot = vm.session(session_id);
        if (!snapshot.has_value()) return;
        if (snapshot->status != "awaiting_input" && snapshot->status != "waiting") return;
    }
}

void printSessions(const Options &options, const std::vector<SessionSnapshot> &snapshots) {
    if (options.has("json")) {
        json out = json::array();
        for (const auto &snapshot : snapshots) out.push_back(snapshot.toJson());
        std::cout << out.dump(2) << "\n";
        return;
    }
    if (snapshots.empty()) {
        std::cout << "no sessions in the store\n";
        return;
    }
    std::cout << "SESSION                             WORKFLOW           STATUS          NODES  CURSOR\n";
    for (const auto &snapshot : snapshots) {
        std::cout << snapshot.session_id << "  " << snapshot.blueprint_id << "\n"
                  << "    " << snapshot.status << " · " << snapshot.node_visits << " visits · cursor "
                  << (snapshot.cursor.empty() ? "-" : snapshot.cursor) << " · updated " << snapshot.updated_ms << "\n";
        if (!snapshot.error.empty()) std::cout << "    error: " << snapshot.error << "\n";
        if (snapshot.resumable && snapshot.pending.is_object() && snapshot.pending.contains("prompt")) {
            std::cout << "    awaiting: " << snapshot.pending["prompt"].dump() << "\n";
        }
    }
}

int commandRun(VirtualMachine &vm, const Options &options) {
    vm.start();
    const std::string workflow_id = registerBlueprint(vm, options);
    StartSessionOptions session_options;
    session_options.session_id = options.value("session");
    session_options.start_node = options.value("start-node");
    session_options.correlation_id = options.value("correlation-id");
    session_options.persist = !options.has("no-persist");
    if (const std::string max_visits = options.value("max-visits"); !max_visits.empty()) {
        session_options.max_node_visits = static_cast<size_t>(std::strtoull(max_visits.c_str(), nullptr, 10));
    }
    json input = json::object();
    if (const std::string file = options.value("input"); !file.empty()) input = readFile(file);
    input = mergeVars(options.vars, std::move(input));

    const ExecutionOutcome outcome = vm.startSession(workflow_id, input, session_options);
    if (options.has("wait") && (outcome.status == "awaiting_input" || outcome.status == "suspended")) {
        pumpUntilIdle(vm, options, outcome.session_id);
    }
    printOutcome(options, outcome, vm);
    return statusExitCode(outcome.status);
}

int commandResume(VirtualMachine &vm, const Options &options) {
    if (options.positional.empty()) fail("`resume` needs a session id");
    vm.start();
    // A restarted engine needs the blueprint on disk again to interpret the checkpoint.
    for (int index = 1; index < static_cast<int>(options.positional.size()); ++index) {
        vm.addBlueprintFile(options.positional[static_cast<size_t>(index)]);
    }
    json input = json::object();
    if (const std::string file = options.value("input"); !file.empty()) input = readFile(file);
    if (const std::string answer = options.value("answer"); !answer.empty()) input = parseVarValue(answer);
    input = mergeVars(options.vars, std::move(input));

    const ExecutionOutcome outcome = vm.resumeSession(options.positional.front(), input);
    if (options.has("wait") && (outcome.status == "awaiting_input" || outcome.status == "suspended")) {
        pumpUntilIdle(vm, options, outcome.session_id);
    }
    printOutcome(options, outcome, vm);
    return statusExitCode(outcome.status);
}

int commandValidate(VirtualMachine &vm, const Options &options) {
    if (options.positional.empty()) fail("`validate` needs at least one blueprint path");
    size_t counted = 0;
    std::vector<std::string> problems;
    for (const auto &path : options.positional) {
        std::error_code error;
        if (std::filesystem::is_directory(path, error)) {
            counted += vm.addBlueprintDirectory(path);
        } else {
            vm.addBlueprintFile(path);
            ++counted;
        }
    }
    problems = vm.validateAll();
    if (options.has("json")) {
        std::cout << json{{"blueprints", counted}, {"problems", problems}}.dump(2) << "\n";
    } else if (problems.empty()) {
        std::cout << counted << " blueprint(s) valid\n";
        for (const auto &id : vm.workflows().ids()) std::cout << "  ok  " << id << "\n";
    } else {
        std::cout << counted << " blueprint(s), " << problems.size() << " problem(s):\n";
        for (const auto &problem : problems) std::cout << "  ✗ " << problem << "\n";
    }
    return problems.empty() ? 0 : 2;
}

} // namespace

int main(int argc, char **argv) {
    const Options options = parseArguments(argc, argv);
    if (options.command == "help" || options.command == "--help" || options.command == "-h") {
        printUsage();
        return 0;
    }
    if (options.command == "version" || options.command == "--version") {
        std::cout << "sapoc " << kVersion << " (Sapo Engine)\n";
        std::cout << "  http transport: "
#if defined(SAPO_ENABLE_CPR)
                  << "cpr (libcurl)"
#else
                  << "none (rebuild with -DSAPO_ENABLE_CPR=ON to enable http.* commands)"
#endif
                  << "\n";
        return 0;
    }

    TaskServices services;
    try {
        services = buildServices(options);
    } catch (const std::exception &error) {
        fail(error.what());
    }
    VirtualMachine vm(std::move(services));

    try {
        if (options.command == "run") return commandRun(vm, options);
        if (options.command == "resume") return commandResume(vm, options);
        if (options.command == "validate") return commandValidate(vm, options);

        if (options.command == "sessions") {
            vm.start();
            printSessions(options, vm.sessions());
            return 0;
        }
        if (options.command == "session") {
            if (options.positional.empty()) fail("`session` needs a session id");
            vm.start();
            const auto snapshot = vm.session(options.positional.front());
            if (!snapshot.has_value()) {
                if (options.has("json")) {
                    std::cout << json{{"error", "session not found"}, {"session_id", options.positional.front()}}.dump(2)
                              << "\n";
                } else {
                    std::cout << "no session '" << options.positional.front() << "' in the store\n";
                }
                return 1;
            }
            if (options.has("json")) {
                json out = snapshot->toJson();
                out["context"] = snapshot->context;
                std::cout << out.dump(2) << "\n";
            } else {
                printSessions(options, {*snapshot});
            }
            return 0;
        }
        if (options.command == "cancel") {
            if (options.positional.empty()) fail("`cancel` needs a session id");
            vm.start();
            const bool cancelled =
                vm.cancelSession(options.positional.front(), options.value("reason", "cancelled from the CLI"));
            std::cout << (cancelled ? "cancelled\n" : "nothing to cancel (unknown or finished session)\n");
            return cancelled ? 0 : 1;
        }
        if (options.command == "emit") {
            if (options.positional.empty()) fail("`emit` needs an event name");
            vm.start();
            json payload = json::object();
            payload = mergeVars(options.vars, std::move(payload));
            vm.publishEvent(options.positional.front(), payload);
            if (options.has("wait")) pumpUntilIdle(vm, options, options.value("session"));
            std::cout << "published '" << options.positional.front() << "' (" << payload.dump() << ")\n";
            return 0;
        }
        if (options.command == "describe") {
            for (int index = 0; index < static_cast<int>(options.positional.size()); ++index) {
                vm.addBlueprintFile(options.positional[static_cast<size_t>(index)]);
            }
            vm.start();
            const json summary = vm.describe();
            if (options.has("json")) {
                std::cout << summary.dump(2) << "\n";
            } else {
                std::cout << "Sapo Engine " << kVersion << "\n";
                for (auto it = summary.begin(); it != summary.end(); ++it) {
                    std::cout << it.key() << ": " << it.value().dump() << "\n";
                }
            }
            for (const auto &problem : vm.services().startupCheck()) std::cerr << "startup: " << problem << "\n";
            return 0;
        }
        if (options.command == "metrics") {
            vm.start();
            std::cout << vm.metrics().dump(2) << "\n";
            return 0;
        }
    } catch (const sapo::runtime::SapoError &error) {
        if (options.has("json")) {
            std::cout << json{{"status", "failed"},
                              {"error", error.message()},
                              {"error_code", error.codeString()},
                              {"data", error.data()}}
                             .dump(2)
                      << "\n";
        } else {
            std::cerr << "sapoc: " << error.message() << "\n";
            if (!error.nodeId().empty()) std::cerr << "  in node '" << error.nodeId() << "'\n";
            const json hints = error.data();
            if (hints.is_object() && !hints.empty()) std::cerr << "  detail: " << hints.dump() << "\n";
        }
        return 2;
    } catch (const std::exception &error) {
        fail(error.what());
    }
    std::cerr << "sapoc: unknown command '" << options.command << "' (see `sapoc help`)\n";
    return 2;
}
