# Sapo Engine — Drogon C++ Integration & Library Export Guide

This guide details how to compile, export, and embed **Sapo Engine** (`libsapo_core.a`) as an in-process library inside a [Drogon C++](https://drogon.org) web service.

---

## 1. Architectural Overview

```
                      +---------------------------------------+
                      |       Drogon HTTP Gateway Host        |
                      |  (Listeners, Epoll I/O Event Loops)   |
                      +-------------------+-------------------+
                                          |
                                   drogon::async_run
                                          |
                      +-------------------v-------------------+
                      |      Drogon Worker Thread Pool        |
                      +-------------------+-------------------+
                                          |
                                          | In-process C++ API
                                          v
+-----------------------------------------------------------------------------------+
| Sapo Engine (`libsapo_core.a` / `Sapo::core`)                                     |
|                                                                                   |
|  +-----------------------------------------------------------------------------+  |
|  |                     sapo::runtime::VirtualMachine                           |  |
|  |   - startSession(workflow_id, input, options) -> ExecutionOutcome           |  |
|  |   - resumeSession(session_id, input)          -> ExecutionOutcome           |  |
|  +--------------------------------------+--------------------------------------+  |
|                                         |                                         |
|               +-------------------------+-------------------------+               |
|               |                                                   |               |
|      +--------v--------+                                 +--------v--------+      |
|      |  Interpreter    |                                 |  TaskServices   |      |
|      |  (AST Traversal)|                                 |  (DI Seams)     |      |
|      +--------+--------+                                 +--------+--------+      |
|               |                                                   |               |
+---------------|---------------------------------------------------|---------------+
                |                                                   |
                |                                      +------------+------------+
                |                                      |                         |
        +-------v-------+                      +-------v-------+         +-------v-------+
        | RuntimeContext|                      |  StateStore   |         | IHttpTransport|
        | (Memory Arena)|                      | (File / Redis)|         | (Drogon / CPR)|
        +---------------+                      +---------------+         +---------------+
```

### Why In-Process Library Embedding?
- **Zero Network Overhead:** No HTTP/gRPC serialization penalty between API gateway and DSL execution. Execution on warm ASTs takes < 1 ms.
- **USSD SLA Compliance:** Telco aggregators impose strict 2-second timeout windows; in-process execution preserves budget for downstream banking/database calls.
- **Unified Process Footprint:** Single deployable binary containing both HTTP endpoints and DSL runtime.

---

## 2. Building & Exporting Sapo as a Library

### 2.1 Compiler & System Requirements

| Dependency | Minimum Version | Notes |
|---|---|---|
| **CMake** | `>= 3.28` | Required for C++23 module/target support |
| **C++ Compiler** | `GCC >= 13` or `Clang >= 17` | Sapo Engine utilizes C++23 features (`<expected>`, `std::string_view`, etc.) |
| **POSIX Threads** | System | Required by Drogon and Sapo `WorkerPool` |
| **libcurl headers** | Optional (>= 7.64) | Only required if compiling vendored CPR (`-DSAPO_ENABLE_CPR=ON`) |

### 2.2 CMake Configuration Flags

When building Sapo Engine as a library for Drogon, configure using:

| Option | Recommended for Drogon | Description |
|---|---|---|
| `SAPO_ENABLE_CPR` | **`OFF`** (Recommended) | Compiles vendored libcurl wrapper. When embedding in Drogon, it is recommended to keep this `OFF` and bridge `sapo::http::IHttpTransport` to Drogon's native `HttpClient` instead. |
| `SAPO_ENABLE_REDIS` | **`ON`** | Compiles the built-in POSIX RESP2 client backing `RedisStateStore` (zero external dependencies). |
| `SAPO_BUILD_TESTS` | **`OFF`** | Skip compiling the Catch2 test suite to speed up compilation. |
| `CMAKE_BUILD_TYPE` | **`Release`** | Enables full optimization (`-O3`). |

### 2.3 Compilation Commands

```bash
# Clone the repository
git clone https://github.com/rhyolite-prime/SapoEngine.git
cd SapoEngine

# Configure for Release
cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DSAPO_BUILD_TESTS=OFF \
      -DSAPO_ENABLE_REDIS=ON \
      -DSAPO_ENABLE_CPR=OFF

# Build the static library
cmake --build build -j"$(nproc)"
```

The primary output is `build/libsapo_core.a`.

---

## 3. Library Export & Consumption Models

There are three ways to integrate Sapo Engine into your Drogon project:

### Model A: In-Tree Submodule / `add_subdirectory` (Recommended)

Add SapoEngine as a Git submodule or vendor directory inside your Drogon project:

```bash
cd my_drogon_project
git submodule add https://github.com/rhyolite-prime/SapoEngine.git vendor/SapoEngine
```

In your Drogon `CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.28)
project(my_drogon_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Drogon REQUIRED)
find_package(Threads REQUIRED)

# Sapo Engine configuration
set(SAPO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SAPO_ENABLE_CPR   OFF CACHE BOOL "" FORCE)
set(SAPO_ENABLE_REDIS ON  CACHE BOOL "" FORCE)
add_subdirectory(vendor/SapoEngine sapo_build EXCLUDE_FROM_ALL)

add_executable(my_drogon_app src/main.cc ...)
target_link_libraries(my_drogon_app PRIVATE Drogon::Drogon Sapo::core)
```

**Why Model A is recommended:**
- Automatic propagation of compile definitions and standard requirements (`cxx_std_23`).
- Single-command build; no system installation steps or root privileges required.
- Bypasses CMake package export complexities when vendoring third-party subtargets.

---

### Model B: System Installation & `find_package`

Install Sapo Engine to `/usr/local` or `/opt/sapo`:

```bash
# Build with CPR=OFF for clean CMake package export
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSAPO_ENABLE_CPR=OFF -DSAPO_ENABLE_REDIS=ON -DSAPO_BUILD_TESTS=OFF
cmake --build build -j"$(nproc)"

# Install headers, library, and CMake package config
sudo cmake --install build --prefix /opt/sapo
```

Installed structure:
```
/opt/sapo/
├── include/
│   └── sapo/
│       ├── capabilities/...
│       ├── config/...
│       ├── http/...
│       ├── parser/...
│       ├── redis/...
│       ├── runtime/...
│       └── third_party/nlohmann/json.hpp
├── lib/
│   ├── libsapo_core.a
│   └── cmake/SapoEngine/
│       ├── SapoEngineConfig.cmake
│       ├── SapoEngineConfigVersion.cmake
│       └── SapoEngineTargets.cmake
└── share/
    └── sapo/*.json
```

In your Drogon `CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.28)
project(my_drogon_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Drogon REQUIRED)
find_package(SapoEngine REQUIRED PATHS /opt/sapo)

add_executable(my_drogon_app src/main.cc ...)
target_link_libraries(my_drogon_app PRIVATE Drogon::Drogon Sapo::core)
```

> **Packaging Note on CPR:** If building Sapo with `-DSAPO_ENABLE_CPR=ON`, the vendored `cpr` target is not part of CMake's installed export set. If you need CPR-backed HTTP, use **Model A** or **Model C**, or use the native `DrogonHttpTransport` shown in §6.

---

### Model C: Precompiled Archive Linking (`libsapo_core.a`)

If linking against a pre-built static archive without CMake package discovery:

```cmake
cmake_minimum_required(VERSION 3.28)
project(my_drogon_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Drogon REQUIRED)
find_package(Threads REQUIRED)

add_executable(my_drogon_app src/main.cc ...)

# Point to Sapo include paths
target_include_directories(my_drogon_app PRIVATE
    /path/to/SapoEngine/src
    /path/to/SapoEngine/src/third_party
)

# Link libsapo_core.a and system libraries
target_link_libraries(my_drogon_app PRIVATE
    Drogon::Drogon
    /path/to/SapoEngine/build/libsapo_core.a
    Threads::Threads
)
```

---

## 4. Host Service Lifecycle (`main.cc`)

The host lifecycle coordinates `TaskServices`, `VirtualMachine`, state persistence, and Drogon's event loop.

```cpp
#include <drogon/drogon.h>
#include <iostream>
#include <memory>

#include "runtime/VirtualMachine.hpp"
#include "runtime/StateStore.hpp"
#include "observability/Logger.hpp"

#if defined(SAPO_ENABLE_REDIS)
#include "redis/RedisStateStore.hpp"
#include "redis/SocketRedisClient.hpp"
#endif

using namespace sapo::runtime;

// Global or dependency-injected pointer
static std::shared_ptr<VirtualMachine> g_vm;

int main() {
    // 1. Configure TaskServices
    TaskServices services = TaskServices::defaults();

    // Logger
    auto logger = std::make_shared<sapo::obs::Logger>();
    logger->setLevel(sapo::obs::LogLevel::Info);
    services.logger = logger;

    // State Store (Choose File or Redis)
    const char* redis_host = std::getenv("REDIS_HOST");
    if (redis_host) {
        auto redis_client = std::make_shared<sapo::redis::SocketRedisClient>(redis_host, 6379);
        sapo::redis::RedisStateStoreOptions redis_opts;
        redis_opts.ttl_seconds = 900; // 15-minute session timeout
        services.state_store = std::make_shared<sapo::redis::RedisStateStore>(redis_client, redis_opts);
    } else {
        services.state_store = std::make_shared<FileStateStore>("/var/lib/sapo/sessions");
    }

    // 2. Instantiate VirtualMachine
    g_vm = std::make_shared<VirtualMachine>(std::move(services));
    g_vm->setConfigPath("/etc/sapo/sapo-config.json");
    g_vm->setWorkflowDirectory("/etc/sapo/workflows");

    // 3. Start engine & audit configuration
    std::vector<std::string> problems = g_vm->start();
    if (!problems.empty()) {
        std::cerr << "Sapo Engine startup validation warnings:" << std::endl;
        for (const auto& problem : problems) {
            std::cerr << "  - " << problem << std::endl;
        }
    }

    // 4. Start background scheduler (fires due timers, wait nodes, cron jobs)
    g_vm->startBackgroundTick(std::chrono::milliseconds(250));

    // 5. Register Graceful Shutdown Advice with Drogon
    drogon::app().registerSyncAdvice([]() {
        std::cout << "Gracefully shutting down Sapo VM..." << std::endl;
        if (g_vm) g_vm->stop();
    });

    // 6. Launch Drogon Web Server
    drogon::app()
        .addListener("0.0.0.0", 8080)
        .setThreadNum(4)
        .run();

    return 0;
}
```

---

## 5. Calling Sapo Engine Methods

### 5.1 Critical Threading Rule: Offloading I/O Threads

> ⚠️ **CRITICAL WARNING:** `startSession()`, `resumeSession()`, and `runBlueprint()` execute **synchronously on the calling thread**.
>
> Drogon handlers run on an Epoll/kqueue I/O thread loop by default. Calling `startSession` or `resumeSession` directly on an I/O thread blocks that event loop, starving other HTTP connections.
>
> **Always offload execution using `drogon::async_run` within C++ coroutines (`drogon::Task<>`):**

```cpp
sapo::runtime::ExecutionOutcome outcome;
co_await drogon::async_run([&] {
    outcome = g_vm->startSession(workflow_id, input, options);
});
```

---

### 5.2 Core API Method Reference

#### 1. Start a Session: `vm.startSession(...)`
```cpp
sapo::runtime::StartSessionOptions options;
options.session_id     = "telco-sess-449102"; // External session ID (e.g. USSD)
options.correlation_id = "corr-uuid-991201";

nlohmann::json input = {
    {"msisdn", "233201234567"},
    {"service_code", "*920#"}
};

sapo::runtime::ExecutionOutcome outcome = vm.startSession("ussd_banking", input, options);
```

#### 2. Resume a Suspended Session: `vm.resumeSession(...)`
When a workflow node has `await_input: true`, it pauses and saves state to the state store. When the user replies with input, resume it:
```cpp
// Pass user reply string, number, or object
nlohmann::json user_input = "1"; // User selected menu item 1

sapo::runtime::ExecutionOutcome outcome = vm.resumeSession(session_id, user_input);
```

#### 3. Run a Raw JSON Blueprint: `vm.runBlueprint(...)`
Used for executing ad-hoc or dynamic blueprints without saving to disk first:
```cpp
nlohmann::json blueprint = nlohmann::json::parse(raw_json_str);
sapo::runtime::ExecutionOutcome outcome = vm.runBlueprint(blueprint, input, options);
```

#### 4. Inspect or Cancel Sessions
```cpp
// Inspect session checkpoint
std::optional<sapo::runtime::SessionSnapshot> snap = vm.session(session_id);

// Cancel an active/parked session
bool cancelled = vm.cancelSession(session_id, "user timed out or dialled cancel");
```

#### 5. Publishing Events
Trigger workflows listening for event patterns (e.g., `payment.confirmed`):
```cpp
vm.publishEvent("payment.confirmed", {
    {"account_id", "ACC-1002"},
    {"amount", 150.00}
});
```

---

### 5.3 Handling `ExecutionOutcome`

Every workflow execution returns an `ExecutionOutcome` struct:

```cpp
struct ExecutionOutcome {
    std::string session_id;     // Active session ID
    std::string execution_id;   // Unique run ID
    std::string workflow_id;    // Blueprint name
    std::string status;         // "completed" | "terminated" | "awaiting_input" | "suspended" | "failed" | "cancelled"
    bool ok;                    // true if status != "failed"
    nlohmann::json output;      // Payload from terminate / finish nodes
    nlohmann::json prompt;      // Prompt object when awaiting_input: {"message": "...", "interaction_type": "..."}
    nlohmann::json context;     // Full variable memory snapshot
    std::string error;          // Human-readable error message
    std::string error_code;     // Sapo error code (e.g. "WORKFLOW_NOT_FOUND", "TIMEOUT")
    std::string error_node;     // Node ID where failure occurred
    int64_t elapsed_ms;         // Wall-clock run time
};
```

#### Mapping Outcome to HTTP Status Codes

| Sapo Status | Meaning | Recommended HTTP Code | Gateway / USSD Action |
|---|---|---|---|
| `awaiting_input` | Parked waiting for user response | `202 Accepted` | Return USSD menu with `Type: Response` (keep open) |
| `completed` / `terminated` | Workflow finished | `200 OK` | Return final message with `Type: Release` (close session) |
| `suspended` | Parked on timer or external event | `202 Accepted` | Webhook/Job acknowledged, waiting asynchronously |
| `failed` | Error during execution | `404` (not found) / `500` | Return user-friendly error message, close session |

---

## 6. Drogon Outbound HTTP Transport Adapter

Blueprints executing `http.get`, `http.post`, etc., call Sapo's `IHttpTransport` seam. Rather than linking CPR and `libcurl`, you can route outbound requests through Drogon's native `HttpClient`:

```cpp
// DrogonHttpTransport.hpp
#pragma once
#include "http/IHttpTransport.hpp"
#include <drogon/HttpClient.h>

class DrogonHttpTransport final : public sapo::http::IHttpTransport {
public:
    sapo::http::Response send(const sapo::http::Request &request) override;
    std::string name() const override { return "drogon"; }
};
```

```cpp
// DrogonHttpTransport.cc
#include "DrogonHttpTransport.hpp"

sapo::http::Response DrogonHttpTransport::send(const sapo::http::Request &request) {
    sapo::http::Response out;
    // 1. Create or retrieve pooled Drogon HttpClient for host
    auto client = drogon::HttpClient::newHttpClient(request.url);

    // 2. Prepare request
    auto d_req = drogon::HttpRequest::newHttpRequest();
    d_req->setMethod(request.method == "POST" ? drogon::Post : drogon::Get);
    if (request.body) {
        d_req->setBody(request.body->is_string() ? request.body->get<std::string>() : request.body->dump());
    }

    // 3. Send synchronously on Sapo worker thread
    const double timeout = request.timeout_ms / 1000.0;
    auto [result, resp] = client->sendRequest(d_req, timeout);

    if (result == drogon::ReqResult::Ok && resp) {
        out.status_code = static_cast<int>(resp->getStatusCode());
        out.body = std::string(resp->getBody());
    } else {
        out.transport_error = "Drogon HTTP client failure";
    }
    return out;
}
```

Plug it into `TaskServices` during startup:
```cpp
services.transport = std::make_shared<DrogonHttpTransport>();
```

---

## 7. Complete Drogon USSD Controller Example

```cpp
#include <drogon/HttpController.h>
#include <drogon/utils/Coroutine.h>
#include "runtime/VirtualMachine.hpp"

class UssdController : public drogon::HttpController<UssdController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(UssdController::handleUssd, "/ussd", drogon::Post);
    METHOD_LIST_END

    drogon::Task<drogon::HttpResponsePtr> handleUssd(drogon::HttpRequestPtr req) {
        auto body = nlohmann::json::parse(req->getBody());
        std::string session_id = body["SessionId"];
        std::string user_input = body["Message"];
        bool is_initiation     = (body["Type"] == "Initiation" || user_input.front() == '*');

        sapo::runtime::ExecutionOutcome outcome;
        co_await drogon::async_run([&] {
            if (is_initiation) {
                sapo::runtime::StartSessionOptions opts;
                opts.session_id = session_id;
                outcome = g_vm->startSession("daccu_ussd", {{"msisdn", body["Mobile"]}}, opts);
            } else {
                outcome = g_vm->resumeSession(session_id, user_input);
            }
        });

        nlohmann::json ussd_reply;
        ussd_reply["SessionId"] = session_id;
        if (outcome.status == "awaiting_input") {
            ussd_reply["Type"] = "Response"; // Keep session open
            ussd_reply["Message"] = outcome.prompt.value("message", "Continue:");
        } else {
            ussd_reply["Type"] = "Release";  // Terminate session
            ussd_reply["Message"] = outcome.output.value("message", "Thank you for using our service.");
        }

        auto resp = drogon::HttpResponse::newHttpJsonResponse(ussd_reply);
        co_return resp;
    }
};
```

---

## 8. Summary Checklist for Production

- [ ] Drogon and Sapo compiled with matching standard: **C++23** (`CMAKE_CXX_STANDARD 23`).
- [ ] Engine calls executed off Drogon I/O threads using **`drogon::async_run`**.
- [ ] State Store selected: `FileStateStore` for single-node, `RedisStateStore` with TTL for multi-node clusters.
- [ ] Background tick scheduled: `vm->startBackgroundTick(std::chrono::milliseconds(250))`.
- [ ] Graceful shutdown advice registered: `app().registerSyncAdvice([&]{ vm->stop(); });`.
- [ ] Outbound transport: Use `DrogonHttpTransport` to keep dependencies minimal and reuse connection pools.
