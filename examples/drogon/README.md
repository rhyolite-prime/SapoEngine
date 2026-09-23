# Sapo Engine — Drogon Reference Host Service

This directory provides a production reference implementation embedding `sapo_core` inside a [Drogon](https://github.com/drogonframework/drogon) C++ web service.

## Architecture Highlights

1. **In-Process High Performance:** Embeds `libsapo_core.a` directly inside Drogon, eliminating network RPC latency between the API gateway and the DSL execution engine.
2. **Non-Blocking Threading:** Offloads Sapo's AST execution (`startSession` / `resumeSession`) off Drogon's I/O event loops onto worker threads using `drogon::async_run` within C++ coroutines (`drogon::Task`).
3. **Native Drogon HTTP Transport:** Implements Sapo's `IHttpTransport` seam using `drogon::HttpClient`. Blueprints running `http.*` tasks reuse Drogon's connection pool without requiring `cpr` or direct `libcurl` linking.
4. **State Persistence:** Supports single-node `FileStateStore` or distributed `RedisStateStore` with atomic Compare-And-Swap (CAS) and claim locking.
5. **Standard USSD Gateway Adapter:** Implements single-endpoint initiation (`*920#`) vs continuation (`1`, `2`) mapping for telco aggregators (Hubtel, Nalo, Africa's Talking).

## Prerequisites

- Drogon Framework >= 1.9 (`sudo apt install libdrogon-dev` or compiled from source)
- C++23 compliant compiler (GCC >= 13 or Clang >= 17)
- CMake >= 3.28

## Building the Service

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j"$(nproc)"
```

## Running the Service

```bash
# By default, loads workflows from ./workflows and stores sessions in /tmp/sapo_sessions
./sapo_drogon_service

# To use Redis for distributed sessions across a fleet:
export SAPO_REDIS_HOST="127.0.0.1"
./sapo_drogon_service
```

## Testing Endpoints with cURL

### 1. USSD Initiation (User dials *920*1#)
```bash
curl -X POST http://localhost:8080/ussd \
  -H "Content-Type: application/json" \
  -d '{
    "SessionId": "sess-ussd-1001",
    "Mobile": "233201234567",
    "ServiceCode": "ussd_menu",
    "Type": "Initiation",
    "Message": "*920*1#"
  }'
```
Response:
```json
{
  "SessionId": "sess-ussd-1001",
  "Type": "Response",
  "Message": "Welcome to Sapo Bank\n1. Check Balance\n2. Mini Statement\n3. Exit"
}
```

### 2. USSD Continuation (User replies "1")
```bash
curl -X POST http://localhost:8080/ussd \
  -H "Content-Type: application/json" \
  -d '{
    "SessionId": "sess-ussd-1001",
    "Mobile": "233201234567",
    "Type": "Response",
    "Message": "1"
  }'
```
Response:
```json
{
  "SessionId": "sess-ussd-1001",
  "Type": "Release",
  "Message": "Your current balance is GHS 1,450.25. Thank you."
}
```

### 3. REST API: Start Workflow
```bash
curl -X POST http://localhost:8080/api/v1/workflows/ussd_menu/runs \
  -H "Content-Type: application/json" \
  -d '{
    "input": { "msisdn": "233201234567" },
    "session_id": "rest-sess-2001"
  }'
```

### 4. REST API: Resume Session
```bash
curl -X POST http://localhost:8080/api/v1/sessions/rest-sess-2001/input \
  -H "Content-Type: application/json" \
  -d '{ "input": "2" }'
```

### 5. Health & Metrics
```bash
curl http://localhost:8080/healthz
curl http://localhost:8080/metrics
```
