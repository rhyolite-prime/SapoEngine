//
//  Sapo Engine — data shaping: transform, query and script nodes (T1.4, T2.7).
//
#include "test_helpers.hpp"

#include "config/ProviderConfig.hpp"

#include <string>

using namespace sapo;
using nlohmann::json;

namespace {

/// Runs a blueprint whose only node is `node`, and returns the resulting context.
json runTransform(const json &node, const json &input = json::object()) {
    json blueprint{{"name", "transform.probe"},
                   {"version", "1.0"},
                   {"nodes", json::array({node})}};
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto outcome = vm->runBlueprint(blueprint, input);
    INFO(outcome.error << " [" << outcome.error_code << "]");
    REQUIRE((outcome.status == "completed" || outcome.status == "terminated"));
    return outcome.context;
}

} // namespace

TEST_CASE("transform map projects each row through the mapping", "[transform]") {
    const auto context = runTransform({{"id", "shape"},
                                            {"type", "transform"},
                                            {"op", "map"},
                                            {"input", "$rows"},
                                            {"mapping", {{"who", "$item.name"}, {"gross", "${item.amount * 100}"}}},
                                            {"output", "shaped"}},
                                    {{"rows", {{{"name", "ama"}, {"amount", 2}}, {{"name", "kojo"}, {"amount", 5}}}}});
    const auto &shaped = context.at("shaped");
    REQUIRE(shaped.is_array());
    REQUIRE(shaped.size() == 2);
    CHECK(shaped[0]["who"] == "ama");
    CHECK(shaped[0]["gross"] == 200);
    CHECK(shaped[1]["gross"] == 500);
}

TEST_CASE("transform filter and the renamable item variable", "[transform]") {
    const auto context = runTransform({{"id", "shape"},
                                            {"type", "transform"},
                                            {"op", "filter"},
                                            {"input", "$orders"},
                                            {"where", "order.amount >= 100"},
                                            {"item_variable", "order"},
                                            {"output", "big"}},
                                      {{"orders", {{{"amount", 10}}, {{"amount", 250}}, {{"amount", 99}}}}});
    REQUIRE(context.at("big").size() == 1);
    CHECK(context.at("big")[0]["amount"] == 250);
}

TEST_CASE("transform reduce, group, sort and flatten", "[transform]") {
    SECTION("reduce sums into an accumulator") {
        const auto context = runTransform({{"id", "shape"},
                                                {"type", "transform"},
                                                {"op", "reduce"},
                                                {"input", "$rows"},
                                                {"predicate", "acc + item.amount"},
                                                {"initial", 0},
                                                {"output", "total"}},
                                          {{"rows", {{{"amount", 3}}, {{"amount", 4}}}}});
        CHECK(context.at("total") == 7);
    }
    SECTION("group buckets by a key expression") {
        const auto context = runTransform({{"id", "shape"},
                                                {"type", "transform"},
                                                {"op", "group"},
                                                {"input", "$rows"},
                                                {"mapping", {{"key", "$item.status"}}},
                                                {"output", "buckets"}},
                                          {{"rows", {{{"status", "ok"}}, {{"status", "late"}}, {{"status", "ok"}}}}});
        const auto &buckets = context.at("buckets");
        CHECK(buckets.size() == 2);
        if (buckets.is_object()) {
            REQUIRE(buckets.contains("ok"));
            CHECK(buckets["ok"].size() == 2);
        }
    }
    SECTION("sort orders by an expression") {
        const auto context = runTransform({{"id", "shape"},
                                                {"type", "transform"},
                                                {"op", "sort"},
                                                {"input", "$rows"},
                                                {"predicate", "item.amount"},
                                                {"output", "ordered"}},
                                          {{"rows", {{{"amount", 9}}, {{"amount", 2}}, {{"amount", 7}}}}});
        const auto &ordered = context.at("ordered");
        REQUIRE(ordered.size() == 3);
        CHECK(ordered.front()["amount"] == 2);
        CHECK(ordered.back()["amount"] == 9);
    }
    SECTION("flatten one level") {
        const auto context = runTransform({{"id", "shape"},
                                                {"type", "transform"},
                                                {"op", "flatten"},
                                                {"input", "$nested"},
                                                {"output", "flat"}},
                                          {{"nested", {{{1, 2}, {3}}, {4}}}});
        CHECK(context.at("flat").size() == 4);
    }
}

TEST_CASE("transform assign/copy/set write the shaped value into the context", "[transform]") {
    const auto copied = runTransform({{"id", "shape"},
                                          {"type", "transform"},
                                          {"op", "copy"},
                                          {"input", "$source"},
                                          {"output", "copy"}},
                                     {{"source", {{"a", 1}}}});
    CHECK(copied.at("copy") == json{{"a", 1}});

    const auto merged = runTransform({{"id", "shape"},
                                          {"type", "transform"},
                                          {"op", "merge"},
                                          {"input", "$parts"},
                                          {"output", "whole"}},
                                     {{"parts", {{{"a", 1}}, {{"b", 2}}}}});
    CHECK(merged.at("whole") == json{{"a", 1}, {"b", 2}});
}

TEST_CASE("an unknown transform operation is a parse error, not a no-op", "[transform][parse]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const json blueprint = testing::objectOrThrow(R"JSON({
      "name": "transform.bad", "version": "1.0",
      "nodes": [ { "id": "shape", "type": "transform", "op": "pivot", "input": "$rows", "output": "x" } ]
    })JSON");
    CHECK_THROWS_AS(vm->addBlueprintText(blueprint.dump()), sapo::runtime::SapoError);
}

TEST_CASE("a declared context data source is read by a query node", "[data][query]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "query.context", "version": "1.0",
      "nodes": [
        { "id": "declare", "type": "action", "capability": "", "data_sources": [
            { "name": "balances", "provider": "context", "config": { "source": "$accounts" } }
          ] },
        { "id": "read", "type": "query", "source": "balances", "output": "rows", "next": "count" },
        { "id": "count", "type": "noop", "assign": { "how_many": "${count(rows)}" }, "next": "end" },
        { "id": "end", "type": "terminate", "status": "success", "output": { "how_many": "$how_many" } }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint, {{"accounts", {{{"id", "A"}}, {{"id", "B"}}, {{"id", "C"}}}}});
    INFO(outcome.error << " [" << outcome.error_code << "]");
    CHECK(outcome.status == "terminated");
    CHECK(outcome.output.value("how_many", 0) == 3);
}

TEST_CASE("query filters rows with a SEL predicate", "[data][query]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "query.filter", "version": "1.0",
      "nodes": [
        { "id": "read", "type": "query", "source": "orders", "filter": "amount >= 100", "output": "big",
          "next": "end" },
        { "id": "end", "type": "terminate", "status": "success", "output": { "n": "${count(big)}" } }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint,
                                          {{"orders", {{{"amount", 20}}, {{"amount", 400}}, {{"amount", 150}}}}});
    INFO(outcome.error << " [" << outcome.error_code << "]");
    CHECK(outcome.status == "terminated");
    CHECK(outcome.output.value("n", 0) == 2);
}

TEST_CASE("mock provider serves the rows declared in config", "[data][mock]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "query.mock", "version": "1.0",
      "nodes": [
        { "id": "declare", "type": "action", "capability": "", "data_sources": [
            { "name": "tariffs", "provider": "mock",
              "config": { "rows": [{ "fee": 0.05, "currency": "GHS" }] } }
          ] },
        { "id": "read", "type": "query", "source": "tariffs", "output": "tariff", "next": "compute" },
        { "id": "compute", "type": "noop", "assign": { "fee": "${tariff[0].fee}", "ccy": "${tariff[0].currency}" },
          "next": "end" },
        { "id": "end", "type": "terminate", "status": "success", "output": { "fee": "$fee", "ccy": "$ccy" } }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint);
    INFO(outcome.error << " [" << outcome.error_code << "]");
    CHECK(outcome.status == "terminated");
    CHECK(outcome.output.value("ccy", "") == "GHS");
    CHECK(outcome.output.value("fee", 0.0) == 0.05);
}

TEST_CASE("a data source with an unregistered provider fails the query", "[data][error]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "query.missing", "version": "1.0",
      "nodes": [
        { "id": "declare", "type": "action", "capability": "", "data_sources": [
            { "name": "rows", "provider": "no_such_provider", "config": {} }
          ] },
        { "id": "read", "type": "query", "source": "rows", "output": "out" }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint);
    CHECK(outcome.status == "failed");
    CHECK(outcome.error.find("no_such_provider") != std::string::npos);
}

TEST_CASE("a deferred provider says so instead of returning empty rows", "[data][deferred]") {
    testing::Fixture fixture;
    // The declaration comes from provider config: postgresql is registered as a
    // deferred adapter in this build (no libpq offline), so it must not pretend to
    // have returned an empty table.
    fixture.services.provider_config = std::make_shared<sapo::config::ProviderConfigStore>(
        json{{"data_sources", json::array({{{"name", "pg"},
                                            {"provider", "postgresql"},
                                            {"config", {{"sql", "select 1"}}}}})}});
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "query.deferred", "version": "1.0",
      "nodes": [ { "id": "read", "type": "query", "source": "pg", "output": "rows" } ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint);
    CHECK(outcome.status == "failed");
    CHECK(outcome.error.find("postgresql") != std::string::npos);
}

TEST_CASE("script node evaluates SEL and can bind locals first", "[script]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "script.basic", "version": "1.0",
      "nodes": [
        { "id": "compute", "type": "script", "bindings": { "fee": "amount * 0.02" },
          "code": "min(fee, 10)", "output": "charge" },
        { "id": "end", "type": "terminate", "status": "success", "output": { "charge": "$charge" } }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint, {{"amount", 100}});
    INFO(outcome.error << " [" << outcome.error_code << "]");
    CHECK(outcome.status == "terminated");
    CHECK(outcome.output.value("charge", 0.0) == 2.0);

    const auto capped = vm->runBlueprint(blueprint, {{"amount", 100000}});
    CHECK(capped.output.value("charge", 0.0) == 10.0);   // min() clamped it
}

TEST_CASE("capabilities dispatch through the registry and write declared outputs", "[capability]") {
    testing::Fixture fixture;
    auto vm = fixture.machine();
    fixture.provider->reply("mtn.sms.send", json{{"message_id", "SMS-1"}, {"status", "queued"}});
    const auto blueprint = testing::objectOrThrow(R"JSON({
      "name": "capability.send", "version": "1.0",
      "nodes": [
        { "id": "send", "type": "action", "capability": "mtn.sms.send",
          "input": { "to": "$msisdn", "text": "Your OTP is ${otp}" },
          "output": { "sms_id": "$.message_id", "sms_status": "$.status" }, "next": "end" },
        { "id": "end", "type": "terminate", "status": "success", "output": { "sms_id": "$sms_id" } }
      ]
    })JSON");
    const auto outcome = vm->runBlueprint(blueprint, {{"msisdn", "233201234567"}, {"otp", "4821"}});
    INFO(outcome.error << " [" << outcome.error_code << "] at " << outcome.error_node << ") context=" << outcome.context.dump());
    CHECK(outcome.status == "terminated");
    CHECK(outcome.output.value("sms_id", "") == "SMS-1");
    CHECK(fixture.provider->calls("mtn.sms.send") == 1);
    CHECK(fixture.provider->lastInput()["text"] == "Your OTP is 4821");
    CHECK(fixture.provider->lastInput()["to"] == "233201234567");
}
