//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//

#include "ParallelTask.hpp"

#include "parser/AstNodes.hpp"
#include "ActionTask.hpp"
#include "TransformTask.hpp"
#include <iostream>
#include <future>
#include <vector>

namespace sapo::tasks {


    // Helper utility to instantiate and run an isolated worker thread polymorphically
    static TaskOutcome executeIsolatedChild(std::shared_ptr<parser::AstNode> node, runtime::RuntimeContext& forked_memory)
    {
        TaskExecutionContext child_ctx{ node, forked_memory };
        std::future<TaskOutcome> fut;

        // Resolve task workers directly based on AST signatures
        if (node->getType() == parser::TaskType::Command) {
            ActionTask worker;
            fut = worker.execute(child_ctx);
        } else if (node->getType() == parser::TaskType::Transform) {
            TransformTask worker;
            fut = worker.execute(child_ctx);
        } else {
            std::cerr << "   -> [Parallel Core] Error: Unsupported concurrent task type.\n";
            return TaskOutcome::Failed;
        }

        return fut.valid() ? fut.get() : TaskOutcome::Failed;
    }

    std::future<TaskOutcome> ParallelTask::execute(TaskExecutionContext exec_context)
    {

        auto parallel_node = std::static_pointer_cast<parser::ParallelNode>(exec_context.node_config);

        std::cout << "[DEBUG] Child tasks size: " << parallel_node->child_tasks.size() << std::endl;
        std::cout << "[DEBUG] Fail fast: " << parallel_node->fail_fast << std::endl;

        // Note: We use pass-by-reference/pointer capture for master context manipulation safely
        // because the main engine loop explicitly blocks on our top-level future resolution.
        auto& master_memory = exec_context.runtime_memory;
        std::string node_id = parallel_node->id.value_or("unnamed_parallel");
        auto tasks_to_run = parallel_node->child_tasks;
        bool fail_fast = parallel_node->fail_fast;

        return std::async(std::launch::async, [node_id, tasks_to_run, fail_fast, &master_memory]() -> TaskOutcome {
            try {
                std::cout << "[Parallel Hub: " << node_id << "] Forking " << tasks_to_run.size() << " concurrent branches...\n";

                // 1. Create isolated heap pointer memory blocks to preserve your non-copyable context design
                std::vector<std::unique_ptr<runtime::RuntimeContext>> forked_memories;
                std::vector<std::future<TaskOutcome>> branch_futures;

                forked_memories.reserve(tasks_to_run.size());
                branch_futures.reserve(tasks_to_run.size());

                for (const auto& child_node : tasks_to_run) {
                    // Instantiate a pristine runtime context on the heap
                    auto isolated_space = std::make_unique<runtime::RuntimeContext>();

                    // Safely duplicate variables from master via thread-safe state serialization streams
                    isolated_space->loadState(master_memory.serializeState());
                    forked_memories.push_back(std::move(isolated_space));

                    // Safely unpack raw heap references out to background system thread tasks
                    runtime::RuntimeContext& parallel_ref = *forked_memories.back();
                    branch_futures.push_back(std::async(std::launch::async, executeIsolatedChild, child_node, std::ref(parallel_ref)));
                }

                // 2. Join Barrier: Wait for all background branches to return receipts
                bool overall_success = true;
                for (size_t i = 0; i < branch_futures.size(); ++i) {
                    TaskOutcome branch_result = branch_futures[i].get();
                    if (branch_result == TaskOutcome::Failed) {
                        std::cerr << "   -> [Parallel Branch Error] Thread index [" << i << "] failed execution.\n";
                        overall_success = false;
                        if (fail_fast) return TaskOutcome::Failed;
                    }
                }

                if (!overall_success) {
                    return TaskOutcome::Failed;
                }

                // 3. Thread-Safe State Merge Phase
                std::cout << " -> [Parallel Join] Synchronizing variant memory registries back to Master state...\n";
                for (const auto& localized_space_ptr : forked_memories) {
                    // Loop through the modifications made in the branch and merge variables back.
                    auto variables_snapshot = nlohmann::json::parse(localized_space_ptr->serializeState());
                    for (const auto& [key, val] : variables_snapshot.items()) {
                        // Skip system temporary override flags
                        if (key == "__SYS_NEXT_JUMP") continue;

                        // Commit localized computations safely back to primary storage
                        master_memory.setVariable(key, val);
                    }
                }

                std::cout << "[Parallel Hub: " << node_id << "] Synchronization finalized. Execution paths joined successfully.\n";
                return TaskOutcome::Success;

            } catch (const std::exception& e) {
                std::cerr << "[Parallel Hub Exception]: Internal pipeline split error: " << e.what() << "\n";
                return TaskOutcome::Failed;
            }
        });
    }

} // namespace sapo::tasks