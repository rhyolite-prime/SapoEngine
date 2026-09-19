//
//  Sapo Engine — loops, frames and resumable iteration (T2.1, T2.2, T4.1).
//
#include "test_helpers.hpp"

#include <string>

using namespace sapo;
using nlohmann::json;

TEST_CASE("loop iterates a context collection with iterator and index bound", "[loop]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "loop.collection", "version": "1.0",
      "nodes": [
        { "id": "each", "type": "loop", "collection": "$subscribers", "iterator": "row", "index": "i",
          "body": [
            { "id": "sum", "type": "noop", "assign": { "total": "${total + row.amount}", "last_i": "$i",
                                                        "last_row": "$row.msisdn" } }
          ],
          "next": "finish" },
        { "id": "finish", "type": "terminate", "status": "success", "output": { "total": "$total" } }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint, {{"total", 0},
                                                      {"subscribers",
                                                       {
                                                           {{"msisdn", "A"}, {"amount", 10}},
                                                           {{"msisdn", "B"}, {"amount", 25}},
                                                           {{"msisdn", "C"}, {"amount", 5}},
                                                       }}});
    CHECK(outcome.status == "terminated");
    CHECK(outcome.output.value("total", 0) == 40);
    CHECK(outcome.context.value("last_i", -1) == 2);
    CHECK(outcome.context.value("last_row", "") == "C");
}

TEST_CASE("empty and missing collections trip zero times", "[loop]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "loop.empty", "version": "1.0",
      "nodes": [
        { "id": "each", "type": "loop", "collection": "$rows", "body": [
            { "id": "body", "type": "noop", "assign": { "touched": true } } ],
          "next": "after" },
        { "id": "after", "type": "noop", "assign": { "after": true } }
      ]
    })JSON");
    const auto empty = vm->runBlueprint(blueprint, {{"rows", json::array()}});
    CHECK(empty.status == "completed");
    CHECK(empty.context.value("touched", false) == false);
    CHECK(empty.context.value("after", false) == true);

    const auto missing = vm->runBlueprint(blueprint, {{"rows", nullptr}});
    CHECK(missing.status == "completed");
    CHECK(missing.context.value("touched", false) == false);
}

TEST_CASE("count loops and break/continue", "[loop]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "loop.count", "version": "1.0",
      "nodes": [
        { "id": "each", "type": "loop", "count": "${limit}", "iterator": "n", "index": "i", "body": [
            { "id": "skip", "type": "continue", "when": "i % 2 == 0" },
            { "id": "add", "type": "noop", "assign": { "sum": "${sum + i}" } },
            { "id": "halt", "type": "break", "when": "sum >= 6" } ],
          "next": "done" },
        { "id": "done", "type": "terminate", "status": "success", "output": { "sum": "$sum" } }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint, {{"limit", 10}, {"sum", 0}});
    CHECK(outcome.status == "terminated");
    // odd indexes only: 1 + 3 + 5 = 9, but 1+3=4 then +5 → 9 >= 6 stops at i=5
    CHECK(outcome.output.value("sum", 0) == 9);
}

TEST_CASE("while-style loop keeps running while its guard holds", "[loop]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "loop.while", "version": "1.0",
      "nodes": [
        { "id": "each", "type": "loop", "while": "attempts < 3", "body": [
            { "id": "bump", "type": "noop", "assign": { "attempts": "${attempts + 1}" } } ] },
        { "id": "done", "type": "noop", "assign": { "final": "$attempts" } }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint, {{"attempts", 0}});
    CHECK(outcome.context.value("final", 0) == 3);
    CHECK(outcome.node_visits == 5);   // loop + 3 bodies + the final node
}

TEST_CASE("on_item_error continue skips the failing iteration", "[loop][error]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    fixture.transport->on("GET https://api.test/row",
                          [count = std::make_shared<int>(0)](const sapo::http::Request &) {
                              sapo::http::Response response;
                              ++*count;
                              if (*count == 2) {
                                  response.status_code = 500;
                                  response.body = "flaky";
                              } else {
                                  response.status_code = 200;
                                  response.body = "{\"ok\":true}";
                              }
                              return response;
                          });
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "loop.itemerror", "version": "1.0",
      "nodes": [
        { "id": "each", "type": "loop", "collection": "$ids", "iterator": "id", "on_item_error": "continue",
          "body": [
            { "id": "call", "type": "command", "command": "http.get",
              "http_request": { "url": "https://api.test/row?id=$id" } },
            { "id": "mark", "type": "noop", "assign": { "ok_count": "${ok_count + 1}" } } ],
          "next": "done" },
        { "id": "done", "type": "terminate", "status": "success", "output": { "ok": "$ok_count" } }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint, {{"ids", {1, 2, 3}}, {"ok_count", 0}});
    CHECK(outcome.status == "terminated");
    CHECK(outcome.output.value("ok", 0) == 2);   // the failing row's mark node never ran
    REQUIRE(outcome.warnings.size() == 1);
    CHECK(outcome.warnings.front().find("skipped iteration") != std::string::npos);
}

TEST_CASE("on_item_error fail aborts the session", "[loop][error]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    fixture.transport->on("GET https://api.test/row", fixture.reply(500, "nope"));
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "loop.itemfail", "version": "1.0",
      "nodes": [
        { "id": "each", "type": "loop", "collection": "$ids", "on_item_error": "fail", "body": [
            { "id": "call", "type": "command", "command": "http.get",
              "http_request": { "url": "https://api.test/row" } } ],
          "next": "done" },
        { "id": "done", "type": "terminate", "status": "success" }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint, {{"ids", {1, 2}}});
    CHECK(outcome.status == "failed");
    CHECK(outcome.error_code == "HTTP_STATUS_ERROR");
    CHECK(outcome.error_node == "call");
}

TEST_CASE("nested loops bind both iterators", "[loop][frames]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "loop.nested", "version": "1.0",
      "nodes": [
        { "id": "outer", "type": "loop", "collection": "$shops", "iterator": "shop", "body": [
            { "id": "inner", "type": "loop", "collection": "$shop.items", "iterator": "item", "body": [
                { "id": "count", "type": "noop", "assign": { "pairs": "${pairs + 1}",
                                                              "last": "$shop.name/$item.sku" } } ] } ],
          "next": "end" },
        { "id": "end", "type": "terminate", "status": "success", "output": { "pairs": "$pairs", "last": "$last" } }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint, {{"pairs", 0},
                                                      {"shops",
                                                       {
                                                           {{"name", "north"}, {"items", {{{"sku", "A1"}}, {{"sku", "B2"}},}}},
                                                           {{"name", "south"}, {"items", {{{"sku", "C3"}}}}},
                                                       }}});
    CHECK(outcome.status == "terminated");
    CHECK(outcome.output.value("pairs", 0) == 3);
    CHECK(outcome.output.value("last", "") == "south/C3");
}

TEST_CASE("a session suspended inside the third iteration resumes in place", "[loop][durable][frames]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "loop.suspend", "version": "1.0",
      "nodes": [
        { "id": "each", "type": "loop", "collection": "$rows", "iterator": "row", "index": "i", "body": [
            { "id": "ask", "type": "action", "capability": "", "await_input": true,
              "prompt_config": { "message": "Approve $row.amount for row $i?", "interaction_type": "menu",
                                 "input_validation": "matches(input, '^(yes|no)$')", "output": "answer" } },
            { "id": "tally", "type": "noop",
              "assign": { "approved": "${answer == 'yes' ? approved + 1 : approved}", "seen": "${seen + 1}" } } ],
          "next": "end" },
        { "id": "end", "type": "terminate", "status": "success", "output": { "approved": "$approved", "seen": "$seen" } }
      ]
    })JSON");
    auto outcome = vm->runBlueprint(blueprint, {{"rows", {{{"amount", 5}}, {{"amount", 10}}, {{"amount", 15}}}},
                                                {"approved", 0}, {"seen", 0}});
    CHECK(outcome.status == "awaiting_input");
    CHECK(outcome.cursor == "ask");
    const std::string session_id = outcome.session_id;
    const auto checkpoint = vm->services().state_store->load(session_id);
    REQUIRE(checkpoint.has_value());
    REQUIRE(checkpoint->frames.is_array());
    REQUIRE(checkpoint->frames.size() == 1);
    CHECK(checkpoint->frames[0]["kind"] == "loop");
    CHECK(checkpoint->frames[0]["node"] == "each");
    CHECK(checkpoint->frames[0]["state"]["index"] == 0);
    CHECK(checkpoint->cursor == "ask");

    // three prompts: yes, no, yes
    auto resume_with = [&](const std::string &answer) {
        outcome = vm->resumeSession(session_id, answer);
        return outcome;
    };
    resume_with("yes");
    CHECK(outcome.status == "awaiting_input");
    CHECK(vm->services().state_store->load(session_id)->frames[0]["state"]["index"] == 1);
    resume_with("no");
    CHECK(outcome.status == "awaiting_input");
    const auto final_outcome = resume_with("yes");
    CHECK(final_outcome.status == "terminated");
    CHECK(final_outcome.output.value("approved", 0) == 2);
    CHECK(final_outcome.output.value("seen", 0) == 3);
    CHECK(vm->services().state_store->load(session_id)->frames.empty());
}

TEST_CASE("an invalid prompt answer inside a loop is not swallowed", "[loop][validation][error]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "loop.badanswer", "version": "1.0",
      "nodes": [
        { "id": "each", "type": "loop", "count": "3", "body": [
            { "id": "ask", "type": "action", "capability": "", "await_input": true,
              "prompt_config": { "message": "pin", "interaction_type": "pin",
                                 "input_validation": "matches(input, '^[0-9]{4}$')", "output": "pin" } } ],
          "next": "end" },
        { "id": "end", "type": "terminate", "status": "success" }
      ]
    })JSON");
    const auto first = vm->runBlueprint(blueprint, {});
    CHECK(first.status == "awaiting_input");
    const auto bad = vm->resumeSession(first.session_id, "abcd");
    CHECK(bad.status == "failed");
    CHECK(bad.error_code == "VALIDATION_ERROR");
    CHECK(bad.error_node == "ask");
    CHECK(bad.error.find("pin") != std::string::npos);   // the prompt text helps the caller fix it
}

TEST_CASE("frame stack survives a checkpoint round-trip through JSON", "[frames][durable]") {
    testing::Fixture fixture;
    const auto vm = fixture.machine();
    runtime::RuntimeContext context;
    std::vector<runtime::RuntimeContext::FrameState> frames(2);
    frames[0].kind = "loop";
    frames[0].node_id = "outer";
    frames[0].position = 2;
    frames[0].resume_target = "after";
    frames[0].state = {{"index", 2}, {"items", {1, 2, 3}}, {"ids", {"a", "b"}}};
    frames[1].kind = "try";
    frames[1].node_id = "guard";
    frames[1].state = {{"phase", "catch"}, {"catch_ids", {"handler"}}};

    const json encoded = runtime::RuntimeContext::framesToJson(frames);
    const auto decoded = runtime::RuntimeContext::framesFromJson(encoded);
    REQUIRE(decoded.size() == 2);
    CHECK(decoded[0].kind == "loop");
    CHECK(decoded[0].node_id == "outer");
    CHECK(decoded[0].position == 2);
    CHECK(decoded[0].resume_target == "after");
    CHECK(decoded[0].state["items"] == json::array({1, 2, 3}));
    CHECK(decoded[1].state["phase"] == "catch");
    (void) context;
}
