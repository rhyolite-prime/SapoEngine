//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//
#pragma once


#include <memory>
#include <future>

#include "parser/AstNodes.hpp"
#include "runtime/Context.hpp"

namespace sapo::tasks {

    /**
     * @brief The status outcome of a single task execution slice.
     */
    enum class TaskOutcome {
        Success,   // Task finished successfully; advance to next step
        Failed,    // Task hit a recoverable business rule failure or error block
        Yielded,   // Task is waiting on a non-blocking external event (e.g., HTTP Callback, Timer)
        Terminate  // Task forces the entire VM workflow execution stack to halt instantly
    };

    /**
     * @brief Context wrapper bundled with an explicit task invocation pass.
     * Contains the parsed AST configuration metadata for this specific step block instance.
     */
    struct TaskExecutionContext {
        std::shared_ptr<parser::AstNode> node_config;
        runtime::RuntimeContext& runtime_memory;
    };

    /**
     * @brief Base Abstract Polymorphic Class for all Sapo DSL execution plugin engines.
     */
    class ITask {
    public:
        virtual ~ITask() = default;

        /**
         * @brief Executes the core task implementation block asynchronously.
         * @param exec_context Holds references to the node properties and current variable memory.
         * @return A std::future wrapping the TaskOutcome, facilitating non-blocking orchestration.
         */
        virtual std::future<TaskOutcome> execute(TaskExecutionContext exec_context) = 0;
    };

} // namespace sapo