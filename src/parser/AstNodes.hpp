//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//


#pragma once

#include <string>
#include <vector>
#include <variant>
#include <optional>
#include <map>
#include <memory>

namespace sapo::parser {

    // Forward declaration of an Expression type for dynamic conditions/mappings
    // In Sapo DSL, an expression can be a raw string expression (like "$item.active == true")
    // or a structured comparison object.
    using Expression = std::variant<std::string, struct ExpressionMap>;

    struct ExpressionMap {
        std::string left;
        std::string operator_str; // e.g., "==", ">", "+"
        std::string right;
    };

    // Strongly typed representation of schema projection mappings (used in Transform)
    using MappingValue = std::variant<std::string, std::map<std::string, std::string>>;

    // Base Task Types supported by the Sapo Runtime Engine
    enum class TaskType {
        Transform,
        Query,
        Command,
        Event,
        Loop,
        Choice,
        Wait,
        Schedule,
        Subflow,
        Script,
        Parallel,
        Terminate,
        Noop,
        Condition,
        Action
    };

    // Base Abstract Class for all AST Nodes
    class AstNode {
    public:
        virtual ~AstNode() = default;
        [[nodiscard]] virtual TaskType getType() const = 0;
        
        // Every task node in Sapo optionally tracks its own ID for jump targets
        std::optional<std::string> id;
    };

    // --- 1. TRANSFORM NODE ---
    class TransformNode : public AstNode {
    public:
       [[nodiscard]] TaskType getType() const override { return TaskType::Transform; }

        std::string operation;      // "filter", "map", "project", "merge"
        std::string input;          // Raw input payload target variable (e.g., "$users")
        MappingValue mapping;       // The transition logic rule or schema mapping template
        std::string output;         // Output target variable token name
    };

    // --- 2. QUERY NODE ---
    class QueryNode : public AstNode {
    public:
       [[nodiscard]] TaskType getType() const override { return TaskType::Query; }

        // These are the missing symbols Clangd is complaining about!
        std::string data_source_id;
        std::string query_statement;
        std::vector<std::string> query_parameters;
        std::string output_context_key;
    };

    // --- 3. COMMAND NODE ---
    class CommandNode : public AstNode {
    public:
       [[nodiscard]] TaskType getType() const override { return TaskType::Command; }

        std::string command;                         // Dot-notation target route (e.g., "user.create", "http.post")
        std::optional<std::variant<std::string, std::vector<std::string>>> output; // Optional receipt metadata destination or array of keys to extract

        // Optional block — only required for "http.*" commands
        struct HttpAuthConfig {
            std::string type;
            std::string username;
            std::string password;
        };

        //optional block - only required for notify.email commands
        struct EmailConfig {
            std::string to;
            std::string subject;
            std::string template_id;
        };

        struct HttpRequestConfig {
            std::string url;                                          // Target API endpoint (expression-aware)
            std::optional<HttpAuthConfig> auth;                        // Optional authentication details
            std::optional<std::map<std::string, std::string>> headers; // HTTP Headers (Content-Type, Authorization, etc.)
            std::optional<std::map<std::string, std::string>> query;   // URL query parameters
            std::optional<MappingValue> body;                          // Request body: raw string or structured object
            std::optional<int> timeout;                                // Optional request timeout in milliseconds
        };
        std::optional<HttpRequestConfig> http_request; // Present only when command starts with "http."
        std::optional<EmailConfig> email_config; // Present only when command is "notify.email"
    };

    // --- 4. EVENT EMIT NODE ---
    class EventEmitNode : public AstNode {
    public:
       [[nodiscard]] TaskType getType() const override { return TaskType::Event; }

        std::string name;                                      // Target event bus routing key
        std::optional<std::map<std::string, std::string>> payload; // Dynamic context payload
        std::optional<std::string> next;                       // Next sequential task wire identifier
    };

    // --- 5. WAIT NODE ---
    class WaitNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Wait; }

        std::optional<std::string> duration;    // Time delta string (e.g., "10m")
        std::optional<Expression> until;        // Absolute ISO timestamp evaluation rule
        std::optional<std::string> next;        // Resumption execution sequence link
    };

    // --- 6. SCHEDULE NODE ---
    class ScheduleNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Schedule; }

        std::string cron;                       // Standard 5/6 field background timing string
        std::optional<std::string> timezone;    // Context reference time standard
        std::string body;                       // Downstream task ID target to wake up
        bool enabled{true};                     // Background loop active state flag
    };

    // --- 7. SUBFLOW NODE ---
    class SubflowNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Subflow; }

        std::string workflow;                   // External registered module file pointer
        bool wait_for_completion{true};         // Call-stack block vs detached fire-and-forget
        std::optional<std::map<std::string, std::string>> inputs; // Parameter crossing boundaries
        std::optional<std::string> output;      // Aggregated return data capture target
    };

    // --- 8. SCRIPT NODE ---
    class ScriptNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Script; }

        std::string language;                   // Target interpreter engine sandbox (e.g., "expr")
        std::string code;                       // Inline expression raw calculations script
        std::optional<std::string> output;      // Evaluation outcome storage variable
    };

    // --- 9. TERMINATE NODE ---
    class TerminateNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Terminate; }

        std::string status;                     // "success", "failed", "cancelled"
        std::optional<std::string> error_code;  // Uppercase lookup categorization key
        std::optional<Expression> message;      // Descriptive context metadata
    };

    // --- 10. NOOP NODE ---
    class NoopNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Noop; }

        std::optional<std::map<std::string, std::string>> meta; // Visually layout tracking fields
    };

    // --- 11. CONDITION NODE ---
    class ConditionNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Condition; }

        std::string expression;                // e.g., "subtotal >= 100" or "status == 'SUCCESS'"
        std::string on_true;                   // Target node ID to jump to if true
        std::optional<std::string> on_false;   // Target node ID to jump to if false
    };

    // --- 12. Choice Node Structure ---
    class ChoiceNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Choice; }

        std::string variable_key;                    // The context variable to evaluate (e.g., "response_code")
        std::map<std::string, std::string> cases;    // Mapping of expected values to target Node IDs
        std::optional<std::string> default_target;   // Fallback Node ID if no cases match
    };

    // --- 13. Parallel Node Structure ---
    class ParallelNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Parallel; }

        // A collection of independent task configurations to execute simultaneously
        std::vector<std::shared_ptr<AstNode>> child_tasks;

        // Optional: Determine if failure of one branch cancels everything immediately
        bool fail_fast = true;
    };

    // --- 14. ACTION NODE ---
    class ActionNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Action; }

        struct SystemEvent {
            std::string event_name;
            std::optional<Expression> trigger_condition;
        };

        struct DataSource {
            std::string name;
            std::string scope; // "internal" | "external"
            std::string provider;
            std::optional<std::map<std::string, std::string>> config;
        };

        struct TaskReference {
            std::string task_id;
            std::optional<Expression> execute_condition;
        };

        struct PromptConfig {
            Expression message;
            std::string interaction_type;
            std::optional<Expression> input_validation;
            std::optional<int> timeout_ms;
        };

        std::string capability;
        std::optional<PromptConfig> prompt_config;
        
        std::optional<SystemEvent> on_event;
        std::vector<DataSource> data_sources;
        std::map<std::string, MappingValue> inputs;
        std::optional<std::map<std::string, std::string>> outputs;
        std::vector<TaskReference> next_tasks;
    };

    // Type alias for a collection of polymorphically tracked workflow nodes
    using WorkflowAST = std::vector<std::shared_ptr<AstNode>>;

} // namespace sapo