# `sapo_service` — Sapo Engine behind Drogon

Reference implementation of the engine's **service mode** (the `sapo-server`
described in `implementation_plan_2.md` T4.3), and a copy-paste starting point
for embedding Sapo in any Drogon API. It wraps one
`sapo::runtime::VirtualMachine` in a Drogon process and exposes the session
API over HTTP.

## Endpoints

| Method | Path | Action |
|---|---|---|
| POST | `/workflows/{id}/runs` | start a session — body `{"input": {...}, "session_id": "optional-fixed-id", "correlation_id": ""}` |
| POST | `/sessions/{id}/input` | resume a parked session — body `{"input": {...}}` (or the raw answer) |
| GET | `/sessions` | list sessions |
| GET | `/sessions/{id}` | one session snapshot (status, pending prompt, context) |
| DELETE | `/sessions/{id}` | cancel — body `{"reason": "..."}` optional |
| POST | `/events` | publish an event — body `{"name": "...", "payload": {...}}` |
| GET | `/workflows` | registered workflow ids |
| GET | `/healthz` | liveness + engine summary |
| GET | `/metrics` | engine metrics (JSON) |

Responses carry the engine's `ExecutionOutcome` JSON verbatim
(`status`, `session_id`, `prompt`, `output`, `context`, `error*`, …):

- `200` — session finished (`completed`/`terminated`) or cancelled
- `202` — session parked (`awaiting_input`/`suspended`); `prompt` holds what to show the user
- `404` — unknown workflow / session (`error_code` says which)
- `500` — workflow execution failed (`error_code`, `error_node` say where)

## Build

Requirements: CMake ≥ 3.28, GCC ≥ 13 or Clang ≥ 17, **Drogon ≥ 1.9**
(coroutine handlers + `drogon::async_run`), libcurl dev headers.

```bash
# from the SapoEngine repo root
cmake -S examples/drogon -B build-service -DCMAKE_BUILD_TYPE=Release
cmake --build build-service -j"$(nproc)"
```

If Drogon is not installed system-wide, point CMake at it:
`-DCMAKE_PREFIX_PATH=/path/to/drogon-install`.

## Run

```bash
./build-service/sapo_service \
    --port 8090 \
    --workflows examples \
    --state-dir .sapo-state \
    --config sapo-config.example.json    # optional
```

## Smoke test

```bash
curl -s localhost:8090/healthz | jq

# start the KYC USSD example (workflow id = metadata.name), pinning our own session id
curl -s -X POST localhost:8090/workflows/examples.kyc_ussd_flow/runs \
     -H 'content-type: application/json' \
     -d '{"session_id": "demo-1", "input": {"msisdn": "233201234567"}}' | jq

# answer the prompt (202 → next prompt, until 200)
curl -s -X POST localhost:8090/sessions/demo-1/input \
     -H 'content-type: application/json' \
     -d '{"input": "1"}' | jq
```

## Notes

- Engine calls are synchronous; the handlers hop off Drogon's IO threads with
  `co_await drogon::async_run(...)` — keep it that way when adding routes.
- The default state store is in-memory; pass `--state-dir` for durable
  sessions that survive restarts. For multi-node deployments implement
  `sapo::runtime::IStateStore` over Redis (sketch in `docs/INTEGRATING.md` §4).
- See `docs/INTEGRATING.md` for the full deployment guide (library vs service
  mode, CMake wiring, concurrency, outbound HTTP, production checklist).
