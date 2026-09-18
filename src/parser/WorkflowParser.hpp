//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//

#pragma once

#include "AstNodes.hpp"
#include <string>

namespace sapo::parser {

    class WorkflowParser {
    public:
        // Main structural entry point: converts raw JSON string into a valid execution AST
        static WorkflowAST parse(const std::string& json_content);
    };

} // namespace sapo