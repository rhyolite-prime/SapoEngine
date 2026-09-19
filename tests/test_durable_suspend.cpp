//
//  Sapo Engine — suspensions that survive the clock and the process (T2.2, T2.4, T4.1).
//
#include "test_helpers.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace sapo;
using nlohmann::json;

TEST_CASE("a durable wait parks the session and is woken by the scheduler", "[wait][durable]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "wait.plain", "version": "1.0",
      "nodes": [
        { "id": "hold", "type": "wait", "duration": "30s", "next": "after" },
        { "id": "after", "type": "terminate", "status": "success", "output": { "woke": true } }
      ]
    })JSON");
    const auto first = vm->runBlueprint(blueprint);
    CHECK(first.status == "suspended");
    CHECK(first.cursor == "after");   // the checkpoint records where work resumes
    CHECK(vm->services().scheduler->timers().size() == 1);
    CHECK(vm->services().pool->queued() == 0);   // nothing parked on a worker thread

    // Time passes, but not enough: the session must stay parked.
    fixture.clock->advanceMs(29'999);
    CHECK(vm->tick() == 0);
    CHECK(vm->session(first.session_id)->status == "waiting");

    fixture.clock->advanceMs(2);
    CHECK(vm->tick() == 1);
    const auto resumed = vm->session(first.session_id);
    REQUIRE(resumed.has_value());
    CHECK(resumed->status == "completed");
    CHECK(resumed->output.value("woke", false) == true);
    CHECK(vm->services().metrics->counter("sapo.sessions.resumed") == 1.0);
}

TEST_CASE("a condition wait re-checks on every tick and picks up new facts", "[wait][durable]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "wait.until", "version": "1.0",
      "nodes": [
        { "id": "hold", "type": "wait", "until": "ready == true", "poll_interval": "5m", "next": "after" },
        { "id": "after", "type": "terminate", "status": "success", "output": { "seen": "$payload.id" } }
      ]
    })JSON");
    const auto first = vm->runBlueprint(blueprint, {{"ready", false}});
    CHECK(first.status == "suspended");
    CHECK(first.cursor == "hold");   // condition waits re-run themselves, they do not skip ahead
    auto stored = vm->services().state_store->load(first.session_id);
    REQUIRE(stored.has_value());
    CHECK(stored->pending["data"]["await_condition"].get<std::string>().find("ready") != std::string::npos);

    // A tick that changes nothing re-arms the same wait.
    fixture.clock->advanceMs(60'000);
    vm->tick();
    CHECK(vm->session(first.session_id)->status == "waiting");
    const size_t visits_after_retry = vm->session(first.session_id)->node_visits;
    CHECK(visits_after_retry == first.node_visits + 1);

    // …and the wake-up payload can carry the fact the workflow is waiting for.
    const auto done = vm->resumeSession(first.session_id, {{"ready", true}, {"payload", {{"id", "msg_9"}}}});
    CHECK(done.status == "terminated");
    CHECK(done.output.value("seen", "") == "msg_9");
}

TEST_CASE("a wait below the inline limit does not create a session at all", "[wait]") {
    testing::Fixture fixture;
    fixture.services.limits.inline_wait_limit_ms = 2000;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "wait.inline", "version": "1.0",
      "nodes": [
        { "id": "blink", "type": "wait", "duration": "1200ms", "durable": false, "next": "after" },
        { "id": "after", "type": "terminate", "status": "success", "output": { "done": true } }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint);
    CHECK(outcome.status == "terminated");
    REQUIRE(fixture.delays.size() == 1);
    CHECK(fixture.delays[0] == 1200);   // the host paid the delay in virtual time
    CHECK(vm->suspendedSessionCount() == 0);   // no parked session
}

TEST_CASE("a prompt suspends, resumes with the answer and validates it", "[prompt][durable]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "prompt.pin", "version": "1.0",
      "nodes": [
        { "id": "ask", "type": "action", "capability": "", "await_input": true, "input_variable": "pin",
          "prompt_config": { "message": "Enter your 4-digit PIN", "interaction_type": "pin",
                             "input_validation": "matches(input, '^[0-9]{4}$')", "timeout": "2m" },
          "next": "check" },
        { "id": "check", "type": "terminate", "status": "success", "output": { "pin": "$pin" } }
      ]
    })JSON");
    const auto first = vm->runBlueprint(blueprint);
    CHECK(first.status == "awaiting_input");
    REQUIRE(first.prompt.is_object());
    CHECK(first.prompt["interaction_type"] == "pin");
    CHECK(first.prompt["message"] == "Enter your 4-digit PIN");
    CHECK(first.cursor == "ask");   // the reply is validated by re-entering the prompt node

    const auto resumed = vm->resumeSession(first.session_id, "4720");
    CHECK(resumed.status == "terminated");
    CHECK(resumed.output.value("pin", "") == "4720");
    CHECK(vm->sessions().size() == 1);
    CHECK(vm->sessions()[0].status == "completed");
}

TEST_CASE("an unanswered prompt times out through the scheduler", "[prompt][timeout]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "prompt.timeout", "version": "1.0",
      "nodes": [
        { "id": "ask", "type": "action", "capability": "", "await_input": true,
          "prompt_config": { "message": "Confirm", "interaction_type": "menu", "timeout": "90s" },
          "on_error": "expired", "next": "go" },
        { "id": "go", "type": "terminate", "status": "success", "output": { "confirmed": "$input" } },
        { "id": "expired", "type": "terminate", "status": "failure", "output": { "note": "timed out" } }
      ]
    })JSON");
    const auto first = vm->runBlueprint(blueprint);
    CHECK(first.status == "awaiting_input");
    CHECK(vm->services().scheduler->timers().size() == 1);   // the timeout is an armed timer

    fixture.clock->advanceMs(91'000);
    CHECK(vm->tick() == 1);
    const auto snapshot = vm->session(first.session_id);
    REQUIRE(snapshot.has_value());
    // The blueprint routed the timeout to a terminate(failure) node: that is the
    // session's own verdict, and its output payload survives on the snapshot.
    CHECK(snapshot->status == "failed");
    CHECK(snapshot->output.value("note", "") == "timed out");
}

TEST_CASE("a wait for an event resumes when the event is published", "[event][durable]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "event.wait", "version": "1.0",
      "nodes": [
        { "id": "hold", "type": "action", "capability": "",
          "on_event": { "event_name": "bank.transfer.confirmed" },
          "next": "settle" },
        { "id": "settle", "type": "terminate", "status": "success",
          "output": { "reference": "$event.reference", "amount": "$event.amount" } }
      ]
    })JSON");
    const auto first = vm->runBlueprint(blueprint);
    INFO(first.error << " [" << first.error_code << "]");
    CHECK(first.status == "suspended");
    CHECK(vm->services().events->subscriberCount() >= 1);

    vm->publishEvent("bank.transfer.confirmed", {{"reference", "REF-77"}, {"amount", 1500}});
    const auto snapshot = vm->session(first.session_id);
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->status == "completed");
    CHECK(snapshot->output.value("reference", "") == "REF-77");
    CHECK(snapshot->output.value("amount", 0) == 1500);
}

TEST_CASE("a cancelled session ignores its pending timer and refuses resume", "[durable]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "wait.cancel", "version": "1.0",
      "nodes": [
        { "id": "hold", "type": "wait", "duration": "1h", "next": "after" },
        { "id": "after", "type": "noop", "assign": { "ran": true } }
      ]
    })JSON");
    const auto first = vm->runBlueprint(blueprint);
    REQUIRE(first.status == "suspended");
    CHECK(vm->cancelSession(first.session_id, "user closed the app") == true);
    CHECK(vm->services().scheduler->timers().empty());

    fixture.clock->advanceMs(3'600'000);
    CHECK(vm->tick() == 0);   // the wake-up is gone, so nothing runs

    const auto resumed = vm->resumeSession(first.session_id, "late");
    CHECK(resumed.status == "failed");
    CHECK(resumed.error_code == "SESSION_NOT_SUSPENDED");
    CHECK(vm->session(first.session_id)->status == "cancelled");
    CHECK(vm->session(first.session_id)->context.value("ran", false) == false);
    CHECK(vm->cancelSession(first.session_id) == false);   // nothing left to cancel
}

TEST_CASE("resume rejects unknown and finished sessions without crashing", "[durable]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "wait.short", "version": "1.0",
      "nodes": [
        { "id": "hold", "type": "wait", "duration": "10s", "next": "after" },
        { "id": "after", "type": "terminate", "status": "success" }
      ]
    })JSON");
    const auto unknown = vm->resumeSession("00000000-0000-4000-8000-000000000000", "x");
    CHECK(unknown.status == "failed");
    CHECK(unknown.error_code == "SESSION_NOT_FOUND");

    const auto first = vm->runBlueprint(blueprint);
    fixture.clock->advanceMs(10'001);
    vm->tick();
    const auto again = vm->resumeSession(first.session_id, "late answer");
    CHECK(again.status == "failed");
    CHECK(again.error_code == "SESSION_NOT_SUSPENDED");
}

TEST_CASE("the checkpoint holds exactly what a restart needs", "[durable]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "wait.snapshot", "version": "1.0",
      "nodes": [
        { "id": "loop", "type": "loop", "collection": "$rows", "iterator": "row", "body": [
            { "id": "ask", "type": "action", "capability": "", "await_input": true, "input_variable": "answer",
              "prompt_config": { "message": "Approve row $row?", "interaction_type": "menu" } } ],
          "next": "end" },
        { "id": "end", "type": "terminate", "status": "success" }
      ]
    })JSON");
    const auto first = vm->runBlueprint(blueprint, {{"rows", {1, 2, 3}}, {"account_id", "acc_42"}});
    REQUIRE(first.status == "awaiting_input");
    const auto stored = vm->services().state_store->load(first.session_id);
    REQUIRE(stored.has_value());
    CHECK(stored->blueprint_id == "wait.snapshot");
    CHECK(stored->status == runtime::SessionStatus::AwaitingInput);
    CHECK(stored->cursor == "ask");
    CHECK(stored->frames.size() == 1);
    CHECK(stored->frames[0]["kind"] == "loop");
    CHECK(stored->pending["input_variable"] == "answer");
    CHECK(stored->pending["prompt"]["message"] == "Approve row 1?");
    CHECK(stored->context["account_id"] == "acc_42");
    CHECK(stored->node_visits > 0);
    CHECK(stored->updated_ms >= stored->created_ms);

    const json round_trip = stored->toJson();
    const auto restored = runtime::SessionCheckpoint::fromJson(round_trip);
    CHECK(restored.session_id == stored->session_id);
    CHECK(restored.frames == stored->frames);
    CHECK(restored.pending == stored->pending);
}

TEST_CASE("a session survives a full engine restart through a file store", "[durable][restart]") {
    const auto directory = std::filesystem::temp_directory_path() / ("sapo-restart-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    testing::Fixture fixture;
    fixture.services.state_store = std::make_shared<runtime::FileStateStore>(directory.string());
    auto first_process = fixture.machine();

    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "wait.restart", "version": "1.0",
      "nodes": [
        { "id": "ask", "type": "action", "capability": "", "await_input": true, "input_variable": "otp",
          "prompt_config": { "message": "Enter the OTP sent to $msisdn", "interaction_type": "input",
                             "input_validation": "matches(input, '^[0-9]{6}$')", "timeout": "10m" },
          "next": "verify" },
        { "id": "verify", "type": "noop", "assign": { "verified": "${otp == '123456'}" }, "next": "done" },
        { "id": "done", "type": "terminate", "status": "success", "output": { "verified": "$verified",
                                                                               "msisdn": "$msisdn" } }
      ]
    })JSON");
    const auto started = first_process->runBlueprint(blueprint, {{"msisdn", "233201234567"}});
    REQUIRE(started.status == "awaiting_input");
    REQUIRE(std::filesystem::exists(directory / (started.session_id + ".json")));

    // The first process dies; only the bytes on disk remain.
    first_process->stop();
    first_process.reset();

    testing::Fixture second;
    second.services.state_store = std::make_shared<runtime::FileStateStore>(directory.string());
    auto second_process = second.machine();
    second_process->addBlueprintText(blueprint.dump());   // the deployment re-registers the same blueprint

    const auto pending = second_process->sessions();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].session_id == started.session_id);
    CHECK(pending[0].blueprint_id == "wait.restart");

    const auto resumed = second_process->resumeSession(started.session_id, "123456");
    CHECK(resumed.status == "terminated");
    CHECK(resumed.output.value("verified", false) == true);
    CHECK(resumed.output.value("msisdn", "") == "233201234567");

    second_process->stop();
    std::filesystem::remove_all(directory);
}

TEST_CASE("dangling waits are swept and reported as expired", "[durable][limits]") {
    testing::Fixture fixture;
    fixture.services.limits.default_timeout_ms = std::optional<int64_t>(60'000);
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "wait.expire", "version": "1.0",
      "nodes": [ { "id": "hold", "type": "action", "capability": "", "await_input": true,
                   "prompt_config": { "message": "Confirm", "interaction_type": "menu" } } ]
    })JSON");
    const auto first = vm->runBlueprint(blueprint);
    INFO(first.error << " [" << first.error_code << "]");
    REQUIRE(first.status == "awaiting_input");
    fixture.clock->advanceMs(61'000);
    CHECK(vm->tick() == 1);
    const auto snapshot = vm->session(first.session_id);
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->status == "failed");
    CHECK(snapshot->error.find("TIMEOUT") != std::string::npos);
}
