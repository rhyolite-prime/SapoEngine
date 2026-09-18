//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//

#pragma once


#include "Context.hpp"
#include <memory>
#include <map>
#include "parser/AstNodes.hpp"

namespace sapo::runtime {

    class Interpreter {
    public:
        Interpreter(parser::WorkflowAST ast);

        /**
         * @brief Executes the loaded workflow from the beginning or from a specific node ID.
         * @param context The runtime variable memory arena to operate on.
         * @param start_node_id Optional ID of the node to resume execution from.
         * @return true if the workflow completed or suspended successfully, false if it failed.
         */
        bool execute(RuntimeContext& context, const std::string& start_node_id = "");

    private:
        // Helper to index nodes by their user-defined ID for lightning-fast jumps/lookups
        void indexNodes();

        // Evaluates a single AST Node and returns the ID of the next node to run, if any
        std::optional<std::string> executeNode(const std::shared_ptr<parser::AstNode>& node, RuntimeContext& context);

        parser::WorkflowAST m_ast;
        std::map<std::string, std::shared_ptr<parser::AstNode>> m_node_registry;
    };

} // namespace sapo