#include <drogon/drogon.h>
#include <iostream>
#include <memory>

#include "adapters/DrogonHttpTransport.hpp"
#include "controllers/UssdWorkflowController.hpp"
#include "observability/Logger.hpp"
#include "runtime/StateStore.hpp"
#include "runtime/VirtualMachine.hpp"

#if defined(SAPO_ENABLE_REDIS)
#include "redis/RedisStateStore.hpp"
#include "redis/SocketRedisClient.hpp"
#endif

using namespace sapo::runtime;

int main(int argc, char *argv[]) {
    std::cout << "[Sapo Drogon Host] Initializing Sapo Engine Host Service..." << std::endl;

    // 1. Compose Sapo TaskServices
    TaskServices services = TaskServices::defaults();

    // Logger
    auto logger = std::make_shared<sapo::obs::Logger>();
    logger->setLevel(sapo::obs::LogLevel::Info);
    services.logger = logger;

    // Outbound HTTP Transport: bridge to Drogon's native HttpClient
    // This allows blueprint http.* nodes to use Drogon's connection pool
    services.transport = std::make_shared<drogon_sapo::DrogonHttpTransport>();

    // State Store: choose FileStateStore (single-node) or RedisStateStore (fleet)
    const char *redis_url_env = std::getenv("SAPO_REDIS_HOST");
    if (redis_url_env) {
#if defined(SAPO_ENABLE_REDIS)
        const std::string host = redis_url_env;
        const int port = 6379;
        std::cout << "[Sapo Drogon Host] Connecting to Redis state store at " << host << ":" << port << std::endl;
        auto redis_client = std::make_shared<sapo::redis::SocketRedisClient>(host, port);
        sapo::redis::RedisStateStoreOptions redis_opts;
        redis_opts.ttl_seconds = 900; // 15-minute session expiration
        services.state_store = std::make_shared<sapo::redis::RedisStateStore>(redis_client, redis_opts);
#else
        std::cerr << "[Sapo Drogon Host] Warning: SAPO_REDIS_HOST set, but engine built without SAPO_ENABLE_REDIS. "
                  << "Falling back to FileStateStore." << std::endl;
        services.state_store = std::make_shared<FileStateStore>("/tmp/sapo_sessions");
#endif
    } else {
        std::cout << "[Sapo Drogon Host] Using FileStateStore at /tmp/sapo_sessions" << std::endl;
        services.state_store = std::make_shared<FileStateStore>("/tmp/sapo_sessions");
    }

    // 2. Instantiate VirtualMachine
    auto vm = std::make_shared<VirtualMachine>(std::move(services));

    // Optional config path and workflows directory
    const std::string config_path = argc > 1 ? argv[1] : "sapo-config.json";
    if (std::filesystem::exists(config_path)) {
        vm->setConfigPath(config_path);
    }
    vm->setWorkflowDirectory("workflows");

    // 3. Start engine and audit
    std::cout << "[Sapo Drogon Host] Starting Virtual Machine audit and initialization..." << std::endl;
    std::vector<std::string> problems = vm->start();
    if (!problems.empty()) {
        std::cerr << "[Sapo Drogon Host] Startup audit encountered warnings/problems:" << std::endl;
        for (const auto &problem : problems) {
            std::cerr << "  - " << problem << std::endl;
        }
        // In strict production setups, abort if critical problems exist
    }

    // 4. Start background scheduler tick (for timers, wait nodes, cron triggers)
    vm->startBackgroundTick(std::chrono::milliseconds(250));

    // 5. Register Graceful Shutdown
    drogon::app().registerSyncAdvice([vm]() {
        std::cout << "[Sapo Drogon Host] Gracefully shutting down Sapo Engine..." << std::endl;
        vm->stop();
    });

    // 6. Inject VM into Drogon Controllers
    drogon_sapo::UssdWorkflowController::setVirtualMachine(vm);

    // 7. Configure and Launch Drogon
    std::cout << "[Sapo Drogon Host] Starting Drogon HTTP server on 0.0.0.0:8080..." << std::endl;
    drogon::app()
        .addListener("0.0.0.0", 8080)
        .setThreadNum(4)
        .run();

    return 0;
}
