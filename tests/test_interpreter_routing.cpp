//
//  Sapo Engine — interpreter routing tests (T1.5, M1 golden suite).
//
#include "test_helpers.hpp"

#include <string>

using namespace sapo;
using nlohmann::json;

TEST_CASE("linear flow follows materialised next edges", "[interpreter][routing]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "routing.linear", "version": "1.0",
      "nodes": [
        { "id": "a", "type": "noop", "assign": { "a": 1 }, "next": "b" },
        { "id": "b", "type": "noop", "assign": { "b": "${a + 1}" }, "next": "c" },
        { "id": "c", "type": "noop", "assign": { "c": "${b * 10}" } }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint);
    CHECK(outcome.status == "completed");
    CHECK(outcome.node_visits == 3);
    CHECK(outcome.context.value("a", 0) == 1);
    CHECK(outcome.context.value("b", 0) == 2);
    CHECK(outcome.context.value("c", 0) == 20);
}

TEST_CASE("a node with no successor ends the session", "[interpreter][routing]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "routing.end", "version": "1.0",
      "nodes": [ { "id": "only", "type": "noop", "assign": { "ran": true } } ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint);
    CHECK(outcome.status == "completed");
    CHECK(outcome.context.value("ran", false) == true);
}

TEST_CASE("if/then/else: body form and jump form, both directions", "[interpreter][condition]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "routing.if", "version": "1.0",
      "vars": { "amount": 0 },
      "nodes": [
        { "id": "gate", "type": "if", "condition": "$amount > 100",
          "then": [ { "id": "big", "type": "noop", "assign": { "tier": "gold" } } ],
          "else": [ { "id": "small", "type": "noop", "assign": { "tier": "silver" } } ],
          "next": "tail" },
        { "id": "tail", "type": "noop", "assign": { "tail": "${1 + 1}" }, "next": "fin" },
        { "id": "fin", "type": "terminate", "status": "success", "output": { "tier": "$tier" } }
      ]
    })JSON");

    const auto low = vm->runBlueprint(blueprint, {{"amount", 50}});
    CHECK(low.status == "terminated");
    CHECK(low.context.value("tier", "") == "silver");
    CHECK(low.output.value("tier", "") == "silver");

    const auto high = vm->runBlueprint(blueprint, {{"amount", 150}});
    CHECK(high.context.value("tier", "") == "gold");
    CHECK(high.context.value("tail", 0) == 2);
}

TEST_CASE("if without a taken body jumps to its target", "[interpreter][condition]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "routing.ifjump", "version": "1.0",
      "nodes": [
        { "id": "gate", "type": "if", "condition": "$ok == true", "then": "yes", "else": "no" },
        { "id": "yes", "type": "noop", "assign": { "where": "yes" }, "next": "end" },
        { "id": "no", "type": "noop", "assign": { "where": "no" }, "next": "end" },
        { "id": "end", "type": "terminate", "status": "success" }
      ]
    })JSON");
    const auto yes = vm->runBlueprint(blueprint, {{"ok", true}});
    CHECK(yes.context.value("where", "") == "yes");
    const auto no = vm->runBlueprint(blueprint, {{"ok", false}});
    CHECK(no.context.value("where", "") == "no");
}

TEST_CASE("choice selects a case or the default", "[interpreter][choice]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "routing.choice", "version": "1.0",
      "nodes": [
        { "id": "pick", "type": "choice", "expression": "$status",
          "cases": { "approved": "yes", "pending": "hold" }, "default": "no" },
        { "id": "yes", "type": "noop", "assign": { "took": "approved" }, "next": "end" },
        { "id": "hold", "type": "noop", "assign": { "took": "pending" }, "next": "end" },
        { "id": "no", "type": "noop", "assign": { "took": "default" }, "next": "end" },
        { "id": "end", "type": "terminate", "status": "success" }
      ]
    })JSON");
    CHECK(vm->runBlueprint(blueprint, {{"status", "approved"}}).context.value("took", "") == "approved");
    CHECK(vm->runBlueprint(blueprint, {{"status", "pending"}}).context.value("took", "") == "pending");
    CHECK(vm->runBlueprint(blueprint, {{"status", "mystery"}}).context.value("took", "") == "default");
    // numeric and string spellings of the same key must match (blueprints lie)
    const auto numeric = testing::objectOrThrow(R"JSON({
      "name": "routing.choice.num", "version": "1.0",
      "nodes": [
        { "id": "pick", "type": "choice", "expression": "$code", "cases": { "1": "one", "2": "two" } },
        { "id": "one", "type": "noop", "assign": { "took": 1 }, "next": "end" },
        { "id": "two", "type": "noop", "assign": { "took": 2 }, "next": "end" },
        { "id": "end", "type": "terminate", "status": "success" }
      ]
    })JSON");
    CHECK(vm->runBlueprint(numeric, {{"code", 2}}).context.value("took", 0) == 2);
}

TEST_CASE("choice with no case and no default fails with ROUTING_ERROR", "[interpreter][choice]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "routing.choice.nomatch", "version": "1.0",
      "nodes": [
        { "id": "pick", "type": "choice", "expression": "$status", "cases": { "approved": "yes" } },
        { "id": "yes", "type": "terminate", "status": "success" }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint, {{"status", "other"}});
    CHECK(outcome.status == "failed");
    CHECK(outcome.error_code == "ROUTING_ERROR");
    CAPTURE(outcome.error);
    CHECK(outcome.error.find("approved") != std::string::npos);   // the message lists what was available
    REQUIRE(outcome.error_data.contains("cases"));
    CHECK(outcome.error_data["cases"] == json::array({"approved"}));
}

TEST_CASE("terminate carries its payload and status", "[interpreter][terminate]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "routing.terminate", "version": "1.0",
      "nodes": [
        { "id": "a", "type": "noop", "assign": { "reason": "dupe" }, "next": "stop" },
        { "id": "stop", "type": "terminate", "status": "failed", "error_code": "ALREADY_MEMBER",
          "message": "duplicate msisdn: $reason", "output": { "reason": "$reason" } }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint);
    CHECK(outcome.status == "failed");
    CHECK(outcome.error_code == "ALREADY_MEMBER");
    CHECK(outcome.error == "duplicate msisdn: dupe");
    CHECK(outcome.node_visits == 2);
}

TEST_CASE("disabled nodes are skipped without executing", "[interpreter][routing]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "routing.disabled", "version": "1.0",
      "nodes": [
        { "id": "a", "type": "noop", "assign": { "a": 1 }, "next": "skipped" },
        { "id": "skipped", "type": "noop", "enabled": false, "assign": { "boom": "${undefined_var}" }, "next": "b" },
        { "id": "b", "type": "noop", "assign": { "b": 2 } }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint);
    CHECK(outcome.status == "completed");
    CHECK(outcome.context.find("boom") == outcome.context.end());
    CHECK(outcome.context.value("b", 0) == 2);
}

TEST_CASE("a jump out of a loop body abandons the loop", "[interpreter][routing][jump]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "routing.escape", "version": "1.0",
      "nodes": [
        { "id": "loop", "type": "loop", "count": "10", "body": [
            { "id": "tick", "type": "noop", "assign": { "n": "${n + 1}" } },
            { "id": "leave", "type": "noop", "assign": { "extra": true }, "next": "outside" } ],
          "next": "after" },
        { "id": "after", "type": "noop", "assign": { "landed": true } },
        { "id": "outside", "type": "noop", "assign": { "escaped": true } }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint, {{"n", 0}});
    CHECK(outcome.status == "completed");
    CHECK(outcome.context.value("n", 0) == 1);
    CHECK(outcome.context.value("escaped", false) == true);
    CHECK(outcome.context.value("landed", false) == false);   // jumping out skipped the loop's own successor
    // the frame stack must be empty again once the session is over
    const auto checkpoint = vm->services().state_store->load(outcome.session_id);
    REQUIRE(checkpoint.has_value());
    CHECK(checkpoint->frames.empty());
}

TEST_CASE("execution budget stops a runaway workflow instead of hanging", "[interpreter][limits]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "routing.runaway", "version": "1.0",
      "nodes": [
        { "id": "loop", "type": "loop", "count": "100000", "body": [
            { "id": "spin", "type": "noop", "assign": { "n": "${n + 1}" } } ] }
      ]
    })JSON");
    runtime::StartSessionOptions options;
    options.max_node_visits = 50;
    const auto outcome = vm->runBlueprint(blueprint, {{"n", 0}}, options);
    CHECK(outcome.status == "failed");
    CHECK(outcome.error_code == "LIMIT_EXCEEDED");
    CAPTURE(outcome.error);
    CHECK(outcome.error.find("budget") != std::string::npos);
    CHECK(outcome.node_visits <= 51);
}

TEST_CASE("loop max_iterations caps a condition-driven loop", "[interpreter][limits][loop]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "routing.while", "version": "1.0",
      "nodes": [
        { "id": "loop", "type": "loop", "while": "n < 1000", "max_iterations": 7, "body": [
            { "id": "step", "type": "noop", "assign": { "n": "${n + 1}" } } ] },
        { "id": "done", "type": "noop", "assign": { "finished": true } }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint, {{"n", 0}});
    CHECK(outcome.status == "completed");
    CHECK(outcome.context.value("n", 0) == 7);
    CHECK(outcome.context.value("finished", false) == true);
    CHECK_FALSE(outcome.warnings.empty());
    CAPTURE(outcome.warnings);
}

TEST_CASE("session and execution ids are stable and reported", "[interpreter][ids]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "routing.ids", "version": "1.0",
      "nodes": [ { "id": "check", "type": "noop", "assign": { "seen_id": "$session.id" } } ]
    })JSON");
    runtime::StartSessionOptions options;
    options.session_id = "sess-fixed";
    options.correlation_id = "corr-1";
    const auto outcome = vm->runBlueprint(blueprint, {}, options);
    CHECK(outcome.session_id == "sess-fixed");
    CHECK(outcome.context.value("seen_id", "") == "sess-fixed");
    CHECK_FALSE(outcome.execution_id.empty());
}
