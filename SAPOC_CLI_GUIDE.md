# `sapoc` Command-Line Tool Guide

`sapoc` is the command-line driver and runtime supervisor for **SapoEngine**. Everything the CLI can do is an interface over `sapo::runtime::VirtualMachine`, ensuring that blueprint parsing, validation, execution, and state persistence behave identically in the CLI and in host applications.

---

## Table of Contents

1. [Quick Start & Build](#1-quick-start--build)
2. [CLI Syntax & Exit Codes](#2-cli-syntax--exit-codes)
3. [Core Commands](#3-core-commands)
   - [`run`](#sapoc-run)
   - [`resume`](#sapoc-resume)
   - [`sessions` & `session`](#sapoc-sessions--sapoc-session)
   - [`cancel`](#sapoc-cancel)
   - [`validate`](#sapoc-validate)
   - [`describe`](#sapoc-describe)
   - [`emit`](#sapoc-emit)
   - [`metrics`](#sapoc-metrics)
4. [Command Options & Flags](#4-command-options--flags)
5. [Passing Variables & Inputs](#5-passing-variables--inputs)
6. [Durable Sessions & Park/Resume Patterns](#6-durable-sessions--parkresume-patterns)
7. [Debugging & Observability](#7-debugging--observability)
8. [Common Pitfalls & Pro Tips](#8-common-pitfalls--pro-tips)

---

## 1. Quick Start & Build

The engine is written in **C++23**. Vendored libraries (`nlohmann/json` and `Catch2`) are located in `src/third_party/`, allowing compilation without downloading dependencies.

```bash
# Configure release build
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build the sapoc executable
cmake --build build --target sapoc

# Verify the build
./build/sapoc version
```

> **HTTP Transport Note:** SapoEngine builds with `NullTransport` by default to support air-gapped/offline builds. To enable real outbound HTTP requests, install `libcurl` and `cpr`, then compile with `-DSAPO_ENABLE_CPR=ON`:
> ```bash
> cmake -B build -DSAPO_ENABLE_CPR=ON
> cmake --build build --target sapoc
> ```

---

## 2. CLI Syntax & Exit Codes

```text
Usage:
  sapoc <command> [positional arguments...] [options]
```

### Exit Codes

| Code | Status | Meaning |
|:---:|:---|:---|
| `0` | **Success** | Blueprint reached a successful terminal state (`success` / `terminated`), or utility command completed. |
| `1` | **Execution Failed** | Workflow encountered an unhandled error (`failed`), session not found, or cancel target invalid. |
| `2` | **Usage / Parse Error** | Invalid command-line arguments, malformed JSON, or blueprint validation failure. |
| `3` | **Parked / Suspended** | Workflow entered an asynchronous wait state (`awaiting_input` or `suspended` by a timer). |

---

## 3. Core Commands

### `sapoc run`
Starts a new workflow session from a blueprint file.

```bash
sapoc run <blueprint.json> [options]
```

**Examples:**

```bash
# Basic run with variables
./build/sapoc run examples/hello_world.json --var msisdn=233201234567

# Run with persistent state store and custom session ID
./build/sapoc run examples/kyc_ussd_flow.json \
  --var msisdn=233201234567 \
  --session kyc-user-1001 \
  --state-dir .sapo-state

# Run with complex JSON input from a file
./build/sapoc run examples/agent_commission_batch.json --input batch.json

# Step into blueprint starting at a specific node
./build/sapoc run examples/hello_world.json --start-node finish
```

---

### `sapoc resume`
Resumes a parked or suspended workflow session (e.g., answering a user prompt or continuing after approval).

```bash
sapoc resume <session-id> [blueprint.json...] [options]
```

> **Note:** The blueprint file must be supplied (or registered in config) so `sapoc` can rebuild the AST to interpret the checkpoint.

**Examples:**

```bash
# Resume with a scalar answer string
./build/sapoc resume kyc-user-1001 examples/kyc_ussd_flow.json \
  --answer '"1234"' \
  --state-dir .sapo-state

# Resume with a structured JSON object
./build/sapoc resume order-session examples/order_flow.json \
  --answer '{"approved": true, "reason": "Authorized by manager"}' \
  --state-dir .sapo-state
```

---

### `sapoc sessions` & `sapoc session`
Inspects stored workflow sessions in a state directory.

```bash
# List all sessions in the directory
./build/sapoc sessions --state-dir .sapo-state

# Show detailed checkpoint and context for a specific session
./build/sapoc session kyc-user-1001 --state-dir .sapo-state

# Output session details in machine-readable JSON
./build/sapoc session kyc-user-1001 --state-dir .sapo-state --json
```

---

### `sapoc cancel`
Cancels a currently parked or suspended workflow session.

```bash
sapoc cancel <session-id> [options]
```

**Example:**
```bash
./build/sapoc cancel kyc-user-1001 \
  --reason "User cancelled via USSD" \
  --state-dir .sapo-state
```

---

### `sapoc validate`
Statically parses and validates blueprints without executing them. Verifies task schemas, branching targets, variable syntax, and reachability.

```bash
sapoc validate <path...> [options]
```

**Examples:**

```bash
# Validate an entire directory
./build/sapoc validate examples/

# Validate a specific file with strict mode (warnings treated as errors)
./build/sapoc validate examples/hello_world.json --strict

# Output validation results as JSON (ideal for CI/CD pipelines)
./build/sapoc validate examples/ --json
```

---

### `sapoc describe`
Inspects the capabilities, loaded blueprints, providers, data sources, and limits of the engine environment.

```bash
# Inspect defaults
./build/sapoc describe

# Inspect with a specific blueprint
./build/sapoc describe examples/payment_confirmed_trigger.json

# Inspect with full configuration
./build/sapoc describe --config sapo-config.example.json --json
```

---

### `sapoc emit`
Publishes an event into the running engine's event bus, triggering any blueprints with matching `trigger.event` expressions.

```bash
sapoc emit <event.name> [options]
```

**Example:**
```bash
./build/sapoc emit payments.confirmed \
  --config sapo-config.example.json \
  --var reference=REF-100 \
  --var amount=50 \
  --var channel=sms
```

---

### `sapoc metrics`
Dumps engine execution metrics, worker pool states, and queue statistics:

```bash
./build/sapoc metrics
```

---

## 4. Command Options & Flags

| Flag | Value | Description |
|:---|:---|:---|
| `--var <name>=<json>` | `name=value` | Seed session input. Repeatable. Auto-detects JSON vs strings. |
| `--input <file>` | `path` or `-` | Load session context from JSON file or standard input (`-`). |
| `--answer <json>` | JSON / String | Shorthand input to supply when using `sapoc resume`. |
| `--session <id>` | String | Set an explicit, deterministic session ID for `run`. |
| `--state-dir <dir>` | Directory | Directory for durable session checkpoints. Defaults to in-memory. |
| `--config <file>` | File path | Path to `sapo-config.json` (providers, limits, secrets). |
| `--start-node <id>` | Node ID | Jump into the workflow execution at a specific node. |
| `--max-visits <n>` | Integer | Node-activation budget limit (guards against infinite loops). |
| `--wait` | Flag | Pump scheduler ticks until waiting/timer sessions finish. |
| `--max-time <duration>`| Duration | Maximum wait ceiling for `--wait` (e.g. `10s`, `2m`). Default: `10s`. |
| `--tick-ms <n>` | Milliseconds | Scheduler tick interval when `--wait` is enabled. Default: `25ms`. |
| `--trace` | Flag | Print per-node execution trace and set log level to `debug`. |
| `--json` | Flag | Format terminal stdout as machine-readable JSON. |
| `--log-level <lvl>` | Level | Set logging level: `trace`, `debug`, `info`, `warn`, `error`, `off`. |
| `--log-file <file>` | File path | Append JSON log lines to a file instead of `stderr`. |
| `--strict` | Flag | Fail statically if unused fields or minor warnings exist. |
| `--no-persist` | Flag | Disable writing session checkpoints to disk for single-shot runs. |

---

## 5. Passing Variables & Inputs

### 1. Using `--var`
The `--var` option is smart:
* Valid JSON values (numbers, booleans, arrays, objects) are parsed automatically:
  ```bash
  --var amount=100                 # parsed as integer 100
  --var enabled=true               # parsed as boolean true
  --var tags='["vip", "urgent"]'   # parsed as JSON array
  ```
* Phone numbers (MSISDNs), account numbers, and token IDs with leading zeros or > 15 digits are **strictly preserved as strings** so leading zeros are not lost:
  ```bash
  --var msisdn=0244123456          # preserved as string "0244123456"
  ```

### 2. Passing Complex Objects via `--input`
For large inputs, create a JSON file:

```json
{
  "transactions": [
    { "id": "TX-01", "commission": 150 },
    { "id": "TX-02", "commission": 200 }
  ]
}
```

Then execute:
```bash
./build/sapoc run examples/agent_commission_batch.json --input input.json
```

Or pipe directly via stdin (`-`):
```bash
curl -s https://api.internal/batch | ./build/sapoc run examples/agent_commission_batch.json --input -
```

---

## 6. Durable Sessions & Park/Resume Patterns

When building conversational workflows (e.g., USSD, Chatbots, Multi-step approvals), tasks with `await_input: true` or `wait` timers halt execution.

### Pattern: Interactive USSD Flow

1. **Start the session with `--state-dir` and a custom `--session` name:**
   ```bash
   ./build/sapoc run examples/kyc_ussd_flow.json \
     --var msisdn=233201234567 \
     --session user-session-1 \
     --state-dir .sapo-state
   ```
   **Output:**
   ```text
   status:   awaiting_input
   workflow: kyc_ussd_flow
   session:  user-session-1
   cursor:   ask_pin
   prompt:   {"interaction_type":"pin","message":"Enter your 4-digit PIN..."}
   ```
   *(Exit code is `3`, indicating the session is parked.)*

2. **Resume the session with the user's answer:**
   ```bash
   ./build/sapoc resume user-session-1 examples/kyc_ussd_flow.json \
     --answer '"1234"' \
     --state-dir .sapo-state
   ```
   **Output:**
   ```text
   status:   terminated
   nodes:    9 visits in 1ms
   output:   {"degraded":true,"fallback_verified":true,"message":"KYC complete. Your daily limit is GHS 2000."}
   ```

---

## 7. Debugging & Observability

### Trace Execution Spans (`--trace`)
To see exactly which nodes were visited, their execution order, and the outcome of each node:

```bash
./build/sapoc run examples/hello_world.json --var msisdn=233201234567 --trace
```

Output includes a structured trace array:
```json
"trace": [
  {
    "depth": 0,
    "duration_ms": 0,
    "node_id": "greet",
    "node_type": "noop",
    "outcome": "success"
  },
  {
    "depth": 0,
    "duration_ms": 0,
    "node_id": "finish",
    "node_type": "terminate",
    "outcome": "terminated"
  }
]
```

### Structured Logging (`--log-level` & `--log-file`)
Output logs as JSON lines for ingestion into logging systems (ELK, Datadog, Loki):

```bash
./build/sapoc run examples/momo_transfer_with_guard.json \
  --var payer=0244111222 \
  --var payee=0244333444 \
  --var amount=100 \
  --var reference=TX123 \
  --log-level debug \
  --log-file execution.log
```

---

## 8. Common Pitfalls & Pro Tips

### 1. Zsh / Bash Angle Bracket Error (`zsh: no such file or directory`)
* **Cause:** Running documentation placeholders literally, e.g. `<session-id>`. The shell treats `<` as input redirection.
* **Fix:** Replace the placeholder with the actual session ID, or use `--session custom-id` when starting the session:
  ```bash
  # ❌ Incorrect (triggers shell redirection)
  ./build/sapoc resume <session-id> ...

  # ✅ Correct
  ./build/sapoc resume 278b114a-16d6-4a29-aeb3-b7eb23fd94e8 ...
  ```

### 2. Quoting String Literals in `--answer`
* **Cause:** Passing `--answer 1234` parses as integer `1234`, failing regex or length checks expecting a string (`"1234"`).
* **Fix:** Quote the string inside your shell quotes:
  ```bash
  --answer '"1234"'
  ```

### 3. Missing Checkpoints Across Invocations
* **Cause:** By default, `sapoc` runs with an `InMemoryStateStore`. When the process exits, in-memory state is discarded.
* **Fix:** Always provide `--state-dir <directory>` when running workflows that use `wait` or `await_input`.

### 4. Simulating Mock Fallbacks vs Live HTTP
* By default, HTTP calls fail with `[NOT_IMPLEMENTED] no HTTP transport is compiled in`.
* In workflows with `try/catch` or `on_error` handlers (such as `momo_transfer_with_guard.json` and `kyc_ussd_flow.json`), this allows testing fallback and recovery degradation branches deterministically without requiring a live network endpoint.
