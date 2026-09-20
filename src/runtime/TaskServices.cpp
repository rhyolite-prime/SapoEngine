//
//  Sapo Engine — service composition.
//
#include "runtime/TaskServices.hpp"

#include "parser/WorkflowParser.hpp"
#include "runtime/SapoError.hpp"

namespace sapo::runtime {

    TaskServices TaskServices::defaults() {
        TaskServices services;
        services.logger = obs::defaultLogger();
        services.metrics = obs::MetricsRegistry::global();
        services.traces = std::make_shared<obs::TraceRecorder>(services.logger);
        services.clock = defaultClock();
        // No transport is linked by default: HTTP nodes fail with a clear message
        // Default transport: CprTransport when built with -DSAPO_ENABLE_CPR=ON, NullTransport otherwise.
        services.transport = sapo::http::defaultTransport();
        services.capabilities = std::make_shared<capabilities::CapabilityRegistry>();
        auto composite = std::make_shared<CompositeBindingProvider>();
        composite->add(std::make_shared<EnvironmentBindingProvider>());
        services.bindings = composite;
        services.data_sources = data::DataSourceRegistry::withBuiltIns(services.transport);
        services.events = std::make_shared<EventBus>();
        services.scheduler = std::make_shared<Scheduler>(services.clock);
        services.state_store = std::make_shared<InMemoryStateStore>();
        // The registry knows the capability set, so an unregistered
        // `capability`/`command` is a parse-time error rather than a runtime surprise.
        sapo::parser::ParseOptions parse;
        parse.capabilities = services.capabilities.get();
        services.workflows = std::make_shared<WorkflowRegistry>(parse, services.clock);
        services.pool = sharedWorkerPool();
        services.provider_config = std::make_shared<config::ProviderConfigStore>();
        return services;
    }

    void TaskServices::applySecretRedaction() {
        if (provider_config == nullptr || logger == nullptr) return;
        for (const auto &secret : provider_config->redactList()) {
            if (!secret.empty()) logger->addSecret(secret);
        }
    }

    std::vector<std::string> TaskServices::startupCheck() const {
        std::vector<std::string> problems;

        if (capabilities != nullptr) {
            auto audit = capabilities->audit();
            problems.insert(problems.end(), audit.begin(), audit.end());
        }
        if (provider_config != nullptr && !provider_config->empty()) {
            auto config_problems = provider_config->validate();
            if (!config_problems.empty()) {
                for (auto &problem : config_problems) {
                    problems.push_back(problem + " (in " + provider_config->origin() + ")");
                }
            }
            // Providers named in the config must actually be loadable/registered.
            for (const auto &id : provider_config->providerIds()) {
                const std::string type = provider_config->providerType(id);
                if (type.empty()) continue;
                if (provider_config->providerDeferred(id)) {
                    problems.push_back("provider '" + id + "' (type " + type + ") is marked deferred; any capability "
                                       "under it will fail at runtime");
                }
            }
            if (data_sources != nullptr) {
                std::vector<data::DataSourceConfig> declared;
                for (const auto &entry : provider_config->dataSourceDeclarations()) {
                    data::DataSourceConfig config;
                    config.name = entry.is_object() ? entry.value("name", "") : "";
                    config.provider = entry.is_object() ? entry.value("provider", "") : "";
                    config.scope = entry.is_object() ? entry.value("scope", "external") : "external";
                    if (entry.is_object() && entry.contains("config")) config.config = entry["config"];
                    config.origin = provider_config->origin();
                    declared.push_back(std::move(config));
                }
                auto source_problems = data_sources->validate(declared);
                problems.insert(problems.end(), source_problems.begin(), source_problems.end());
            }
        }
        if (workflows != nullptr) {
            auto references = workflows->unresolvedReferences();
            problems.insert(problems.end(), references.begin(), references.end());
        }
        if (pool != nullptr && pool->maxThreads() == 0) {
            problems.push_back("worker pool has no threads configured");
        }
        return problems;
    }

} // namespace sapo::runtime
