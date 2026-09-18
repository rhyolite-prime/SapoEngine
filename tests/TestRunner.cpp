//
// Created by Emmanuel Addo-Odame on 14/06/2026.
//

// tests/TestRunner.cpp
#include "../src/third_party/catch2/catch_amalgamated.hpp" // Point to your downloaded header

// Include the headers you want to test
#include "../src/runtime/Context.hpp"
#include "parser/AstNodes.hpp"
#include "tasks/ParallelTask.hpp"
#include "tasks/QueryTask.hpp"
#include "tasks/Task.hpp"

using namespace sapo::runtime;

TEST_CASE("ParallelTask processes 4 child branches", "[parallel]") {
    using namespace sapo::runtime;
    using namespace sapo::tasks;
    using namespace sapo::parser;

    RuntimeContext master_memory;
    auto parallel_node = std::make_shared<ParallelNode>();

    // Create 4 distinct transformation tasks
    for(int i = 1; i <= 4; ++i) {
        auto child = std::make_shared<TransformNode>();
        child->operation = "assign";
        child->input = std::to_string(i * 10);
        child->output = "var" + std::to_string(i);
        parallel_node->child_tasks.push_back(child);
    }

    TaskExecutionContext ctx{parallel_node, master_memory};
    ParallelTask task;

    // Execution
    auto future = task.execute(ctx);
    REQUIRE(future.get() == TaskOutcome::Success);

    // Verification: Ensure all 4 variables were merged into Master memory
    for(int i = 1; i <= 4; ++i) {
        auto val = master_memory.getVariable("var" + std::to_string(i));
        REQUIRE(val.has_value());
        REQUIRE(val.value() == std::to_string(i * 10));
    }
}

TEST_CASE("QueryTask reads state and binds output", "[query]") {
    using namespace sapo::runtime;
    using namespace sapo::tasks;

    RuntimeContext memory;
    memory.setVariable("target_id", "USER_99");

    // 1. Setup QueryNode configuration
    auto query_node = std::make_shared<sapo::parser::QueryNode>();
    query_node->data_source_id = "core_postgres";
    query_node->query_statement = "SELECT balance FROM accounts WHERE id = :target_id;";
    query_node->query_parameters = {"target_id"};
    query_node->output_context_key = "account_data";

    TaskExecutionContext ctx{query_node, memory};
    QueryTask task;

    // 2. Execute
    auto future = task.execute(ctx);
    REQUIRE(future.get() == TaskOutcome::Success);

    // 3. Verify Mapping
    // Check if the output_context_key now holds the simulated data
    auto result = memory.getVariable("account_data");
    REQUIRE(result.has_value());
}