//
//  Sapo Engine — RedisStateStore (implementation_plan_2.md T4.1).
//
//  Runs against `MockRedisClient`, so it needs no server and no network: the
//  point is to pin down the store's key layout, CAS semantics, index
//  maintenance, TTL behaviour and error posture. The Lua scripts themselves are
//  only really executed by the opt-in live-server case at the bottom
//  (SAPO_REDIS_URL) — see the note in MockRedisClient.hpp.
//
#include "test_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "redis/IRedisClient.hpp"
#include "redis/MockRedisClient.hpp"
#include "redis/RedisStateStore.hpp"
#if defined(SAPO_ENABLE_REDIS)
#include "redis/SocketRedisClient.hpp"
#endif

using namespace sapo;
using nlohmann::json;
using runtime::SaveResult;
using runtime::SessionCheckpoint;
using runtime::SessionStatus;

namespace {

    SessionCheckpoint makeCheckpoint(const std::string &id, SessionStatus status, const std::string &cursor = "menu") {
        SessionCheckpoint checkpoint;
        checkpoint.session_id = id;
        checkpoint.execution_id = "exec-" + id;
        checkpoint.blueprint_id = "daccu_ussd_service";
        checkpoint.blueprint_version = "1.0";
        checkpoint.correlation_id = "corr-" + id;
        checkpoint.status = status;
        checkpoint.cursor = cursor;
        checkpoint.context = json{{"phoneNo", "+233242602262"}, {"network", "mtn"}, {"currency", "GHS"}};
        checkpoint.frames = json::array();
        checkpoint.pending = json{{"node", cursor}, {"input_variable", "choice"}};
        checkpoint.created_ms = 1'700'000'000'000LL;
        checkpoint.updated_ms = 1'700'000'000'000LL;
        checkpoint.node_visits = 3;
        return checkpoint;
    }

    struct Harness {
        redis::MockRedisClientPtr mock;
        std::shared_ptr<redis::RedisStateStore> store;

        explicit Harness(redis::RedisStateStoreOptions options = {})
            : mock(std::make_shared<redis::MockRedisClient>()),
              store(std::make_shared<redis::RedisStateStore>(mock, std::move(options))) {}
    };

} // namespace

// ===========================================================================
// options
// ===========================================================================
TEST_CASE("redis:// URLs parse into connection options", "[redis][options]") {
    SECTION("bare host") {
        const auto options = redis::RedisOptions::fromUrl("redis://cache.internal");
        REQUIRE(options.has_value());
        CHECK(options->host == "cache.internal");
        CHECK(options->port == 6379);
        CHECK(options->database == 0);
        CHECK(options->password.empty());
    }

    SECTION("host, port and database") {
        const auto options = redis::RedisOptions::fromUrl("redis://10.0.0.5:6380/3");
        REQUIRE(options.has_value());
        CHECK(options->host == "10.0.0.5");
        CHECK(options->port == 6380);
        CHECK(options->database == 3);
    }

    SECTION("password only, and percent-decoded") {
        const auto options = redis::RedisOptions::fromUrl("redis://:p%40ssw0rd@cache:6379");
        REQUIRE(options.has_value());
        CHECK(options->username.empty());
        CHECK(options->password == "p@ssw0rd");
    }

    SECTION("ACL user and password") {
        const auto options = redis::RedisOptions::fromUrl("redis://sapo:s3cret@cache:6379/1");
        REQUIRE(options.has_value());
        CHECK(options->username == "sapo");
        CHECK(options->password == "s3cret");
        CHECK(options->database == 1);
    }

    SECTION("bracketed IPv6 literal") {
        const auto options = redis::RedisOptions::fromUrl("redis://[::1]:6390/2");
        REQUIRE(options.has_value());
        CHECK(options->host == "::1");
        CHECK(options->port == 6390);
        CHECK(options->database == 2);
    }

    SECTION("rediss:// is refused rather than silently downgraded to plaintext") {
        std::string problem;
        CHECK_FALSE(redis::RedisOptions::fromUrl("rediss://cache:6379", &problem).has_value());
        CHECK(problem.find("TLS") != std::string::npos);
    }

    SECTION("malformed URLs are rejected with a reason") {
        std::string problem;
        CHECK_FALSE(redis::RedisOptions::fromUrl("http://cache", &problem).has_value());
        CHECK(problem.find("redis://") != std::string::npos);
        CHECK_FALSE(redis::RedisOptions::fromUrl("redis://cache:notaport", &problem).has_value());
        CHECK_FALSE(redis::RedisOptions::fromUrl("redis://cache:99999", &problem).has_value());
        CHECK_FALSE(redis::RedisOptions::fromUrl("redis://", &problem).has_value());
    }

    SECTION("describe() never leaks the password") {
        const auto options = redis::RedisOptions::fromUrl("redis://sapo:s3cret@cache:6379/1");
        REQUIRE(options.has_value());
        const auto text = options->describe();
        CHECK(text.find("s3cret") == std::string::npos);
        CHECK(text.find("cache:6379/1") != std::string::npos);
        CHECK(text.find("auth=yes") != std::string::npos);
    }
}

// ===========================================================================
// round trip
// ===========================================================================
TEST_CASE("a checkpoint survives a save/load round trip", "[redis][store]") {
    Harness harness;
    const auto original = makeCheckpoint("sess-1", SessionStatus::AwaitingInput, "menu_amount");

    harness.store->save(original);
    const auto loaded = harness.store->load("sess-1");

    REQUIRE(loaded.has_value());
    CHECK(loaded->session_id == "sess-1");
    CHECK(loaded->execution_id == "exec-sess-1");
    CHECK(loaded->blueprint_id == "daccu_ussd_service");
    CHECK(loaded->blueprint_version == "1.0");
    CHECK(loaded->correlation_id == "corr-sess-1");
    CHECK(loaded->status == SessionStatus::AwaitingInput);
    CHECK(loaded->cursor == "menu_amount");
    CHECK(loaded->context["phoneNo"] == "+233242602262");
    CHECK(loaded->pending["input_variable"] == "choice");
    CHECK(loaded->node_visits == 3);
    CHECK(loaded->created_ms == 1'700'000'000'000LL);

    SECTION("a missing session is absent, not an error") {
        CHECK_FALSE(harness.store->load("nope").has_value());
    }

    SECTION("kind() identifies the store") {
        CHECK(harness.store->kind() == "redis:sapo:session:");
    }

    SECTION("the blob is stored compact, not pretty-printed") {
        const auto blob = harness.mock->hashField("sapo:session:sess-1", "blob");
        REQUIRE(blob.has_value());
        CHECK(blob->find('\n') == std::string::npos); // dump(), not dump(2)
        CHECK(blob->size() < original.toJson().dump(2).size());
    }

    SECTION("an empty session id is refused rather than writing a bare prefix key") {
        CHECK_THROWS_AS(harness.store->load(""), runtime::SapoError);
        CHECK_THROWS_AS(harness.store->save(makeCheckpoint("", SessionStatus::Running)), runtime::SapoError);
    }
}

// ===========================================================================
// compare and swap — the reason this adapter exists
// ===========================================================================
TEST_CASE("saveIf is a genuine compare-and-swap", "[redis][cas]") {
    Harness harness;
    auto checkpoint = makeCheckpoint("pay-1", SessionStatus::AwaitingInput);
    harness.store->save(checkpoint);

    const auto created = harness.store->load("pay-1");
    REQUIRE(created.has_value());
    CHECK(created->version == 1); // first write assigns version 1

    SECTION("the expected version writes and bumps") {
        const auto outcome = harness.store->saveIf(*created, 1);
        CHECK(outcome.ok());
        CHECK(outcome.version == 2);
        CHECK(harness.store->load("pay-1")->version == 2);
    }

    SECTION("a stale expected version writes nothing and reports the winner") {
        REQUIRE(harness.store->saveIf(*created, 1).ok());
        const auto stale = harness.store->saveIf(*created, 1); // same expectation, now outdated
        CHECK(stale.result == SaveResult::VersionConflict);
        CHECK(stale.version == 2); // the winner's version, so the caller can reload
        CHECK(harness.store->load("pay-1")->cursor == "menu"); // untouched
    }

    SECTION("expected 0 creates, and only once") {
        const auto fresh = makeCheckpoint("pay-2", SessionStatus::Running);
        const auto outcome = harness.store->saveIf(fresh, 0);
        CHECK(outcome.ok());
        CHECK(outcome.version == 1);
        const auto again = harness.store->saveIf(fresh, 0);
        CHECK(again.result == SaveResult::VersionConflict);
    }

    SECTION("a nonzero expectation against a missing session reports Gone") {
        const auto outcome = harness.store->saveIf(makeCheckpoint("never-saved", SessionStatus::Running), 7);
        CHECK(outcome.result == SaveResult::Gone);
        CHECK_FALSE(harness.store->load("never-saved").has_value());
    }

    SECTION("unconditional save also advances the version") {
        harness.store->save(*created);
        CHECK(harness.store->load("pay-1")->version == 2);
        harness.store->save(*created);
        CHECK(harness.store->load("pay-1")->version == 3);
    }

    SECTION("the hash field and the loaded version agree") {
        const auto outcome = harness.store->saveIf(*created, 1);
        REQUIRE(outcome.ok());
        const auto field = harness.mock->hashField("sapo:session:pay-1", "version");
        REQUIRE(field.has_value());
        CHECK(std::stoll(*field) == harness.store->load("pay-1")->version);
    }

    SECTION("saveIf refuses a negative expectation instead of silently overwriting") {
        CHECK_THROWS_AS(harness.store->saveIf(*created, -1), runtime::SapoError);
    }

    SECTION("the store advertises CAS support") {
        CHECK(harness.store->supportsCompareAndSwap());
    }
}

TEST_CASE("racing writers on one session produce exactly one winner", "[redis][cas][concurrency]") {
    // This is the shape of the real failure: a gateway retry, a redial, or an
    // API node and a queue worker all resume the same suspended session at
    // once. Without CAS the last writer wins and the side-effecting `command`
    // node runs once per racer.
    Harness harness;
    harness.store->save(makeCheckpoint("race", SessionStatus::AwaitingInput));
    const auto loaded = harness.store->load("race");
    REQUIRE(loaded.has_value());
    const int64_t version = loaded->version;

    constexpr int kRacers = 8;
    std::atomic<int> wins{0};
    std::atomic<int> conflicts{0};
    std::vector<std::string> winning_cursors(kRacers);
    std::vector<std::thread> racers;
    racers.reserve(kRacers);
    for (int index = 0; index < kRacers; ++index) {
        racers.emplace_back([&, index] {
            auto attempt = *loaded;
            attempt.cursor = "paid_by_" + std::to_string(index);
            if (harness.store->saveIf(attempt, version).ok()) {
                ++wins;
                winning_cursors[index] = attempt.cursor;
            } else {
                ++conflicts;
            }
        });
    }
    for (auto &racer : racers) racer.join();

    CHECK(wins.load() == 1);
    CHECK(conflicts.load() == kRacers - 1);

    const auto after = harness.store->load("race");
    REQUIRE(after.has_value());
    CHECK(after->version == version + 1); // exactly one write landed
    const auto winner = std::find_if(winning_cursors.begin(), winning_cursors.end(),
                                     [](const std::string &text) { return !text.empty(); });
    REQUIRE(winner != winning_cursors.end());
    CHECK(after->cursor == *winner);
}

// ===========================================================================
// index: list() and count() without a SCAN
// ===========================================================================
TEST_CASE("the status index answers count() and list() without scanning", "[redis][index]") {
    Harness harness;
    harness.store->save(makeCheckpoint("s1", SessionStatus::AwaitingInput));
    harness.store->save(makeCheckpoint("s2", SessionStatus::AwaitingInput));
    harness.store->save(makeCheckpoint("s3", SessionStatus::Completed));
    harness.store->save(makeCheckpoint("s4", SessionStatus::Waiting));

    CHECK(harness.store->count(SessionStatus::AwaitingInput) == 2);
    CHECK(harness.store->count(SessionStatus::Completed) == 1);
    CHECK(harness.store->count(SessionStatus::Waiting) == 1);
    CHECK(harness.store->count(SessionStatus::Failed) == 0);
    CHECK(harness.store->count(std::nullopt) == 4);
    CHECK(harness.store->list().size() == 4);

    SECTION("neither KEYS nor SCAN is ever issued") {
        // The INTEGRATING.md §4 sketch answered list() with SCAN + a GET per
        // key and count() with `return 0`. On a million-key instance that is a
        // stall on Redis's single command thread.
        CHECK(harness.mock->countCalls("SCAN") == 0);
        CHECK(harness.mock->countCalls("KEYS") == 0);
        CHECK(harness.mock->countCalls("ZCARD") > 0);
    }

    SECTION("a status transition moves index membership") {
        auto moved = harness.store->load("s1");
        REQUIRE(moved.has_value());
        moved->status = SessionStatus::Completed;
        REQUIRE(harness.store->saveIf(*moved, moved->version).ok());

        CHECK(harness.store->count(SessionStatus::AwaitingInput) == 1);
        CHECK(harness.store->count(SessionStatus::Completed) == 2);
        CHECK(harness.store->count(std::nullopt) == 4); // moved, not duplicated
    }

    SECTION("list() is ordered by session id, matching the other stores") {
        const auto all = harness.store->list();
        REQUIRE(all.size() == 4);
        CHECK(all[0].session_id == "s1");
        CHECK(all[3].session_id == "s4");
    }

    SECTION("list() is bounded by max_list") {
        redis::RedisStateStoreOptions options;
        options.max_list = 2;
        Harness bounded(options);
        for (int index = 0; index < 5; ++index) {
            bounded.store->save(makeCheckpoint("b" + std::to_string(index), SessionStatus::Running));
        }
        CHECK(bounded.store->count(std::nullopt) == 5); // the index still knows about all of them
        CHECK(bounded.store->list().size() == 2);
    }

    SECTION("remove() clears the hash and its index membership") {
        CHECK(harness.store->remove("s3"));
        CHECK_FALSE(harness.store->load("s3").has_value());
        CHECK(harness.store->count(SessionStatus::Completed) == 0);
        CHECK(harness.store->count(std::nullopt) == 3);
        CHECK_FALSE(harness.store->remove("s3")); // second remove is a no-op, not a success
    }
}

TEST_CASE("cluster-safe mode keeps the write single-key and indexes separately", "[redis][cluster]") {
    redis::RedisStateStoreOptions options;
    options.atomic_index = false; // Redis Cluster would reject CROSSSLOT otherwise
    Harness harness(options);

    harness.store->save(makeCheckpoint("c1", SessionStatus::AwaitingInput));
    CHECK(harness.store->count(SessionStatus::AwaitingInput) == 1);
    CHECK(harness.store->count(std::nullopt) == 1);

    // The CAS script must touch only the session hash, so it stays legal on a
    // sharded deployment.
    for (const auto &command : harness.mock->recordedCommands()) {
        if (command.empty() || command[0] != "EVAL") continue;
        const int numkeys = std::atoi(command[2].c_str());
        CHECK(numkeys == 1);
    }

    const auto loaded = harness.store->load("c1");
    REQUIRE(loaded.has_value());
    auto next = *loaded;
    next.status = SessionStatus::Completed;
    REQUIRE(harness.store->saveIf(next, loaded->version).ok());
    CHECK(harness.store->count(SessionStatus::AwaitingInput) == 0);
    CHECK(harness.store->count(SessionStatus::Completed) == 1);
}

// ===========================================================================
// TTL, expiry and maintenance
// ===========================================================================
TEST_CASE("sessions expire, and the index is reconciled afterwards", "[redis][ttl][maintenance]") {
    redis::RedisStateStoreOptions options;
    options.ttl_seconds = 10;
    Harness harness(options);

    harness.store->save(makeCheckpoint("t1", SessionStatus::AwaitingInput));
    harness.store->save(makeCheckpoint("t2", SessionStatus::AwaitingInput));

    SECTION("the TTL is applied on write") {
        const auto ttl = harness.mock->ttlSeconds("sapo:session:t1");
        REQUIRE(ttl.has_value());
        CHECK(*ttl == 10);
    }

    SECTION("an expired session reads as absent") {
        harness.mock->advanceTime(11'000);
        CHECK_FALSE(harness.store->load("t1").has_value());
    }

    SECTION("count() drifts until pruned, then reconciles") {
        harness.mock->advanceTime(11'000);
        // The hash TTLed out but the ZSET did not: this is the documented drift.
        CHECK(harness.store->count(std::nullopt) == 2);
        CHECK(harness.store->pruneExpired() == 2);
        CHECK(harness.store->count(std::nullopt) == 0);
        CHECK(harness.store->count(SessionStatus::AwaitingInput) == 0);
        CHECK(harness.store->pruneExpired() == 0); // idempotent
    }

    SECTION("list() prunes lazily as it walks") {
        harness.mock->advanceTime(11'000);
        CHECK(harness.store->list().empty());
        CHECK(harness.store->count(std::nullopt) == 0); // self-healed without pruneExpired()
    }

    SECTION("ttl_seconds = 0 keeps sessions indefinitely") {
        redis::RedisStateStoreOptions persistent;
        persistent.ttl_seconds = 0;
        Harness harness2(persistent);
        harness2.store->save(makeCheckpoint("forever", SessionStatus::Waiting));
        CHECK(harness2.mock->ttlSeconds("sapo:session:forever") == -1); // no expiry
        harness2.mock->advanceTime(60 * 60 * 1000);
        CHECK(harness2.store->load("forever").has_value());
    }
}

// ===========================================================================
// claims: the idempotency mutex
// ===========================================================================
TEST_CASE("tryClaim gives one holder per session", "[redis][claim]") {
    Harness harness;
    harness.store->save(makeCheckpoint("claim-1", SessionStatus::AwaitingInput));

    CHECK(harness.store->tryClaim("claim-1", "exec-A"));
    CHECK_FALSE(harness.store->tryClaim("claim-1", "exec-B")); // a duplicate request loses

    SECTION("only the holder can release") {
        CHECK_FALSE(harness.store->releaseClaim("claim-1", "exec-B"));
        CHECK(harness.store->releaseClaim("claim-1", "exec-A"));
        CHECK(harness.store->tryClaim("claim-1", "exec-B")); // free again
    }

    SECTION("an expired claim cannot wedge the session") {
        // The holder crashed without releasing; PX bounds the damage.
        harness.mock->advanceTime(6'000); // default ttl is 5000ms
        CHECK(harness.store->tryClaim("claim-1", "exec-C"));
    }

    SECTION("arguments are validated") {
        CHECK_THROWS_AS(harness.store->tryClaim("claim-2", ""), runtime::SapoError);
        CHECK_THROWS_AS(harness.store->tryClaim("claim-2", "tok", 0), runtime::SapoError);
    }
}

// ===========================================================================
// error posture
// ===========================================================================
TEST_CASE("store failures are loud, never silent", "[redis][errors]") {
    Harness harness;
    harness.store->save(makeCheckpoint("e1", SessionStatus::AwaitingInput));

    SECTION("a server error surfaces as SapoError, not an empty optional") {
        harness.mock->failNextCommands(1, "ERR something went wrong");
        CHECK_THROWS_AS(harness.store->load("e1"), runtime::SapoError);
    }

    SECTION("a transport outage surfaces too") {
        harness.mock->setDown(true);
        CHECK_THROWS_AS(harness.store->load("e1"), runtime::SapoError);
        CHECK_THROWS_AS(harness.store->count(std::nullopt), runtime::SapoError);
        CHECK_THROWS_AS(harness.store->save(makeCheckpoint("e2", SessionStatus::Running)), runtime::SapoError);
    }

    SECTION("a corrupt checkpoint is not mistaken for a missing session") {
        // Silent nullopt here would restart a payment conversation from scratch.
        const auto seeded = harness.mock->command({"HSET", "sapo:session:bad", "blob", "{not json", "version", "4"});
        seeded.throwIfError("test setup");
        CHECK_THROWS_AS(harness.store->load("bad"), runtime::SapoError);
    }

    SECTION("a truncated EVAL reply is reported") {
        harness.mock->onScript("-- sapo:session.save v1",
                               [](const std::vector<std::string> &, const std::vector<std::string> &) {
                                   return redis::RedisValue::array({redis::RedisValue::integer(1)});
                               });
        CHECK_THROWS_AS(harness.store->save(makeCheckpoint("e3", SessionStatus::Running)), runtime::SapoError);
    }

    SECTION("an unmirrored script is refused rather than silently ignored") {
        // Guards against a typo'd marker making EVAL a no-op that looks like success.
        const auto reply = harness.mock->command({"EVAL", "-- some other script\nreturn 1", "0"});
        CHECK(reply.isError());
        const auto text = reply.toString();
        REQUIRE(text.has_value());
        CHECK(text->find("no mirror") != std::string::npos);
    }

    SECTION("a null client is rejected at construction") {
        CHECK_THROWS_AS(redis::RedisStateStore{nullptr}, runtime::SapoError);
    }

    SECTION("empty prefixes are rejected at construction") {
        redis::RedisStateStoreOptions options;
        options.key_prefix = "";
        CHECK_THROWS_AS(redis::RedisStateStore(std::make_shared<redis::MockRedisClient>(), options),
                        runtime::SapoError);
    }
}

// ===========================================================================
// wire economy
// ===========================================================================
TEST_CASE("the hot path costs one round trip", "[redis][wire]") {
    Harness harness;

    harness.store->save(makeCheckpoint("w1", SessionStatus::AwaitingInput));
    CHECK(harness.mock->countCalls("EVAL") == 1);
    CHECK(harness.mock->countCalls("HSET") == 0); // the script does it, not a second round trip
    CHECK(harness.mock->countCalls("EXPIRE") == 0);
    CHECK(harness.mock->countCalls("ZADD") == 0); // folded into the script when atomic_index

    const size_t before = harness.mock->recordedCommands().size();
    const auto loaded = harness.store->load("w1");
    REQUIRE(loaded.has_value());
    CHECK(harness.mock->recordedCommands().size() - before == 1); // one HMGET

    REQUIRE(harness.store->saveIf(*loaded, loaded->version).ok());
    CHECK(harness.mock->countCalls("EVAL") == 2); // still one command per write
}

TEST_CASE("NullRedisClient fails loudly instead of pretending", "[redis][null]") {
    redis::NullRedisClient client;
    CHECK(client.name() == "null");
    CHECK_FALSE(client.healthy());
    CHECK_THROWS_AS(client.command({"GET", "k"}), runtime::SapoError);
    CHECK_THROWS_AS(client.pipeline({{"PING"}}), runtime::SapoError);
    CHECK(client.pipeline({}).empty()); // nothing to do is not an error
}

#if defined(SAPO_ENABLE_REDIS)
TEST_CASE("RESP2 encoding is length-prefixed and binary safe", "[redis][wire][resp]") {
    CHECK(redis::SocketRedisClient::encodeCommand({"GET", "k"}) == "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
    CHECK(redis::SocketRedisClient::encodeCommand({"PING"}) == "*1\r\n$4\r\nPING\r\n");
    // A value containing CRLF must survive: framing is by length, never by delimiter.
    // A session context holding a newline is otherwise a protocol injection hole.
    const std::string payload = "line\r\n*2\r\n$3\r\nGET";
    const auto encoded = redis::SocketRedisClient::encodeCommand({"SET", "k", payload});
    CHECK(encoded == "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$" + std::to_string(payload.size()) + "\r\n" + payload + "\r\n");
}
#endif

// ===========================================================================
// the SPI contract, uniformly across adapters
// ===========================================================================
TEST_CASE("every state store agrees on version semantics", "[statestore][cas]") {
    // A caller's expectations must not depend on which adapter happens to be
    // wired up, so the same script of assertions runs against all three.
    const auto temporary = std::filesystem::temp_directory_path() / "sapo-version-semantics";
    std::filesystem::remove_all(temporary);

    const std::vector<std::pair<std::string, runtime::StateStorePtr>> stores{
        {"memory", std::make_shared<runtime::InMemoryStateStore>()},
        {"file", std::make_shared<runtime::FileStateStore>(temporary.string())},
        {"redis", std::make_shared<redis::RedisStateStore>(std::make_shared<redis::MockRedisClient>())},
    };

    for (const auto &[name, store] : stores) {
        INFO("store: " << name);
        CHECK(store->supportsCompareAndSwap());

        const auto checkpoint = makeCheckpoint("v1", SessionStatus::AwaitingInput);
        CHECK(checkpoint.version == 0); // the caller never assigns it

        store->save(checkpoint);
        auto loaded = store->load("v1");
        REQUIRE(loaded.has_value());
        CHECK(loaded->version == 1); // the store assigned it on first write

        store->save(checkpoint);
        CHECK(store->load("v1")->version == 2); // unconditional writes still advance

        const auto current = store->load("v1");
        REQUIRE(current.has_value());
        const auto won = store->saveIf(*current, current->version);
        CHECK(won.ok());
        CHECK(won.version == 3);

        const auto lost = store->saveIf(*current, current->version); // now stale
        CHECK(lost.result == SaveResult::VersionConflict);
        CHECK(lost.version == 3);
        CHECK(store->load("v1")->version == 3); // the loser wrote nothing

        CHECK(store->remove("v1"));
        CHECK_FALSE(store->load("v1").has_value());
        CHECK(store->saveIf(makeCheckpoint("v1", SessionStatus::Running), 9).result == SaveResult::Gone);
    }
    std::filesystem::remove_all(temporary);
}

// ===========================================================================
// config wiring (sapo-config.json → engine.state_redis / engine.state_dir)
// ===========================================================================
namespace {

    /// Writes a config file and starts a VM against the fixture's services.
    struct ConfigRun {
        std::filesystem::path directory;
        std::vector<std::string> problems;
        runtime::StateStorePtr store;

        ConfigRun(const std::string &name, const std::string &engine_json,
                  const runtime::StateStorePtr &injected = nullptr) {
            directory = std::filesystem::temp_directory_path() / ("sapo-cfg-" + name);
            std::filesystem::remove_all(directory);
            std::filesystem::create_directories(directory);
            const auto path = directory / "sapo-config.json";
            {
                std::ofstream out(path);
                out << R"JSON({"version": "1.0", "engine": )JSON" << engine_json << "}";
            }
            testing::Fixture fixture;
            if (injected != nullptr) fixture.services.state_store = injected;
            runtime::VirtualMachine vm(fixture.services);
            vm.setConfigPath(path.string());
            problems = vm.start();
            store = vm.services().state_store;
            vm.stop();
        }

        ~ConfigRun() { std::filesystem::remove_all(directory); }

        [[nodiscard]] bool anyProblemContains(const std::string &needle) const {
            return std::any_of(problems.begin(), problems.end(),
                               [&](const std::string &text) { return text.find(needle) != std::string::npos; });
        }
    };

} // namespace

TEST_CASE("engine.state_dir never clobbers an injected shared store", "[config][statestore]") {
    // Regression guard: the previous condition reduced to `!directory.empty()`,
    // so a config state_dir replaced *any* store — including a shared one. That
    // silently splits a fleet's sessions across node-local disks and breaks the
    // cross-node resume the shared store exists to provide.
    const auto shared = std::make_shared<redis::RedisStateStore>(std::make_shared<redis::MockRedisClient>());
    const ConfigRun run("clobber", R"JSON({"state_dir": "/tmp/sapo-should-not-be-used"})JSON", shared);
    CHECK(run.store == std::static_pointer_cast<runtime::IStateStore>(shared));
    CHECK_FALSE(std::filesystem::exists("/tmp/sapo-should-not-be-used"));
}

TEST_CASE("engine.state_dir still upgrades the default in-memory store", "[config][statestore]") {
    const auto temporary = std::filesystem::temp_directory_path() / "sapo-cfg-target";
    std::filesystem::remove_all(temporary);
    const ConfigRun run("upgrade", json{{"state_dir", temporary.string()}}.dump());
    CHECK(std::dynamic_pointer_cast<runtime::FileStateStore>(run.store) != nullptr);
    std::filesystem::remove_all(temporary);
}

TEST_CASE("engine.state_redis is resolved, validated and reported", "[config][statestore]") {
    SECTION("a malformed URL is a startup problem, not a runtime surprise") {
        const ConfigRun run("bad-url", R"JSON({"state_redis": "redis://cache:notaport"})JSON");
        CHECK(run.anyProblemContains("engine.state_redis"));
    }

    SECTION("a non-string value is rejected") {
        const ConfigRun run("wrong-type", R"JSON({"state_redis": 6379})JSON");
        CHECK(run.anyProblemContains("must be a non-empty redis:// URL"));
    }

    SECTION("rediss:// is refused rather than silently downgraded") {
        const ConfigRun run("tls", R"JSON({"state_redis": "rediss://cache:6379"})JSON");
        CHECK(run.anyProblemContains("TLS"));
    }

    SECTION("$env indirection keeps the credential out of the config file") {
        // The `engine` block is not expanded at load time, so VirtualMachine
        // resolves it explicitly — otherwise a URL with an inline password ends
        // up in the config file and in every log line that echoes it.
        ::setenv("SAPO_TEST_REDIS_URL", "redis://127.0.0.1:1/0", 1);
        const ConfigRun run("env", R"JSON({"state_redis": {"$env": "SAPO_TEST_REDIS_URL"}})JSON");
        CHECK_FALSE(run.anyProblemContains("must be a non-empty")); // it resolved to a string
#if defined(SAPO_ENABLE_REDIS)
        // Port 1 is unreachable: the failure is reported at startup, where the
        // production checklist treats it as a deploy failure.
        CHECK(run.anyProblemContains("cannot reach"));
        CHECK(std::dynamic_pointer_cast<redis::RedisStateStore>(run.store) != nullptr);
#else
        CHECK(run.anyProblemContains("SAPO_ENABLE_REDIS=ON"));
#endif
        ::unsetenv("SAPO_TEST_REDIS_URL");
    }

    SECTION("a missing environment variable is reported") {
        ::unsetenv("SAPO_TEST_REDIS_MISSING");
        const ConfigRun run("env-missing", R"JSON({"state_redis": {"$env": "SAPO_TEST_REDIS_MISSING"}})JSON");
        CHECK(run.anyProblemContains("is not set"));
    }
}

// ===========================================================================
// opt-in: the same contract against a real server
//
// This is the half MockRedisClient cannot cover — that Redis actually accepts
// and executes the Lua. Run with:
//   SAPO_REDIS_URL=redis://127.0.0.1:6379/15 ctest -R test_redis_state_store
// Use a disposable database number: the case FLUSHDBs it.
// ===========================================================================
TEST_CASE("a live redis server accepts the scripts and enforces the CAS", "[redis][live]") {
    const char *url = std::getenv("SAPO_REDIS_URL");
    if (url == nullptr || std::string(url).empty()) {
        SKIP("set SAPO_REDIS_URL=redis://host:port/db to run against a live server");
    }
#if !defined(SAPO_ENABLE_REDIS)
    SKIP("built without -DSAPO_ENABLE_REDIS=ON, so there is no socket client to use");
#else
    std::string problem;
    const auto parsed = redis::RedisOptions::fromUrl(url, &problem);
    REQUIRE(parsed.has_value());
    auto client = std::make_shared<redis::SocketRedisClient>(*parsed);
    REQUIRE(client->healthy());
    client->command({"FLUSHDB"}).throwIfError("FLUSHDB");

    redis::RedisStateStoreOptions options;
    options.ttl_seconds = 60;
    redis::RedisStateStore store(client, options);

    // Round trip.
    store.save(makeCheckpoint("live-1", SessionStatus::AwaitingInput, "menu_amount"));
    const auto loaded = store.load("live-1");
    REQUIRE(loaded.has_value());
    CHECK(loaded->cursor == "menu_amount");
    CHECK(loaded->context["phoneNo"] == "+233242602262");
    CHECK(loaded->version == 1);

    // The Lua really ran: CAS accepts the right version and rejects the stale one.
    CHECK(store.saveIf(*loaded, 1).ok());
    const auto stale = store.saveIf(*loaded, 1);
    CHECK(stale.result == SaveResult::VersionConflict);
    CHECK(stale.version == 2);
    CHECK(store.load("live-1")->version == 2);

    // Index maintenance really ran.
    CHECK(store.count(SessionStatus::AwaitingInput) == 1);
    store.save(makeCheckpoint("live-2", SessionStatus::Completed));
    CHECK(store.count(std::nullopt) == 2);
    CHECK(store.list().size() == 2);

    // Claims really ran.
    CHECK(store.tryClaim("live-1", "exec-A"));
    CHECK_FALSE(store.tryClaim("live-1", "exec-B"));
    CHECK(store.releaseClaim("live-1", "exec-A"));

    // TTL really applied.
    const auto ttl = client->command({"TTL", "sapo:session:live-1"}).toInt();
    CHECK(ttl > 0);
    CHECK(ttl <= 60);

    // Cleanup: remove() and the index agree.
    CHECK(store.remove("live-2"));
    CHECK(store.count(SessionStatus::Completed) == 0);
    client->command({"FLUSHDB"}).throwIfError("FLUSHDB");
#endif
}
