# Deploying & Integrating Sapo Engine

Sapo Engine ships two production deployment models:

| | **A. Embedded library** | **B. Standalone API service** |
|---|---|---|
| What runs | `libsapo_core.a` linked into *your* process (e.g. a Drogon API) | a separate `sapo-server` process; your API talks HTTP |
| Latency | none (in-process calls) | +1 network hop per start/resume |
| Sessions | in your process' state store | in the server's state store |
| Upgrade story | rebuild + redeploy your API | redeploy the engine service independently |
| Failure isolation | an engine bug takes your API down | engine crash is contained |
| Clients | C++ only | anything that speaks HTTP |
| Status | **available today** (`Sapo::core`) | planned (`implementation_plan_2.md` T4.3); a reference Drogon implementation is provided in [`examples/drogon`](../examples/drogon) |

**Rule of thumb:** if your API layer is already C++/Drogon (e.g. `WssdApi`), embed the
library — it is the architecture this engine was designed for, and USSD-style latency budgets
(2-second gateway windows) leave no room for an extra hop. Run the engine as a separate
service only when non-C++ clients need it or you need independent scaling/isolation.

Both modes expose the *same* semantics, because everything is a driver over one facade:
`sapo::runtime::VirtualMachine` (`src/runtime/VirtualMachine.hpp`).

---

## 1. Building the engine for production

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSAPO_ENABLE_CPR=ON -DSAPO_BUILD_TESTS=OFF
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure          # optional, pre-deploy gate
```

- `-DSAPO_ENABLE_CPR=ON` compiles the libcurl-backed HTTP transport so `http.*` blueprint
  commands work. Needs libcurl dev headers (`apt install libcurl4-openssl-dev`).
  Leave it `OFF` if you inject your own `IHttpTransport` (see §5).
- Artifacts: `build/libsapo_core.a`, `build/sapoc`.

Install (headers + static lib + CMake package config + schemas):

```bash
sudo cmake --install build --prefix /opt/sapo
# /opt/sapo/lib/libsapo_core.a
# /opt/sapo/include/sapo/...                     (headers, incl. third_party/nlohmann)
# /opt/sapo/lib/cmake/SapoEngine/                (SapoEngineConfig.cmake → Sapo::core)
# /opt/sapo/share/sapo/*.json                    (blueprint schemas)
```

> **Packaging caveat:** the installed CMake package is clean only when built with
> `-DSAPO_ENABLE_CPR=OFF`. With CPR enabled, the vendored `cpr` target is not exported,
> so downstream `find_package(SapoEngine)` fails. For CPR builds, consume the engine with
> `add_subdirectory()` instead (§2.1) — or link `build/libsapo_core.a` + `libcpr.a`
> directly, as the `WssdApi` plan does.

---

## 2. Mode A — embed `sapo_core` in your Drogon API (recommended)

### 2.1 CMake wiring

**Option 1 — `add_subdirectory` (simplest, works with CPR):** vendor the SapoEngine
checkout (git submodule) inside your Drogon project:

```cmake
find_package(Drogon REQUIRED)

set(SAPO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SAPO_ENABLE_CPR  ON  CACHE BOOL "" FORCE)
add_subdirectory(vendor/SapoEngine sapo-engine EXCLUDE_FROM_ALL)

target_link_libraries(my_api PRIVATE Drogon::Drogon Sapo::core)
```

**Option 2 — installed package:** build the engine with `-DSAPO_ENABLE_CPR=OFF`,
`cmake --install` it, then:

```cmake
find_package(SapoEngine REQUIRED PATHS /opt/sapo)
target_link_libraries(my_api PRIVATE Drogon::Drogon Sapo::core)
```

and inject a transport yourself (§5) so `http.*` nodes still work.

Notes:

- `Sapo::core` propagates `cxx_std_23`, so your Drogon target compiles as C++23.
  Use Drogon ≥ 1.9 (recent releases compile cleanly under C++23).
- `Sapo::core` also propagates the vendored `nlohmann/json` include path
  (`include/sapo/third_party`), so `#include <nlohmann/json.hpp>` just works.
- Engine headers are included relative to `src/` (or the installed `include/sapo`):
  `#include <runtime/VirtualMachine.hpp>`.

### 2.2 Host composition

Mirror what `sapoc` does (`src/main.cpp` `buildServices`): build a `TaskServices`, then
hand it to the VM. Everything is injectable, which is what makes Drogon integration clean:

```cpp
#include "runtime/VirtualMachine.hpp"
#include "runtime/StateStore.hpp"
#include "observability/Logger.hpp"

using namespace sapo::runtime;

TaskServices services = TaskServices::defaults();

auto logger = std::make_shared<sapo::obs::Logger>();
logger->setLevel(sapo::obs::LogLevel::Info);
logger->addSink(std::make_shared<sapo::obs::JsonLineSink>(/* file/stream */));
services.logger = logger;

services.state_store = std::make_shared<FileStateStore>("/var/lib/sapo/sessions");

VirtualMachine vm(std::move(services));
vm.setConfigPath("/etc/sapo/sapo-config.json");   // providers, secrets, engine limits
vm.setWorkflowDirectory("/etc/sapo/workflows");   // auto-registered on start()

std::vector<std::string> problems = vm.start();   // validate + wire triggers/scheduler
// abort startup if `problems` is non-empty

vm.startBackgroundTick(std::chrono::milliseconds(250));  // fires timers/cron/event wakeups
// ... serve traffic ...
vm.stop();                                        // at shutdown
```

Key lifecycle facts:

- Register blueprints at startup (`setWorkflowDirectory`, `addBlueprintFile/Directory`,
  or `workflows:` in `sapo-config.json`). **Do not mutate the workflow registry while
  serving traffic** — the registry is not lock-protected for concurrent reads + writes.
- A suspended session is *data in the state store*, not a thread. `resumeSession()`
  works across process restarts as long as the blueprint is registered again and the
  store is intact.
- `start()` returns the startup audit (capability/provider/data-source problems); treat
  non-empty as a deploy failure, like a failed migration.

### 2.3 Calling the engine from Drogon handlers

`startSession` / `resumeSession` execute **synchronously on the calling thread** until the
session completes or suspends. Never call them directly on Drogon IO threads — hop off first.
With Drogon ≥ 1.9, `drogon::async_run` + a coroutine handler is the clean pattern:

```cpp
#include <drogon/drogon.h>
#include <drogon/utils/Coroutine.h>
#include <nlohmann/json.hpp>
#include "runtime/VirtualMachine.hpp"

static std::unique_ptr<sapo::runtime::VirtualMachine> g_vm;   // created in main()

drogon::HttpResponsePtr jsonResponse(const nlohmann::json& body,
                                     drogon::HttpStatusCode code = drogon::k200OK) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(code);
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->setBody(body.dump());
    return resp;
}

drogon::HttpResponsePtr outcomeResponse(const sapo::runtime::ExecutionOutcome& o) {
    drogon::HttpStatusCode code = drogon::k200OK;
    if (o.status == "awaiting_input" || o.status == "suspended")
        code = drogon::k202Accepted;                       // parked; body carries prompt + session_id
    else if (o.status == "failed")
        code = (o.error_code == "WORKFLOW_NOT_FOUND" || o.error_code == "SESSION_NOT_FOUND")
                   ? drogon::k404NotFound : drogon::k500InternalServerError;
    return jsonResponse(o.toJson(), code);
}

void registerSapoRoutes() {
    using namespace drogon;

    // POST /workflows/{workflow}/runs   {"input": {...}, "session_id": "optional-fixed-id"}
    app().registerHandler(
        "/workflows/{1}/runs",
        [](const HttpRequestPtr& req,
           std::function<void(const HttpResponsePtr&)>&& callback,
           std::string workflow_id) -> Task<> {
            nlohmann::json body = nlohmann::json::object();
            if (!req->getBody().empty()) {
                try { body = nlohmann::json::parse(req->getBody()); }
                catch (const nlohmann::json::exception&) {
                    callback(jsonResponse({{"error", "body is not valid JSON"}}, k400BadRequest));
                    co_return;
                }
            }
            sapo::runtime::StartSessionOptions options;
            options.session_id     = body.value("session_id", "");       // e.g. telco SESSIONID
            options.correlation_id = body.value("correlation_id", "");

            sapo::runtime::ExecutionOutcome outcome;
            nlohmann::json input = body.value("input", nlohmann::json::object());
            co_await drogon::async_run([&] {                     // off the IO thread
                outcome = g_vm->startSession(workflow_id, input, options);
            });
            callback(outcomeResponse(outcome));
        },
        {Post});

    // POST /sessions/{session}/input    {"input": {...}}  — answer a parked session
    app().registerHandler(
        "/sessions/{1}/input",
        [](const HttpRequestPtr& req,
           std::function<void(const HttpResponsePtr&)>&& callback,
           std::string session_id) -> Task<> {
            nlohmann::json input = nlohmann::json::object();
            try {
                auto body = nlohmann::json::parse(req->getBody());
                input = body.value("input", body);
            } catch (const nlohmann::json::exception&) {
                callback(jsonResponse({{"error", "body is not valid JSON"}}, k400BadRequest));
                co_return;
            }
            sapo::runtime::ExecutionOutcome outcome;
            co_await drogon::async_run([&] { outcome = g_vm->resumeSession(session_id, input); });
            callback(outcomeResponse(outcome));
        },
        {Post});
}
```

The engine returns *results, not exceptions* across the API boundary (an unknown workflow
comes back as `status: "failed"`, `error_code: "WORKFLOW_NOT_FOUND"`), so the mapping above
is all you need. `ExecutionOutcome::toJson()` is ready to hand to clients:
`status`, `session_id`, `execution_id`, `prompt`, `output`, `context`, `error*`, `node_visits`,
`elapsed_ms`.

USSD-style flow (matching `Re-architecting USSD with Sapo DSL Engine.md`):

1. First request: `POST /workflows/{flow}/runs` with `"session_id": "<SESSIONID>"` and
   `{msisdn, network, ...}` as input → response is `awaiting_input` + `prompt`.
2. Each follow-up: `POST /sessions/<SESSIONID>/input` with the user's answer →
   next `prompt`, until `status` is `completed`/`terminated`.

A complete, runnable service with these endpoints (plus sessions listing, cancel, events,
`/healthz`, `/metrics`) is in [`examples/drogon`](../examples/drogon).

### 2.4 Concurrency & scaling notes

- Different sessions may be driven concurrently; per-execution state is stack-local and the
  shared bits (state store, metrics, logger, scheduler/event bookkeeping) are mutex-guarded.
  Still: **register blueprints before serving traffic**, and load-test your concurrency
  profile before relying on it — synchronous HTTP nodes inside blueprints hold the worker
  for their duration.
- Keep blueprint steps short (USSD's 2s window). Anything slow belongs in a `schedule`/
  event-triggered background blueprint, not in the request path.
- Need strict isolation or more parallelism? Run **N `VirtualMachine` instances** and shard
  sessions across them (e.g. hash of MSISDN). Each VM owns its own interpreter, registry,
  event bus and store — no sharing, no global locks.
- `services.limits` (per-run and config-file tunables: `max_node_visits`, `max_depth`,
  `default_timeout_ms`, …) is your DoS protection for user-supplied flows.

---

## 3. Mode B — standalone API service (`sapo-server`)

The service-mode API is designed in `implementation_plan_2.md` (T4.3) but not yet part of
the tree; `src/server/` does not exist yet. The intended surface:

```
POST   /workflows/{id}/runs     start a session (blueprint id + input)
POST   /sessions/{id}/input     resume with user input
GET    /sessions/{id}           snapshot (checkpoint + pending prompt)
DELETE /sessions/{id}           cancel
GET    /healthz                 liveness
GET    /metrics                 Prometheus-format counters
(+ webhook receiver, per T2.4)
```

**Until T4.3 lands, use [`examples/drogon`](../examples/drogon) as the reference
`sapo-server`**: it is exactly that endpoint set wired to an embedded VM. Deploy it as its
own binary (systemd unit below) and have your main Drogon API consume it through
`drogon::HttpClient`.

```ini
# /etc/systemd/system/sapo-server.service
[Unit]
Description=Sapo Engine service
After=network.target

[Service]
ExecStart=/opt/sapo/bin/sapo_service --port 8090 \
    --config /etc/sapo/sapo-config.json \
    --workflows /etc/sapo/workflows \
    --state-dir /var/lib/sapo/sessions
Restart=on-failure
User=sapo
Environment=SAPO_MOMO_TOKEN=...        # secrets referenced as env:* in sapo-config.json

[Install]
WantedBy=multi-user.target
```

Consumer side (any Drogon service):

```cpp
auto client = drogon::HttpClient::newHttpClient("http://sapo.internal:8090");
auto req = drogon::HttpRequest::newHttpRequest();
req->setMethod(drogon::Post);
req->setPath("/workflows/examples.momo_transfer/runs");
req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
req->setBody(R"({"input": {"msisdn": "233201234567", "amount": 25}})");
client->sendRequest(req, [](drogon::ReqResult ok, const drogon::HttpResponsePtr& resp) {
    // resp->getBody() is the ExecutionOutcome JSON
});
```

When you outgrow one node, put a load balancer in front and switch the state store to the
shared Redis adapter (§4) so any instance can resume any session.

---

## 4. Session state in production

`IStateStore` (`src/runtime/StateStore.hpp`) is the seam; the engine never cares where
checkpoints live:

| Store | When |
|---|---|
| `InMemoryStateStore` (default) | tests, ephemeral runs only — sessions die with the process |
| `FileStateStore` (ships) | single-node services; atomic one-JSON-file-per-session, `jq`-inspectable |
| Redis adapter (T4.1, bring-your-own) | multi-node fleets, TTL-based expiry of abandoned sessions |
| PostgreSQL adapter (T4.1) | audit-grade retention, joins against business tables |

Choosing between Redis, Tarantool and PostgreSQL — including the throughput/latency
arithmetic for a USSD fleet, the concurrency control the `IStateStore` seam still needs, and
why the sketched adapter below is not production-complete — is covered in
[`STATE-STORE-REDIS-VS-TARANTOOL.md`](STATE-STORE-REDIS-VS-TARANTOOL.md). Read it before
writing an adapter.

A Redis adapter is ~40 lines because `SessionCheckpoint` serializes to a single JSON blob.
It is called on the engine's own (blocking) execution context, so use a **synchronous**
client (e.g. redis-plus-plus), not Drogon's async `RedisClient`:

```cpp
#include "runtime/StateStore.hpp"
#include <sw/redis++/redis++.h>

class RedisStateStore final : public sapo::runtime::IStateStore {
public:
    RedisStateStore(sw::redis::Redis& redis, std::string prefix, std::chrono::seconds ttl)
        : m_redis(redis), m_prefix(std::move(prefix)), m_ttl(ttl) {}

    void save(const sapo::runtime::SessionCheckpoint& cp) override {
        m_redis.set(m_prefix + cp.session_id, cp.toJson().dump(), m_ttl);
    }
    std::optional<sapo::runtime::SessionCheckpoint> load(const std::string& id) const override {
        auto value = m_redis.get(m_prefix + id);
        if (!value) return std::nullopt;
        return sapo::runtime::SessionCheckpoint::fromJson(nlohmann::json::parse(*value));
    }
    bool remove(const std::string& id) override { return m_redis.del(m_prefix + id) > 0; }
    std::vector<sapo::runtime::SessionCheckpoint> list() const override {
        std::vector<sapo::runtime::SessionCheckpoint> out;   // SCAN m_prefix* + load each
        for (auto key : m_redis.scan(m_prefix + "*", 0, "COUNT", 500).second) { /* ... */ }
        return out;
    }
    size_t count(std::optional<sapo::runtime::SessionStatus>) const override { return 0; }
    std::string kind() const override { return "redis:" + m_prefix; }

private:
    sw::redis::Redis& m_redis;
    std::string m_prefix;
    std::chrono::seconds m_ttl;
};
// services.state_store = std::make_shared<RedisStateStore>(redis, "sapo:session:", 15min);
```

---

## 5. Outbound HTTP from blueprints

Blueprint `http.*` nodes go through `sapo::http::IHttpTransport`
(`src/http/IHttpTransport.hpp`) — the engine never touches a network library directly.
Two production choices:

1. **Keep the built-in cpr transport** (`-DSAPO_ENABLE_CPR=ON`). Zero code; libcurl handles
   TLS/timeouts; the call blocks a Sapo worker thread, which is fine.
2. **Implement the seam over Drogon** — attractive with Mode B / `-DSAPO_ENABLE_CPR=OFF`,
   one network stack for the whole process, and your existing proxies/mTLS config apply.
   `send()` is invoked on a Sapo worker thread, so the *synchronous* client overload is the
   right tool (sketch — verify against your Drogon version):

```cpp
#include "http/IHttpTransport.hpp"
#include <drogon/HttpClient.h>

class DrogonTransport final : public sapo::http::IHttpTransport {
public:
    sapo::http::Response send(const sapo::http::Request& request) override {
        sapo::http::Response out;
        // parse request.url into origin + path (+query)  — e.g. with drogon::utils
        // auto client = drogon::HttpClient::newHttpClient(origin);   // cache per origin!
        // auto req = drogon::HttpRequest::newHttpRequest();
        // req->setMethod(methodFrom(request.method));
        // req->setPath(path); headers → req->addHeader(...);
        // if (request.body) req->setBody(request.body->is_string()
        //                     ? request.body->get<std::string>() : request.body->dump());
        // if (request.bearer_token) req->addHeader("Authorization", "Bearer " + *request.bearer_token);
        // auto resp = client->sendRequest(req, request.timeout_ms / 1000.0);  // blocks worker thread
        // if (!resp) { out.transport_error = "no response from " + request.url; return out; }
        // out.status_code = static_cast<int>(resp->getStatusCode());
        // out.body = std::string(resp->getBody());
        // for (auto& [k, v] : resp->getHeaders()) out.headers[k] = v;
        return out;
    }
    std::string name() const override { return "drogon"; }
};
// services.transport = std::make_shared<DrogonTransport>();
```

For tests and staging, `MockTransport` / `RecordReplayTransport` in the same header let you
record live exchanges once and replay them offline — blueprint tests never touch real APIs.

---

## 6. Production checklist

- [ ] Release build, `-DSAPO_ENABLE_CPR=ON` (or custom transport injected).
- [ ] `sapoc validate <blueprint-dir> --strict --config sapo-config.json` runs in CI before deploy.
- [ ] `vm.start()` audit problems are treated as deploy failures.
- [ ] Durable state store chosen (`FileStateStore` single-node; Redis/PG shared); state dir on
  durable storage with sane permissions.
- [ ] Secrets live in env/vault and enter via `env:*`/`secret:*` in `sapo-config.json`;
  `applySecretRedaction()` keeps them out of logs (done by `start()`).
- [ ] Engine calls happen off Drogon IO threads (`drogon::async_run` or a worker pool).
- [ ] Timers/cron/event triggers needed? `startBackgroundTick()` is running.
- [ ] Blueprints are registered at startup only; hot reload = validate-then-swap process.
- [ ] `/healthz` + `vm.metrics()` exported to your monitoring (`sapo.sessions.*`, latencies).
- [ ] Graceful shutdown: `vm.stop()` from `app().registerSyncAdvice` before exit.
- [ ] USSD/2s SLA: prompts answer synchronously; slow side effects moved to
  scheduled/event-triggered blueprints.

## 7. Status → HTTP mapping reference

| Engine `status` | Meaning | Suggested HTTP |
|---|---|---|
| `completed` / `terminated` | session finished (`output` set) | 200 |
| `awaiting_input` | parked on a prompt; `prompt` set | 202 |
| `suspended` | parked on timer/event | 202 |
| `failed` | workflow error; `error_code`/`error_node` set | 404 for `*_NOT_FOUND`, else 500 |
| `cancelled` | cancelled by caller | 200 |

`sapoc` exit-code equivalents: 0 success · 1 failed · 2 usage/blueprint error · 3 parked.