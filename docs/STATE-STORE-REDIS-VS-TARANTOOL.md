# State store: Redis vs Tarantool for Sapo Engine

**Scope.** Which external store should back `sapo::runtime::IStateStore`
([`src/runtime/StateStore.hpp`](../src/runtime/StateStore.hpp)) in the multi-node USSD
deployment described in [`implementation_plan_1.md`](../implementation_plan_1.md) and
[`Re-architecting USSD with Sapo DSL Engine.md`](../Re-architecting%20USSD%20with%20Sapo%20DSL%20Engine.md).

**Short answer.** Redis (or Valkey) is the right default, and "Redis is single-threaded" is
not a real objection at USSD scale — you will sit at low single-digit percent of one core's
command capacity at national peak. The single-threaded command loop is an *asset* here,
because it hands you per-key atomicity for free, and you have a
read-modify-write race in `Interpreter::resumeSession` that needs exactly that.

Tarantool is the better choice only if you want *queryable* session state, real ACID
multi-key transactions, or to collapse Redis + RabbitMQ into one system. It is not a
throughput play — for this access pattern it buys almost no throughput.

---

## 1. Restating the question

The worry "highly concurrent engine + single-threaded Redis" mixes two different kinds of
concurrency:

| | What it means for Sapo | Does it load the store? |
|---|---|---|
| **Many suspended sessions** | 10 000 – 1 000 000 live sessions | **No.** A suspended session is a blob sitting in memory. Zero ops until the user replies. |
| **Many simultaneous interactions** | Requests arriving per second | **Yes.** This is the only thing that generates store traffic. |

The checkpoint design already made the first kind cheap — that is literally the header
comment on `StateStore.hpp`:

> A suspended session is *data*, not a parked thread … That is what makes 10 000 suspended
> sessions cost memory instead of 10 000 threads.

So the store's op rate is driven by **interactions/second**, not by session count. That
decoupling is what makes Redis comfortable.

---

## 2. What Sapo actually asks of the store

From the code, the entire surface is six methods:

```cpp
save(checkpoint)          // whole blob, one key
load(session_id)          // by key
remove(session_id)
list()                    // enumerate  ← the awkward one
count(status)             // aggregate  ← the awkward one
kind()
```

Observed access pattern per USSD round trip (`Interpreter::resumeSession` → `resume` →
`drive` → `finishRun`):

1. `load(session_id)` — one read, by primary key
2. execute the AST in memory (microseconds to milliseconds, unless a `command` node does HTTP)
3. `save(checkpoint)` — one write of the whole blob, with a TTL for abandoned sessions

**Characteristics that matter:**

- **Single key per operation.** No joins, no cross-session transactions, no range scans on
  the hot path. Session id is the natural shard key.
- **Value is one JSON blob.** Measured from [`.sapo-state/deposit-test.json`](../.sapo-state/deposit-test.json):
  **1 976 bytes pretty-printed → 1 539 bytes with `dump()`** (compact separators).
  `SessionCheckpoint` carries `context`, `frames`, `pending`, `timers`, `result`.
- **Called synchronously on engine worker threads.** `docs/INTEGRATING.md` §4 is explicit:
  use a *synchronous* client. Latency lands directly inside the 2-second USSD gateway window.
- **Cross-process resume is mandatory.** The hybrid architecture has a synchronous API layer
  *and* a RabbitMQ worker fleet sharing sessions. In-process stores are out.
- **`list()` / `count()` are admin/observability**, not hot path — but they are on the same
  interface, and that is where Redis is weakest.

---

## 3. The numbers

Assumptions (substitute your real telco figures — the conclusion is insensitive to 10×):

- 5 M subscribers, 20 % touch USSD daily → 1 M sessions/day
- 6 round trips per session → 6 M interactions/day
- Mean = 6 M / 86 400 s ≈ **70 interactions/s**
- Telco traffic is extremely peaky (lunch, evening, pay-day). Take 20× → **≈1 400 interactions/s**
- × 2 ops (load + save) → **≈2 800 store ops/s at absolute peak**

Against that:

| Metric | Your peak | Single Redis/Valkey node | Headroom |
|---|---|---|---|
| Ops/s (1.5 KB values, no io-threads) | ~2 800 | ~80 000 – 150 000 | **~30 – 50×** |
| Ops/s (Redis 8, `io-threads 8`) | ~2 800 | up to ~2× the above | **~60 – 100×** |
| Network bandwidth | ~4 MB/s | multi-GB/s | huge |
| RAM for 1 M live sessions | ~1.8 GB incl. per-key overhead | instance-sized | fits one node |

Redis 8's reworked async I/O threading is opt-in (`io-threads`, default `1`) and Redis
reports up to ~112 % throughput improvement at `io-threads 8` on multi-core Intel hardware
([redis.io/blog/redis-8-ga](https://redis.io/blog/redis-8-ga/)). You will not need it. Note
what it does and does not change: **io-threads parallelize socket read/write and RESP
parsing; command execution and data access stay on one thread.** That is deliberate, and it
is the part you want.

**Memory per session, worked out:** 1.5 KB payload + jemalloc rounding (~1.6 KB) + dictEntry
(24 B) + key SDS for `sapo:session:<uuid>` (~60 B) + robj (16 B) + expire entry (~40 B)
≈ **1.8 KB**. One million concurrently suspended USSD sessions ≈ **1.8 GB**. Use
`dump()`, not `dump(2)` — measured on the real checkpoint above, pretty-printing inflates the
blob by ~28 % (1 539 → 1 976 bytes). That is right for `jq`-inspectable files on disk, wrong
for a wire store.

**Conclusion: Redis's single-threaded command loop is quantitatively irrelevant to this
workload.** You would need ~30× your estimated national peak before one core saturates, and
sharding by session id is trivial if you ever get there.

### Latency inside the 2-second window

| Stage | Cost |
|---|---|
| `load` (LAN RTT + GET) | ~0.3 – 0.8 ms |
| AST execution, no I/O nodes | ~0.05 – 2 ms |
| `save` (SET EX) | ~0.3 – 0.8 ms |
| **A `command` node calling the MoMo/KYC gateway** | **100 – 2 000 ms** |

Store round trips are **~0.1 %** of the budget; the outbound HTTP call is **~100 %** of it.
Your hybrid architecture already gets this right by pushing heavy `command`/`schedule` work
to the RabbitMQ consumers. Do not spend engineering effort optimizing the store's
throughput — spend it on §6 and §7.

---

## 4. Where Redis single-threadedness *does* bite

Be honest about these; they are the real risks, and all are design choices, not limits.

**4.1 O(N) commands stall every other client.** The adapter sketched in
`docs/INTEGRATING.md` §4 implements `list()` as `SCAN sapo:session:*` + a GET per key, and
stubs `count()` to `return 0`. On 1 M keys that is 1 M GETs. `SCAN` is at least incremental
(never use `KEYS` — it blocks the loop for seconds), but `count(status)` cannot be answered
without reading every blob. **Fix:** maintain a secondary index in the same atomic write
(§7.4), or route `list()`/`count()` to a replica.

**4.2 Large values are pure single-threaded memcpy.** If a blueprint accumulates bulk data
in `context` — e.g. the 10 000-row batch in
[`examples/agent_commission_batch.json`](../examples/agent_commission_batch.json) — the blob
can reach megabytes. At 3 000 ops/s a 5 MB value is 15 GB/s of memory traffic on one thread.
That *would* melt the loop. **Fix:** bound the checkpoint; never store collections in it;
emit a `sapo.checkpoint.bytes` histogram and alarm above ~64 KB.

**4.3 Fork-based persistence spikes p99.** RDB `BGSAVE` and AOF rewrite fork; with a large
dataset, copy-on-write page faults stall the main thread for tens to hundreds of ms.
Survivable in a 2 s window, but visible in p99. **Fix:** do persistence on a replica, or
`appendonly yes` + `appendfsync everysec` + `no-appendfsync-on-rewrite yes` on a modestly
sized primary.

**4.4 Slow Lua / `MULTI`.** A script that loops over many keys blocks the loop for its whole
duration. Keep every script O(1) per session.

**4.5 One shard = one core.** Scaling out means Redis Cluster (or Valkey sharded), and your
key design is ideal for it: one key per session, no cross-slot operations needed on the hot
path. Use hash tags only if you co-locate a session's index entries with its blob.

**4.6 The client can become the real serialization point.** If every worker thread shares
one connection behind a mutex, you have built a single-threaded bottleneck *in your own
process* and Redis's threading model is beside the point. **Fix:** connection pool sized to
worker count (§7.6).

---

## 5. Why single-threadedness is an *asset* here

`Interpreter::resumeSession`
([`src/runtime/Interpreter.cpp:1213`](../src/runtime/Interpreter.cpp)) is:

```
load(session_id)  →  check status == AwaitingInput|Waiting  →  resume(...)  →  drive(...)  →  save(checkpoint)
```

There is **no lock, no version, no compare-and-swap**. `Interpreter::m_mutex` guards only
`m_event_subscriptions`; it does not serialize session execution. So two concurrent requests
for the same session both pass the status check and both execute.

This is reachable in production without any exotic timing:

- Telco gateways **retry on timeout** — the first request is still running when the retry lands.
- A user hangs up and redials inside the session window.
- The API layer and a RabbitMQ consumer both act on the same session (the hybrid design
  explicitly shares state between them).
- An `event`-driven resume fires while a prompt reply is being processed.

Consequence in a payment flow: the `command` node that POSTs to the MoMo gateway runs
**twice**. Last-writer-wins also silently rolls the cursor back.

**This is a correctness bug today, independent of store choice, and Redis's execution model
is the cheapest place to fix it.** Because every command runs atomically with respect to all
other clients:

```
SET sapo:claim:<session_id> <execution_id> NX PX 5000     -- one O(1) op: "I own this session"
```

…is a complete mutual-exclusion primitive. And a version field plus a five-line Lua script
gives optimistic concurrency with no lock convoy, no deadlock risk, and no contention
profile to model. With a multi-threaded store you need the same discipline but must reason
harder about where the atomicity boundary actually is.

Secondary wins for the same reason: no locks in the store, predictable p99 on small ops, and
`SET ... EX` / `GETEX` give TTL-based expiry of abandoned sessions for free — which is
exactly the "sessions die with the process" problem `InMemoryStateStore` has.

---

## 6. Tarantool: what it genuinely adds

Tarantool is an in-memory database *plus a Lua application server*. Latest line is 3.x
(3.8.1 shipped Sep 2026); 2.11 LTS is supported to May 2027.

**6.1 Real transactions with MVCC.** `memtx_use_mvcc_engine=true` enables the transaction
manager for memtx: yields inside a transaction are allowed, uncommitted changes are invisible
to other fibers, and conflicting transactions are rolled back
([docs](https://www.tarantool.io/en/doc/latest/platform/atomic/txn_mode_mvcc/)). IPROTO
streams + interactive transactions (since 2.10) let a remote client drive a multi-request
transaction. So load-modify-save becomes a genuine ACID transaction rather than an
optimistic retry loop. This is the cleanest fix for §5 of any option.

**6.2 Secondary indexes fix `list()` / `count()` for real.** Declare a space with fields
`(session_id, status, blueprint_id, msisdn, updated_ms, blob)` and indexes on `status` and
`updated_ms`. Then `count(AwaitingInput)` is an index count and the ops dashboard is a range
scan — not a `SCAN` over a million keys. On Redis you hand-build and maintain that index;
here it is a schema declaration. **This is the single strongest Tarantool argument, and it
targets the weakest part of the current `IStateStore` surface.**

**6.3 Durability is the primary path, not an add-on.** WAL + snapshots + synchronous
replication with automated leader election. Redis `appendfsync everysec` can lose ~1 s of
writes on power loss; for payment session state, decide explicitly whether that is
acceptable. Tarantool's answer is stronger out of the box.

**6.4 Logic next to data.** Push the *transactional wrapper* into a stored Lua procedure —
claim the session, CAS on version, write blob + index atomically — and you eliminate both
the extra RTT and the race in one move.

> **Do not** push the AST execution into Lua. That contradicts the whole design in
> `docs/INTEGRATING.md` §1 (link `libsapo_core.a`, keep the engine in C++, no extra hop in a
> 2-second window). Keep execution in C++; let Tarantool own only the atomicity boundary.

**6.5 It could collapse Redis + RabbitMQ.** Your hybrid architecture runs RabbitMQ for
background work. A Tarantool `task` space with an index on `available_at`, plus the queue
module, is a durable priority queue whose dequeue is transactional — the same system that
holds session state. One dependency instead of two, and the "worker claims task + updates
session" step becomes a single transaction. **This is the architecture-simplification case
for Tarantool, and it has nothing to do with throughput.**

**6.6 Same story for durable timers.** `Timer.context_checkpoint_key`
([`src/runtime/Scheduler.hpp:105`](../src/runtime/Scheduler.hpp)) couples the timer queue to
the store, but `Scheduler` itself is in-process. On a multi-node fleet, *who fires a
`wait`-node timer when the session lives on another node?* You need a shared durable timer
queue either way: a space indexed on `due_ms` with a transactional claim, or a Redis ZSET
scored by `due_ms` popped atomically in Lua. Neither store gives this to you for free.

**6.7 Licensing.** BSD-2. Redis 8 is tri-licensed RSALv2 / SSPLv1 / AGPLv3. AGPL only bites
if you *modify* Redis and serve it over a network; running unmodified Redis 8 behind your
API is fine. If you want a permissive licence with no analysis, **Valkey** (BSD-3, Linux
Foundation, wire-compatible, default on AWS ElastiCache/MemoryDB) is a drop-in.

### The multi-threading claim, deflated

"Tarantool is multi-threaded, Redis is single-threaded" is a much smaller difference than it
sounds for this workload. Tarantool's transaction processing happens on TX threads running
cooperatively-scheduled **fibers**; parallelism comes from non-blocking I/O, WAL writers on
separate threads, and multiple TX threads — not from N cores mutating the same hash table
concurrently. Since your operations are single-key and O(1), Redis's one core already does
30–50× your peak. **Do not choose Tarantool for throughput. Choose it for §6.1, §6.2, §6.3,
§6.5.**

---

## 7. Work required regardless of which store you pick

This is the part that actually determines whether the deployment is correct. None of it is
optional, and it is mostly store-agnostic.

**7.1 Add optimistic concurrency to the SPI.** `SessionCheckpoint` gains a `version` (or
`etag`) field; `IStateStore` gains a CAS save:

```cpp
enum class SaveResult { Ok, VersionConflict, Gone };
virtual SaveResult saveIf(const SessionCheckpoint& cp, int64_t expected_version) = 0;
```

Without this, *no* store can protect you. With it, `resumeSession` retries-or-rejects on
conflict instead of silently last-writer-wins. This is a small, contained change to
`StateStore.hpp` and the three call sites in `Interpreter.cpp`
(`resumeSession`, `cancelSession`, `finishRun`).

**7.2 Make resume idempotent.** A claim key (`SET ... NX PX`) taken before execution means a
duplicate USSD request returns the cached prompt rather than re-running a payment node.
Store the last rendered prompt in the checkpoint so the retry has something to return.

**7.3 Bound and measure the blob.** Compact JSON; size histogram; alarm at 64 KB; document
that blueprints must not accumulate collections in `context`.

**7.4 Maintain a status index in the same atomic write.** Redis: one Lua script doing
`SET blob` + `ZADD sapo:idx:status:<status> <updated_ms> <session_id>` + `ZREM` from the old
status + `EXPIRE`. Tarantool: one transaction over the tuple. Either way `count()` becomes
O(1) and `list()` becomes a bounded range scan.

**7.5 Fold round trips.** You cannot pipeline load→save (save depends on load), but you
*can*: one `EVAL` for the whole write path (blob + index + TTL = 1 RTT instead of 3), and
`GET` + claim `SET NX` in one pipeline on the read path. Consider a Unix socket when
co-located.

**7.6 Pool the client.** Blocking calls on engine worker threads ⇒ pool sized to worker
count. A shared connection behind a mutex re-creates single-threading inside your process.

**7.7 Fix the cross-node wakeup gap.** `EventBus`
([`src/runtime/EventBus.hpp`](../src/runtime/EventBus.hpp)) is explicitly in-process:
*"No persistence, no replay beyond the recent-event ring."* A session suspended awaiting
`payment.confirmed` on node A will **not** be woken by that event arriving at node B. Redis
pub/sub or Streams (or Tarantool space triggers) closes this. Note that Redis keyspace
notifications are *not* reliable — undelivered when no subscriber is connected — so do not
build the durable timer path on them.

**7.8 Topology.** Primary + 2 replicas. Replicas take persistence, `list()`/`count()`, and
read-only admin. Shard by session id only when one core saturates (~30–50× your peak).

---

## 8. Security: the checkpoint carries secrets

[`.sapo-state/deposit-test.json`](../.sapo-state/deposit-test.json) contains:

```json
"pin": "7002",
"clientSecret": "47809b129a3549c48"
```

`RuntimeContext::snapshot()` copies these into `SessionCheckpoint.context`, which is written
verbatim to the state store. In Redis that means: readable by anyone with `GET` access,
present in RDB/AOF files on disk, present in replica traffic, and included in any `DEBUG`
output. Your logger already has secret redaction (`ProviderConfigStore::redactList`,
`applySecretRedaction`) — the state store has no equivalent.

This is **not** a Redis-vs-Tarantool question; both are in-memory stores with the same
exposure. But it changes the deployment requirements for whichever you pick:

- Never let the checkpoint be the place a PIN lives. Encrypt or tokenize sensitive context
  fields before they reach `save()`, or keep them in a separate short-lived sealed record.
- TLS in transit (`redis.conf` TLS / IPROTO TLS), ACLs with per-command scoping, no
  world-readable dump files.
- Encryption at rest, or accept it explicitly in writing.
- Short TTLs limit the exposure window — another point in favour of Redis's `SET ... EX`.

---

## 9. Decision

| Criterion | Redis / Valkey | Tarantool | PostgreSQL |
|---|---|---|---|
| Peak throughput need (~2.8 k ops/s) | ✅ 30–50× headroom | ✅ | ⚠️ fine, but 10–20× the latency |
| p99 latency in a 2 s window | ✅ sub-ms | ✅ sub-ms | ⚠️ ms-range, connection churn |
| Single-key atomicity / CAS | ✅ free (single-threaded + Lua) | ✅ real ACID transactions | ✅ |
| `list()` / `count()` by status | ❌ hand-built index | ✅ native secondary indexes | ✅ native |
| TTL expiry of abandoned sessions | ✅ first-class `SET EX` | ⚠️ via a cleanup fiber/trigger | ⚠️ via a job |
| Durability on power loss | ⚠️ AOF `everysec` ≈ 1 s loss | ✅ WAL | ✅ |
| C++ client maturity | ✅ hiredis / redis-plus-plus, huge install base | ⚠️ `tntcxx` header-only, ~10 stars, 53 open issues, active (last push Nov 2025) | ✅ libpq |
| Ops familiarity (telco/Ghana stack) | ✅ ubiquitous | ❌ thin talent pool, bus-factor risk | ✅ |
| Could replace RabbitMQ too | ⚠️ Streams, partially | ✅ queue module + transactions | ⚠️ `SKIP LOCKED`, well-trodden |
| Licence | ⚠️ AGPLv3 tri (Valkey: BSD-3) | ✅ BSD-2 | ✅ PostgreSQL |
| Fit with existing `IStateStore` seam | ✅ ~40 lines, already sketched in `INTEGRATING.md` §4 | ⚠️ tuple mapping + Lua procs | ⚠️ SQL + pool |

### Recommendation

**Phase 1 — ship Redis (or Valkey) as the hot session store.** It fits the access pattern
(one key, whole-blob, TTL'd), it is already the documented plan, the adapter is ~40 lines
against the existing seam, and the throughput objection does not survive arithmetic. Budget
the effort for §7.1, §7.2 and §7.7 — the CAS, the idempotency claim, and the cross-node
wakeup — because those are correctness work you owe regardless of store.

**Phase 2 — add PostgreSQL as the cold/audit tier.** Checkpoints TTL out of Redis after the
session window; an append-only history row per session gives you audit-grade retention and
joins against business tables. `implementation_plan_2.md` T4.1 already lists both adapters.

**Choose Tarantool instead only if** at least two of these are true for you:

1. You need to *query* live session state (ops dashboards, "how many sessions stuck at
   `menu_withdrawal`", per-blueprint funnel analysis) and do not want a separate PG for it.
2. You need ACID transactions spanning the session **and** another table (ledger, idempotency
   keys) in one commit.
3. You want to retire RabbitMQ and run the background queue in the same system as the state.
4. You need WAL-grade durability on the hot path and cannot accept ~1 s of AOF loss.

Otherwise Tarantool adds a thin C++ connector, a Lua surface you must maintain, and a scarce
skill set — in exchange for throughput you will never use.

### A note on the third option

If you are evaluating Tarantool primarily for durability and querying, **PostgreSQL is the
lower-risk way to get both**, at the cost of latency you can afford *if* you keep the hot
path in Redis. Redis-hot + PG-cold gives you Redis's p99 and PG's auditability without
betting the engine's only C++ dependency on a 10-star connector.

---

## 10. Suggested next steps

- [ ] Decide Redis vs Valkey (licence posture; Valkey is wire-compatible, so this is
      reversible and can be deferred).
- [ ] Extend `IStateStore` with `version` + `saveIf()` (§7.1) — do this **before** writing
      any adapter, so the seam is right the first time. Add it to `InMemoryStateStore` and
      `FileStateStore` first, with tests; that also reproduces the §5 race in CI.
- [ ] Add a `tests/test_concurrent_resume.cpp` that fires two resumes at one suspended
      payment session and asserts the `command` node ran exactly once.
- [ ] Implement `RedisStateStore` properly — not the §4 sketch: pipelined claim, Lua CAS
      save, status index, compact JSON, pooled connections, blob-size metric.
- [ ] Design the cross-node wakeup (§7.7) and the shared durable timer queue (§6.6).
- [ ] Resolve §8 before any production data lands in a store.
- [ ] Load-test at 3× your estimated peak with realistic blob sizes; watch p99, not mean.
