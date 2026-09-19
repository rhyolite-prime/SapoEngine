//
//  Sapo Engine — the error model: retry, on_error, try/catch/finally (T2.6).
//
#include "test_helpers.hpp"

#include <string>

using namespace sapo;
using nlohmann::json;

TEST_CASE("retry applies exponential backoff and succeeds on the third attempt", "[error][retry]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    fixture.transport->failThenSucceed("POST https://api.test/charge", 2, fixture.reply(200, R"({"id":"ch_1"})"));
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "error.retry", "version": "1.0",
      "nodes": [
        { "id": "charge", "type": "command", "command": "http.post",
          "http_request": { "url": "https://api.test/charge", "body": { "amount": 10 } },
          "retry": { "max_attempts": 3, "backoff_ms": 100, "multiplier": 2, "jitter": 0 },
          "output": { "charge_id": "$.body.id" }, "next": "done" },
        { "id": "done", "type": "terminate", "status": "success", "output": { "charge_id": "$charge_id" } }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint);
    CHECK(outcome.status == "terminated");
    CHECK(outcome.output.value("charge_id", "") == "ch_1");
    CHECK(fixture.transport->callCount() == 3);
    REQUIRE(fixture.delays.size() == 2);
    CHECK(fixture.delays[0] == 100);
    CHECK(fixture.delays[1] == 200);
    CHECK(vm->services().metrics->counter("sapo.nodes.retries") == 2.0);
}

TEST_CASE("jitter stretches the delay but never shrinks it", "[error][retry]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    fixture.transport->on("GET https://api.test/flaky", fixture.reply(503, "busy"));
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "error.jitter", "version": "1.0",
      "nodes": [ { "id": "get", "type": "command", "command": "http.get",
                   "http_request": { "url": "https://api.test/flaky" },
                   "retry": { "max_attempts": 4, "backoff_ms": 50, "multiplier": 1, "jitter": 0.5 } } ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint);
    CHECK(outcome.status == "failed");
    CHECK(fixture.transport->callCount() == 4);
    REQUIRE(fixture.delays.size() == 3);
    for (const auto delay : fixture.delays) {
        CHECK(delay >= 50);
        CHECK(delay <= 75);
    }
}

TEST_CASE("a 4xx is not retried unless the blueprint asks for it", "[error][retry]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    fixture.transport->on("POST https://api.test/kyc", fixture.reply(422, R"({"error":"bad id"})"));
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "error.noretry", "version": "1.0",
      "nodes": [ { "id": "kyc", "type": "command", "command": "http.post",
                   "http_request": { "url": "https://api.test/kyc", "body": { "id": "x" } },
                   "retry": { "max_attempts": 5, "backoff_ms": 10 } } ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint);
    CHECK(outcome.status == "failed");
    CHECK(outcome.error_code == "HTTP_STATUS_ERROR");
    CHECK(fixture.transport->callCount() == 1);   // 422 is deterministic: retrying is waste

    // …but an explicit retry_on list wins
    fixture.transport->clear();
    fixture.transport->on("POST https://api.test/kyc",
                           [count = std::make_shared<int>(0)](const sapo::http::Request &) {
                               sapo::http::Response response;
                               ++*count;
                               if (*count == 1) {
                                   response.status_code = 422;
                                   response.body = R"({"error":"bad id"})";
                               } else {
                                   response.status_code = 200;
                                   response.body = R"({"ok":true})";
                               }
                               return response;
                           });
    const auto wants_retry = testing::objectOrThrow(R"JSON({
      "name": "error.retryon", "version": "1.0",
      "nodes": [ { "id": "kyc", "type": "command", "command": "http.post",
                   "http_request": { "url": "https://api.test/kyc" },
                   "retry": { "max_attempts": 3, "backoff_ms": 10, "retry_on": ["422"] } },
                 { "id": "after", "type": "terminate", "status": "success" } ]
    })JSON");
    const auto retried = vm->runBlueprint(wants_retry);
    CHECK(retried.status == "terminated");
    CHECK(fixture.transport->callCount() == 2);
}

TEST_CASE("retry delays are capped by the engine limits", "[error][retry][limits]") {
    testing::Fixture fixture;
    fixture.services.limits.max_retry_delay_ms = 250;
    auto vm = fixture.machine();
    fixture.transport->on("GET https://api.test/slow", fixture.reply(500, "boom"));
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "error.cap", "version": "1.0",
      "nodes": [ { "id": "get", "type": "command", "command": "http.get",
                   "http_request": { "url": "https://api.test/slow" },
                   "retry": { "max_attempts": 4, "backoff_ms": 1000, "multiplier": 2, "jitter": 0 } } ]
    })JSON");
    vm->runBlueprint(blueprint);
    REQUIRE(fixture.delays.size() == 3);
    for (const auto delay : fixture.delays) CHECK(delay <= 250);
}

TEST_CASE("a non-2xx response reaches on_error with the response captured", "[error][on_error]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    fixture.transport->on("GET https://api.test/user", fixture.reply(404, R"({"detail":"not found"})"));
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "error.onerror", "version": "1.0",
      "nodes": [
        { "id": "load", "type": "command", "command": "http.get", "http_request": { "url": "https://api.test/user" },
          "on_error": "fallback", "next": "use" },
        { "id": "use", "type": "noop", "assign": { "name": "$load.body.name" }, "next": "end" },
        { "id": "fallback", "type": "noop", "assign": { "name": "anonymous", "why": "$error.message",
                                                          "code": "$error.code", "status": "$error.data.status" },
          "next": "end" },
        { "id": "end", "type": "terminate", "status": "success",
          "output": { "name": "$name", "code": "$code", "status": "$status" } }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint);
    CHECK(outcome.status == "terminated");
    CHECK(outcome.output.value("name", "") == "anonymous");
    CHECK(outcome.output.value("code", "") == "HTTP_STATUS_ERROR");
    CHECK(outcome.output.value("status", 0) == 404);
    CHECK(outcome.context.value("why", "").find("404") != std::string::npos);
    CHECK(outcome.output.find("use") == outcome.output.end());
}

TEST_CASE("an unhandled error fails the session and is recorded on the checkpoint", "[error]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    fixture.transport->on("GET https://api.test/down", fixture.reply(0, "", "connection refused"));
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "error.unhandled", "version": "1.0",
      "nodes": [ { "id": "call", "type": "command", "command": "http.get",
                   "http_request": { "url": "https://api.test/down" } } ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint);
    CHECK(outcome.status == "failed");
    CHECK(outcome.error_code == "HTTP_ERROR");
    CHECK(outcome.error_node == "call");
    CHECK(outcome.ok == false);
    const auto checkpoint = vm->services().state_store->load(outcome.session_id);
    REQUIRE(checkpoint.has_value());
    CHECK(checkpoint->status == runtime::SessionStatus::Failed);
    CHECK(checkpoint->error.find("HTTP_ERROR") != std::string::npos);
    CHECK(vm->services().metrics->counter("sapo.nodes.failed") == 1.0);
}

TEST_CASE("try/catch captures $error and finally always runs", "[error][try]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    fixture.transport->on("POST https://api.test/debit", fixture.reply(502, "gateway down"));
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "error.try", "version": "1.0",
      "nodes": [
        { "id": "block", "type": "try",
          "body": [ { "id": "debit", "type": "command", "command": "http.post",
                       "http_request": { "url": "https://api.test/debit" }, "next": "never" },
                    { "id": "never", "type": "noop", "assign": { "should_not_run": true } } ],
          "catch": { "as": "failure", "body": [
            { "id": "refund_note", "type": "noop", "assign": { "outcome": "refunded",
                                                                "captured": "$failure.code",
                                                                "at_node": "$failure.node" } } ] },
          "finally": [ { "id": "audit", "type": "noop", "assign": { "audited": true } } ],
          "next": "end" },
        { "id": "end", "type": "terminate", "status": "success", "output": { "outcome": "$outcome" } }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint);
    CHECK(outcome.status == "terminated");
    CHECK(outcome.output.value("outcome", "") == "refunded");
    CHECK(outcome.context.value("captured", "") == "HTTP_STATUS_ERROR");
    CHECK(outcome.context.value("at_node", "") == "debit");
    CHECK(outcome.context.value("audited", false) == true);
    CHECK(outcome.context.find("should_not_run") == outcome.context.end());
}

TEST_CASE("finally runs on the happy path too, and no error is re-raised", "[error][try]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    fixture.transport->on("POST https://api.test/debit", fixture.reply(200, R"({"ok":true})"));
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "error.tryok", "version": "1.0",
      "nodes": [
        { "id": "block", "type": "try",
          "body": [ { "id": "debit", "type": "command", "command": "http.post",
                       "http_request": { "url": "https://api.test/debit" } } ],
          "catch": { "body": [ { "id": "ohno", "type": "noop", "assign": { "handled": true } } ] },
          "finally": [ { "id": "audit", "type": "noop", "assign": { "audited": true } } ],
          "next": "end" },
        { "id": "end", "type": "terminate", "status": "success", "output": { "audited": "$audited" } }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint);
    CHECK(outcome.status == "terminated");
    CHECK(outcome.output.value("audited", false) == true);
    CHECK(outcome.context.find("handled") == outcome.context.end());   // catch body skipped
}

TEST_CASE("a catch without a handler still runs cleanup, then propagates", "[error][try]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    fixture.provider->reply("test.explode", json{{"__fail", "CAPABILITY_ERROR"}, {"message", "kaboom"}});
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "error.rethrow", "version": "1.0",
      "nodes": [
        { "id": "block", "type": "try",
          "body": [ { "id": "boom", "type": "action", "capability": "test.explode" } ],
          "finally": [ { "id": "cleanup", "type": "noop", "assign": { "cleaned": true } } ],
          "next": "end" },
        { "id": "end", "type": "terminate", "status": "success" }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint);
    CHECK(outcome.status == "failed");
    CHECK(outcome.error_code == "CAPABILITY_ERROR");
    CHECK(outcome.error.find("kaboom") != std::string::npos);
    CHECK(outcome.context.value("cleaned", false) == true);   // cleanup ran before the failure
}

TEST_CASE("catch_when routes by error class", "[error][try]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    fixture.transport->on("GET https://api.test/gone", fixture.reply(410, "gone"));
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "error.catchwhen", "version": "1.0",
      "nodes": [
        { "id": "block", "type": "try",
          "body": [ { "id": "fetch", "type": "command", "command": "http.get",
                       "http_request": { "url": "https://api.test/gone" } } ],
          "catch": { "when": { "HTTP_STATUS_ERROR": "on_http" }, "body": [
            { "id": "generic", "type": "noop", "assign": { "route": "generic" } } ] },
          "next": "end" },
        { "id": "on_http", "type": "noop", "assign": { "route": "http" }, "next": "end" },
        { "id": "end", "type": "terminate", "status": "success", "output": { "route": "$route" } }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint);
    CHECK(outcome.status == "terminated");
    CHECK(outcome.output.value("route", "") == "http");
}

TEST_CASE("the innermost handler wins in nested try blocks", "[error][try]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    fixture.provider->reply("test.fail", json{{"__fail", "NOT_IMPLEMENTED"}, {"message", "no provider"}});
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "error.nested", "version": "1.0",
      "nodes": [
        { "id": "outer", "type": "try",
          "body": [ { "id": "inner", "type": "try",
            "body": [ { "id": "call", "type": "action", "capability": "test.fail" } ],
            "catch": [ { "id": "inner_handled", "type": "noop", "assign": { "handled_by": "inner" } } ] } ],
          "catch": [ { "id": "outer_handled", "type": "noop", "assign": { "handled_by": "outer" } } ] },
        { "id": "end", "type": "terminate", "status": "success", "output": { "handled_by": "$handled_by" } }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint);
    CHECK(outcome.status == "terminated");
    CHECK(outcome.output.value("handled_by", "") == "inner");
}

TEST_CASE("errors thrown by unknown node fields are reported per node", "[error][parse]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    fixture.transport->on("PATCH https://api.test/x", fixture.reply(500, "bad"));
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "error.trace", "version": "1.0",
      "nodes": [ { "id": "patch", "type": "command", "command": "http.patch",
                   "http_request": { "url": "https://api.test/x" } } ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint);
    CHECK(outcome.status == "failed");
    const auto spans = vm->traceFor(outcome.execution_id);
    REQUIRE(spans.size() >= 1);
    CHECK(spans[0]["node_id"] == "patch");
    CHECK(spans[0]["outcome"] == "failed");
    CHECK(spans[0]["error_code"] == "HTTP_STATUS_ERROR");
}
