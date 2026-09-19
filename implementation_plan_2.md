# Sapo Engine — Gap Analysis & Implementation Plan (v2)

> **Goal:** Fully complete the Sapo Engine as a modular, DSL-driven execution engine, and extend it into a durable, plugin-hosting workflow platform.
>
> **Basis:** Full source review of `dev` @ `e5f5176` (all of `src/parser`, `src/runtime`, `src/tasks`, `CMakeLists.txt`, `tests/`, `plugin-dsl-bnf.txt`, `plugin-to-develop.txt`, and both architecture docs).
>
> **Companion docs:** `implementation_plan_1.md` (USSD standardization), `Re-architecting USSD with Sapo DSL Engine.md` (WssdApi integration), `plugin-dsl-bnf.txt` (grammar).

---

## 0. Executive Summary

The engine has a working **skeleton**: a JSON→AST parser, a linear+jump interpreter, a thread-safe context, real HTTP via cpr, and suspend/resume for action nodes. But several task types are **demo stubs that lie in the logs** (they print success without doing the configured work), two node types are **broken end-to-end** (`parallel` crashes the interpreter; `loop` cannot even be parsed), one code path is a **security hole** (`std::system`), and the entire **plugin/capability platform that `plugin-to-develop.txt` depends on does not exist**.

This plan closes those gaps in five phases over ~16 weeks (one experienced C++ developer; phases 3–4 parallelize well across two):

| Milestone | Exit criteria | When |
|---|---|---|
| **M0** | No crashes, no shell exec, truthful tests, green CI | Week 1 |
| **M1** | Grammar-complete language: every node type parses & executes, real expression language | Week 4 |
| **M2** | Real loops / waits / schedules / events / subflows / error model; durable suspend demo | Week 8 |
| **M3** | Capability/plugin SPI, built-in command pack, 5–10 real plugins | Week 12 |
| **M4** | Service mode, Redis/PG durability, async core, installable packaging | Week 16 |
| **M5** | Power-ups: Lua scripting, linter, benchmarks, security hardening | ongoing |

---

## 1. What Already Works (Verified Inventory)

Keep and build on these:

| Component | Files | State |
|---|---|---|
| JSON→AST parser | `src/parser/WorkflowParser.cpp`, `AstNodes.hpp` | Works for 14 of 15 node types (missing: `loop`) |
| Interpreter (linear + jump routing, terminate sentinels) | `src/runtime/Interpreter.cpp` | Works, but missing `parallel` case; routing via hidden `__SYS_*` context variables |
| RuntimeContext (thread-safe JSON store, serialize/deserialize) | `src/runtime/Context.hpp` | Solid; concurrency-safe by design |
| VirtualMachine (run/resume, input validation on resume) | `src/runtime/VirtualMachine.cpp` | Works; resume protocol is ad-hoc |
| HTTP command execution (cpr): GET/POST/PUT/DELETE/PATCH, basic auth, headers/query/body resolution, dotted output extraction | `src/tasks/CommandTask.cpp` | Real and the strongest part of the engine |
| Expression resolution (numeric exprtk + string interpolation + exact `$var` lookup) | `src/runtime/ExpressionEvaluator.cpp` | Partial; see gap E1 |
| Choice / Condition routing | `src/tasks/ChoiceTask.cpp`, `ConditionTask.cpp` | Work for simple numeric conditions via exprtk |
| Parallel fork-join (isolated contexts, state merge, fail-fast) | `src/tasks/ParallelTask.cpp` | Works **in isolation only** (never reachable through the interpreter) |
| Build (CMake + FetchContent: nlohmann/json, cpr; vendored exprtk + Catch2) | `CMakeLists.txt` | Builds; hygiene issues (see P0-5) |

---

## 2. Gap Analysis

Severity: 🔴 **P0** broken/unsafe · 🟠 **P1** core semantics missing · 🟡 **P2** platform missing.

### 🔴 P0 — Broken or unsafe as committed

**P0-1 `parallel` crashes the interpreter.**
`Interpreter::executeNode` has no `Parallel` case (cases present: Noop, Script, Terminate, Transform, Condition, Query, Command, Action, Event, Wait, Choice, Schedule, Loop, Subflow). Any blueprint containing a parallel node throws `Unhandled task execution type signature.` `ParallelTask` is only exercised directly by its unit test, which masks the break.

**P0-2 Shell injection via `std::system`.**
`CommandTask::execute` runs any non-`http.*` command string through the OS shell. A DSL `command` value of `"user.create"` is attempted as a shell command; any attacker-controllable blueprint is an RCE.

**P0-3 `loop` cannot be parsed at all.**
There is no `LoopNode` class in `AstNodes.hpp` and no `"loop"` branch in `WorkflowParser::parseSingleNode` → parse throws `Unknown task type: loop`. The orphaned `LoopTask` is a hardcoded counter (limit 5, jump target `"exit_step"`) that ignores the BNF's `collection` / `iterator` / `body` / `next` fields.

**P0-4 The test suite is untrustworthy.**
The `ParallelTask` test builds `TransformNode` children with `operation: "assign"`, `input: "10"`, `output: "var1"` and asserts `var1..var4` exist. But `TransformTask` is a hardcoded demo (reads fixed key `api_response`, writes fixed key `cleaned_payload`, uses a fixed mapping template) that ignores the node's fields entirely — so the test cannot pass against the current `TransformTask`. The suite must be made truthful before we can rely on it.

**P0-5 Build/repo hygiene.**
- `tests/TestRunner.cpp` is compiled into **both** `sapo_core` (library) and `sapo_tests` (binary) — duplicate registration symbols and bloat.
- `build/`, `.DS_Store`, and `.idea/` are committed.
- No CI, no format/lint config, no sanitizer runs.
- `main.cpp` hardcodes a blueprint and calls a live external IP on every run.

### 🟠 P1 — Core execution semantics missing (grammar promises, code doesn't deliver)

**P1-1 No real expression language.**
- exprtk is double-only: conditions like `status == 'SUCCESS'`, boolean logic over non-numeric values, and paths like `$user.isSubscribed` (dollar sigil + dot path) fail to compile.
- `ExpressionEvaluator::resolveValue` strips **all** `$` characters before compiling (corrupts any value containing `$` elsewhere), supports no dot paths, and silently leaves unresolved variables in place instead of erroring.
- Structured `{left, operator, right}` expressions are parsed into `ExpressionMap` but **never evaluated anywhere**. `ActionTask` stores the literal string `"[Structured message expression not supported yet]"` for structured prompt messages.
- Every evaluation **recompiles** its exprtk program from scratch — no caching.

**P1-2 `wait` does not wait.**
The interpreter ignores `duration` / `until` and falls through to `next` immediately. `WaitTask` (never invoked by the interpreter) sleeps a hardcoded 1 second; the `until` branch contains `// i will come back to this later...`. There is no durable wait (suspend + timer), so waits cannot survive a process restart.

**P1-3 `subflow` is a log-only stub.**
The interpreter's `Subflow` case prints a log line and steps on; `SubflowTask.cpp` is entirely commented-out code. No workflow registry exists — `workflow: "send_notifications"` references a name that nothing resolves. No input/output parameter binding, no recursion guard.

**P1-4 `schedule` ignores its own configuration.**
`ScheduleTask` hardcodes `"every 60s"` → target `"billing_sync_flow"`; the parsed `cron`, `timezone`, `body`, and `enabled` fields are unused. There is no scheduler daemon, no persistent job registry, no catch-up on restart.

**P1-5 Events are `std::cout`.**
`EventTask` (never invoked by the interpreter) sets a `last_event_triggered` variable; the interpreter's `Event` case just logs and returns `next`. No event bus, no subscriber registration, and `on_event` on action nodes is parsed but never triggers anything. **Nothing in the system can event-start a workflow** — this removes the reactive core of the "DSL-driven" story (webhooks, `checkout.completed` triggers, etc.).

**P1-6 `action` capabilities do not dispatch.**
`ActionTask` sleeps 150 ms and logs "Plugin Engine: capability executed successfully." `capability`, `inputs`, `outputs`, `data_sources`, and `next_tasks[].execute_condition` are all parsed and unused. There is no capability registry to dispatch to.

**P1-7 `query` returns a hardcoded simulated row set.**
No data-source registry, no connection configuration, no providers (the BNF's `provider: "postgresql"` / `"redis"` are decorative).

**P1-8 `script` is numeric-exprtk only.**
Only `language: "expr"` works, and only numeric expressions. The BNF's multi-line Lua example throws `Unsupported script sandboxing runtime target`. `ScriptTask.cpp` is an empty stub that is never called (the interpreter handles `expr` inline).

**P1-9 No error-handling model.**
No `try` / `catch` / `finally` nodes, no retry policy, no `on_error` branch, no `$error` variable. A non-2xx HTTP response hard-stops the whole workflow (`Failed` → `__TERMINATE_FAILED__`) with no opportunity to compensate or route to an error path.

**P1-10 Suspend/resume is a hidden-variable protocol.**
Suspend only works for action nodes, via `__SYS_RESUME_NODE`, `__SYS_NEXT_JUMP`, `__SYS_SUSPENDED_PROMPT`, `__SYS_INPUT_VALIDATION`, `__SYS_TIMEOUT_MS`. `TaskOutcome::Yielded` is defined and used nowhere. No execution IDs, no session management, no durable state store — the WssdApi doc's Redis session flow assumes an API the engine doesn't have.

**P1-11 Grammar/implementation drift.**
| BNF says | Code expects |
|---|---|
| `switch` node with `expression` + `cases` + `default` | `choice` node with `variable_key` + `cases` + `default_target` |
| `query` = `source` / `filter` / `limit` / `output` | `data_source_id` / `query_statement` / `query_parameters` / `output_context_key` |
| `parallel.tasks` = list of task references | `parallel.child_tasks` = nested node objects |
| `if` node (high-level grammar) | not implemented |
| `data_node`, `components` categories | not implemented |
| `wait.until`, `schedule.body`, `subflow.inputs/output` | parsed, never used |

Until this drift is reconciled, every sample in `plugin-dsl-bnf.txt` is aspirational rather than executable.

### 🟡 P2 — Platform missing

**P2-1 No plugin/capability architecture.**
No `ICapability`/`ICapabilityProvider` interface, no registry, no provider configuration, no secrets management. The 20 plugins in `plugin-to-develop.txt` and the 14 "Available System Commands" families in the BNF (`entity.*`, `data.*`, `crypto.*`, `auth.*`, `fs.*`, `s3.*`, `mq.*`, `db.*`, `notify.*`, `log.*`, `audit.*`, `metric.*`) have nowhere to plug in — **only `http.*` is real**.

**P2-2 No packaging or interop surface.**
No CMake `install()` targets, no package config; the WssdApi doc expects `libsapo_core.a` + an include directory that the build never produces. No CLI (blueprint is hardcoded in `main.cpp`). No service mode.

**P2-3 Concurrency model is unbounded and synchronous.**
Everything is `std::async(std::launch::async, ...)` on the global pool: no limits, no cancellation, no backpressure. `cpr` is used synchronously (blocks a pool thread per HTTP call). `TaskExecutionContext` carries a `RuntimeContext&` into async lambdas (dangling-reference risk if the caller's context outlives nothing). Parallel state merge is last-writer-wins with no conflict semantics.

**P2-4 Observability is `std::cout`.**
No structured logger, no per-node trace (start/end/duration/outcome), no correlation or execution IDs, no metrics beyond one total duration.

**P2-5 No validation or tooling.**
No JSON Schema for blueprints, no static checks (unknown jump targets, duplicate IDs, unreachable nodes, cycles, unbound variables), no blueprint test harness, no versioning.

---

## 3. Target State — What "Fully Complete" Means

| Layer | Definition of done |
|---|---|
| **Language** | All node types in the canonical grammar parse and execute per spec; typed expression language (dot/bracket paths, string/bool/array ops, function library); JSON Schema + static validation; versioned blueprints |
| **Execution semantics** | Loops over collections; durable waits; cron scheduler with timezones; event emission + event-triggered workflow starts; subflows with input/output contracts; parallel fork-join in the main interpreter; retry / on_error / try-catch error model |
| **Platform** | Capability/plugin SPI + registry (dot-notation dispatch); data-source SPI (context/http/redis/postgres/mock); provider config with `env.*` / `secret.*` resolution; all BNF command families implemented or explicitly deferred |
| **Durability** | Checkpoint/restore of {context, cursor, timers, event subscriptions, session id} to Redis or PostgreSQL; cross-process resume; kill -9 survival |
| **Deployment** | Installable static lib + headers + CMake package config; CLI (`run`, `resume`); service mode (session HTTP API + webhook receiver); the WssdApi integration doc is implementable as written |
| **Quality** | CI (build, tests, ASan, clang-tidy) green; golden tests per node type; HTTP record/replay tests; coverage ≥ 80% on `sapo_core`; zero shell execution |
| **Observability** | Structured logs, per-node trace spans, metrics (throughput, p99 latency, error rate, active sessions) |

---

## 4. Phased Implementation Plan

### Phase 0 — Quick Wins (Week 1) · *fix the bleeding*

| # | Task | Files | Acceptance |
|---|---|---|---|
| T0.1 | **Remove `std::system`.** Unknown non-http commands become parse-time errors (fail closed). | `src/tasks/CommandTask.cpp`, `src/parser/WorkflowParser.cpp` | No shell execution anywhere in the codebase (grep-verified) |
| T0.2 | **Add `Parallel` case** to the interpreter dispatch. | `src/runtime/Interpreter.cpp` | A blueprint with a parallel node executes instead of throwing |
| T0.3 | **Make `TransformTask` truthful.** Implement `assign` (and plan filter/map/project/merge for T2.7) using the node's real `operation`/`input`/`mapping`/`output` fields. | `src/tasks/TransformTask.cpp` | The existing parallel test passes for the right reason |
| T0.4 | **Build hygiene.** Remove `TestRunner.cpp` from `sapo_core` sources; add `.gitignore` (`build/`, `.DS_Store`, `.idea/`, `*.a`); strip committed junk; add GitHub Actions CI (configure + build + `ctest` on gcc & clang, ASan job). | `CMakeLists.txt`, repo root | CI green on push |
| T0.5 | **CLI skeleton.** `sapoc run <blueprint.json> --input <state.json>` (still inline VM for now). | `src/main.cpp` | Blueprints run from files; no hardcoded live-IP demo in `main` |

**Milestone M0:** `ctest` green in CI; no blueprint can crash the interpreter or touch a shell.

---

### Phase 1 — Language Core (Weeks 2–4) · *M1: grammar-complete language*

| # | Task | Files | Acceptance |
|---|---|---|---|
| T1.1 | **Canonical grammar v1.** Reconcile BNF ↔ implementation and lock the decision: `choice` with `expression` + `cases` + `default` (BNF spelling wins); keep nested `child_tasks` for `parallel`; add `if` as condition sugar; align `query` field names. Publish `schemas/workflow.schema.json` + workflow `version` field. This is a **product decision** — lock it before coding T1.2–T1.4. | `docs/DSL_GRAMMAR_v1.md`, `schemas/workflow.schema.json` | Every sample in the updated BNF section either parses or is explicitly marked future |
| T1.2 | **Sapo Expression Language (SEL).** New `src/runtime/expressions/` module: tokenizer → parser → typed evaluator over `nlohmann::json` values. Features: literals (string/number/boolean/null/array/object); variables with dot + bracket paths (`$result.items[0].id`); binary operators (arithmetic, comparison, logical, string concatenation); unary operators; function library (`len`, `upper`, `lower`, `contains`, `is_number`, `is_decimal`, `is_string`, `now`, `concat`, `coalesce`, `to_string`, `to_number`, …). Accept both inline string form and structured `{left, operator, right}`. **Compile once at parse time**; cache compiled expressions on AST nodes. | `src/runtime/expressions/*.hpp/.cpp` | Property-based unit tests: ≥ 200 expression cases; no exprtk recompile at evaluation time |
| T1.3 | **`ExpressionEvaluator` v2.** Full-path string resolution for urls/headers/query/body/prompts; **strict mode** that throws (with node id + variable name) on unresolved references; `env.*` / `secret.*` references resolved from provider config (T3.2 seam). Keep the existing public API shape so tasks don't churn. | `src/runtime/ExpressionEvaluator.cpp/.hpp` | `"https://x/$post_id"` resolves; `"$missing"` fails loudly in strict mode |
| T1.4 | **AST & parser completion.** Add `LoopNode` (`collection` expression, `iterator`, `body`, `next`, `max_iterations`); parser branches for every node type per grammar v1; **validation pass**: duplicate IDs, unknown jump targets, unreachable nodes, cycle detection (warn), missing required fields, workflow metadata (`name`, `version`, `description`). | `src/parser/AstNodes.hpp`, `WorkflowParser.cpp` | 100% parser unit-test coverage of node types + ≥ 30 malformed-input rejection tests |
| T1.5 | **Interpreter v2 — typed control flow.** Replace `__SYS_*` hidden variables with a return type, e.g. `struct ControlSignal { std::variant<Continue, JumpTo, Suspend, Terminate> }` (JumpTo carries node id; Suspend carries prompt/interaction/validation config; Terminate carries status + payload). One switch, one router, fully unit-testable per node. | `src/runtime/Interpreter.cpp/.hpp`, task files | No `__SYS_` strings remain in `Interpreter.cpp`; per-node routing tests pass |

**Milestone M1:** golden-test suite — for every node type: minimal blueprint in → expected context + expected next-node out. All green in CI.

---

### Phase 2 — Execution Semantics (Weeks 5–8) · *M2: waits, schedules, events, subflows, loops are real*

| # | Task | Files | Acceptance |
|---|---|---|---|
| T2.1 | **Real loops.** Iterate context collections (arrays); bind iterator variable per iteration; `break`/`continue` via expressions; `max_iterations` safety cap; route to `next` after body completes. | `src/tasks/LoopTask.cpp`, interpreter | Loop blueprint over `$subscribers` executes body N times with correct iterator binding; cap triggers clean exit |
| T2.2 | **Durable waits.** Parse durations (`30s`, `10m`, `2h`, ISO-8601) and `until` conditions. **Non-blocking**: wait = suspend + registered timer checkpoint, not `sleep_for`. | `src/tasks/WaitTask.cpp`, state store seam (T4.1) | `wait 10m` suspends immediately; resume happens on timer fire across a process restart (in-memory adapter first) |
| T2.3 | **Scheduler.** Cron parser (5/6 fields, timezone via HowardHinnant `date`), persistent job registry, one-shot (`"in 5 minutes"`) + recurring, catch-up on restart, fires workflow instances through the VM. | `src/runtime/Scheduler.{hpp,cpp}`, `src/tasks/ScheduleTask.cpp` | Cron job registered by a `schedule` node fires a fresh workflow run at the correct wall-clock time (test with accelerated clock) |
| T2.4 | **Event system.** `EventBus` interface: in-proc registry + adapter SPI. `event` node publishes resolved payload; `on_event` (action) and workflow-level `trigger` subscribe and **spawn workflow instances**; correlation IDs. Adapters: local webhook receiver (T4.3) + RabbitMQ (optional, T3.3 `mq.*`). | `src/runtime/EventBus.{hpp,cpp}`, `src/tasks/EventTask.cpp`, ActionTask | Emitting `checkout.completed` inside one workflow starts a subscribed workflow with the payload in its initial context |
| T2.5 | **Subflows.** `WorkflowRegistry` (directory / DB / embedded JSON sources); invocation with `inputs`/`output` parameter binding; `wait_for_completion` semantics (blocking join vs fire-and-forget); recursion-depth limit; isolated child context. | `src/tasks/SubflowTask.cpp`, `src/runtime/WorkflowRegistry.{hpp,cpp}` | Payment-orchestration BNF example runs: parent → subflow `send_notifications` with mapped inputs, aggregated output |
| T2.6 | **Error model.** `try`/`catch`/`finally` nodes (and/or per-node `on_error`), retry policy on `command`/`wait` (`retries`, `backoff`, `jitter`), `$error` variable (`code`, `message`, `response`), HTTP non-2xx → error path with captured response instead of hard stop. | interpreter, CommandTask, new `TryTask`/`CatchTask` | Failing HTTP node routes to `on_error`; retry with backoff succeeds on 3rd attempt (mock server) |
| T2.7 | **Transform, real.** `filter` / `map` / `project` / `merge` / `assign` over context variables using SEL predicates and mappings (`$input.x`), covering the BNF payment-plugin `project` examples exactly. | `src/tasks/TransformTask.cpp` | Both BNF payment plugin blueprints (Hubtel + Paystack response shaping) pass golden tests |
| T2.8 | **Query + data sources.** `IDataSourceProvider` SPI (name → provider) with built-ins: `context` (in-memory), `http`, `redis`, `postgresql`, `mock` (tests). Connection registry from provider config with env-referenced credentials. | `src/runtime/DataSourceProvider.*`, `src/tasks/QueryTask.cpp` | `query` against `context` source filters a real context array; `http` source round-trips against a mock server; provider misconfiguration fails at parse time |

**Milestone M2 (demo):** end-to-end USSD-style flow — suspend at menu prompt → state serialized to disk/Redis → **second process** resumes at the correct node; cron fires a workflow; webhook event starts a workflow; loop + transform + choice + retry blueprint passes.

---

### Phase 3 — Plugin & Capability Platform (Weeks 9–12) · *M3: `plugin-to-develop.txt` becomes buildable*

| # | Task | Files | Acceptance |
|---|---|---|---|
| T3.1 | **Capability SPI.** `ICapabilityProvider` interface: `id`, `version`, input/output JSON Schemas, `execute(context) -> json`. `CapabilityRegistry` dispatches dot-notation (`communication.email.send` → provider `communication.email`, action `send`). `ActionTask.capability` and non-http `CommandTask.command` dispatch through it. | `src/capabilities/ICapabilityProvider.hpp`, `CapabilityRegistry.{hpp,cpp}`, ActionTask, CommandTask | `capability: "communication.email.send"` with a registered mock provider executes and writes declared outputs |
| T3.2 | **Provider config & secrets.** Per-provider config blocks (in `sapo-config.json`), `env.*` / `secret.*` resolution in expressions, startup validation of required provider fields, secret redaction in all logs/traces. | `src/capabilities/ProviderConfig.*` | A plugin requiring a secret runs with env-sourced value; the secret never appears in logs |
| T3.3 | **Built-in command pack** (BNF "Available System Commands"). Each family implemented or **explicitly deferred** in the registry (unknown → clear parse-time error, never a no-op): `data.*` (transform/parse/validate), `crypto.*` (OpenSSL: hash/encrypt/decrypt/sign/verify/keypair), `log.*` + `audit.record` + `metric.*` (structured logger + counters), `notify.*` (email/SMTP, webhook, sms), `entity.*` (via data-source SPI), `auth.*` (JWT sign/verify, OAuth2 token exchange), `fs.*`, `s3.*` (SigV4 over cpr or AWS SDK), `mq.*` (RabbitMQ), `db.*`. | `src/capabilities/builtin/*` | Unit + integration test per implemented family; registry lists implemented vs deferred |
| T3.4 | **Plugin packaging.** Static registration macros + optional shared-library plugins (`.so` with entry point) + JSON manifest (id, version, provider config schema, capabilities). | `src/capabilities/plugin.*` | A sample external plugin `.so` loads, registers, and executes |
| T3.5 | **First real plugins** (ordered by value ÷ effort): ① SMTP/email · ② OpenAI + Anthropic + Gemini (pure HTTP — quick wins, unblock AI features) · ③ Session Manager · ④ 2FA (TOTP) · ⑤ AWS S3 · ⑥ Contact Form · ⑦ Device Detector · ⑧ Coupons · ⑨ Taxes · ⑩ NOS Importer. Each ships: provider code + input/output schemas + DSL blueprint example + tests (mock-backed) + one-page doc. | `plugins/*` | 5+ plugins with passing integration tests by M3 |

**Milestone M3:** every BNF command family implemented or explicitly deferred; the ecosystem plugins are buildable on real infrastructure instead of `sleep(150ms)`.

---

### Phase 4 — Durability, Scale & Deployment (Weeks 13–16) · *M4: deployable product*

| # | Task | Files | Acceptance |
|---|---|---|---|
| T4.1 | **State store SPI.** Checkpoint = `{context, cursor node id, pending timers, pending event subscriptions, execution id, session id, blueprint version}`. Adapters: in-memory, **Redis** (redis-plus-plus), **PostgreSQL**. `VirtualMachine` gains first-class `startSession` / `resumeSession` / `cancelSession` APIs, replacing the `__SYS_RESUME_NODE` protocol. | `src/runtime/StateStore.{hpp,cpp}`, adapters, VirtualMachine | Kill -9 mid-workflow → restart → session resumes at the exact node with intact context |
| T4.2 | **Async-first core.** Bounded worker pool; cpr **async** API; timer wheel for waits/schedules; non-blocking suspend; API surface designed so `drogon::Task` (per the WssdApi doc) can wrap execution without burning a thread per session. | `src/runtime/*`, CommandTask, Scheduler | 10k concurrent suspended sessions with < 200 live threads (benchmark) |
| T4.3 | **Service mode.** `sapo-server` binary (Drogon): `POST /workflows/{id}/runs` (blueprint + input), `POST /sessions/{id}/input` (resume with user input), `GET /sessions/{id}`, `DELETE /sessions/{id}`, webhook receiver (T2.4 adapter), `/healthz`, `/metrics`. | `src/server/*` | USSD flow driven entirely over HTTP: start → prompt → reply → prompt → terminal state |
| T4.4 | **Observability.** Structured logging (spdlog, JSON lines); per-node trace spans (execution id → node id → duration/outcome); metrics: throughput, p50/p99 node & workflow latency, error rate, active sessions. | `src/observability/*` | A run produces a complete JSON-lines trace; `/metrics` exposes the counters |
| T4.5 | **Packaging.** `install()` targets (headers, `libsapo_core.a`, CMake package config), semantic versioning, release automation (tag → release artifacts). Makes the WssdApi integration doc a non-fiction: consume `libsapo_core.a` + `find_package(sapo_engine)`. | `CMakeLists.txt`, CI | A downstream CMake project links Sapo via installed package on a clean machine |

**Milestone M4:** the USSD hybrid architecture from `implementation_plan_1.md` works as documented — API-layer execution under the 2-second window, background workers consuming queue items, session state in Redis shared between them.

---

### Phase 5 — Power-Ups (ongoing, scheduled as capacity allows)

1. **Lua scripting** (sol2) for the BNF's multi-line `script` language, sandboxed (time + memory caps). exprtk stays for the fast inline `expr` path.
2. **Blueprint linter & test harness:** static analysis (unbound variables, unreachable nodes, missing outputs, cycles) + per-blueprint golden tests + HTTP record/replay via a local mock server so workflow tests never hit real APIs.
3. **Performance:** expression precompile cache hardening, AST interning, benchmark suite (workflows/sec, p99), parser fuzzing (AFL/libFuzzer in CI).
4. **Security hardening pass:** timeouts on every external call, dependency audit, redaction verified in traces, threat model for the service mode (authn on session APIs).
5. **Docs:** user guide (DSL reference v1 + plugin authoring guide), architecture decision records (ADR log), sample blueprint gallery (`examples/`).

---

## 5. Risks & Sequencing Notes

| Risk | Mitigation |
|---|---|
| **SEL rewrite (T1.2) touches everything** — every task evaluates expressions | Keep `ExpressionEvaluator`'s public API stable during the rewrite; feature-flag strict mode; gate on the golden-test suite from M1 before proceeding |
| **Grammar v1 is a product decision, not an engineering one** (`choice` vs `switch` spellings, loop semantics, error model shape) | Lock T1.1 in review before any Phase 1 code; treat `plugin-dsl-bnf.txt` as the negotiation document |
| **Building the 20 plugins before the capability SPI** would create integration debt in each one | Phase ordering is mandatory: T3.1–T3.2 before any T3.5 work |
| **Durability (T4.1) interacts with every node type's suspend behavior** | In-memory state-store adapter ships with M2 (T2.2) so semantics are stable before Redis/PG adapters |
| **Single-maintainer bus factor** | Every phase leaves CI green + docs current; ADR log from Phase 1 onward |

**Critical path:** T1.1 → T1.2 → T1.5 → T2.2/T2.4/T2.5 → T4.1. Everything else can slip without breaking milestones.

---

## 6. Effort Summary

| Phase | Duration | Staffing |
|---|---|---|
| 0 — Quick wins | 1 week | 1 |
| 1 — Language core | 3 weeks | 1 (T1.2 can parallelize with T1.4/T1.5) |
| 2 — Execution semantics | 4 weeks | 1–2 (T2.3 scheduler ∥ T2.4 events ∥ T2.7/T2.8) |
| 3 — Plugin platform | 4 weeks | 1–2 (built-ins ∥ first plugins after T3.1–T3.2) |
| 4 — Durability & deployment | 4 weeks | 1–2 (T4.2 ∥ T4.3/T4.4) |
| 5 — Power-ups | ongoing | 1 |

**Total to M4: ~16 weeks solo, ~10–12 weeks with two developers.**
