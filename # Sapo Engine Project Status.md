# Sapo Engine Project Status

## 1. System Architecture Diagram

The Sapo Engine is designed as a modular, DSL-driven execution engine. Below is an architecture diagram representing the core system components and their interactions.

```text
======================================================================
                         SAPO RUNTIME ENGINE                          
======================================================================

  [Client / main.cpp]
           |
           | (JSON Blueprint & Initial State)
           v
 +-------------------+             +-----------------------+
 |                   |   (JSON)    |                       |
 |  VirtualMachine   | ----------> |    WorkflowParser     |
 |                   |             |                       |
 +-------------------+             +-----------------------+
           |                                 |
           | (AST & Initial State)           | (Abstract Syntax Tree)
           v                                 |
 +-------------------+                       |
 |                   | <---------------------+
 |    Interpreter    |
 |                   |
 +-------------------+
           |
           | (Manages Execution Flow)
           v
 +-------------------+             +-----------------------+
 |   Task Registry   | . . . . . . |   RuntimeContext      |
 |   / Node Index    |             |   (Memory Arena)      |
 +-------------------+             +-----------------------+
           |
           | (Instantiates & Dispatches)
           v
 +=======================================================+
 ||                  ITask Interface                    ||
 +=======================================================+
    |         |         |         |         |         |
    v         v         v         v         v         v
 +----+    +----+    +----+    +----+    +----+    +-------+
 |ATsk|    |CTsk|    |STsk|    |CdTk|    |WTsk|    | Other |
 +----+    +----+    +----+    +----+    +----+    +-------+
    .         .         .                             
    .         .         .                             
    . . . . . . . . . . . (Read/Write Variables) -----> [RuntimeContext]
              |
              | (External HTTP)
              v
       [External API]

Keys: 
ATsk = ActionTask, CTsk = CommandTask, STsk = ScriptTask
CdTk = ConditionTask, WTsk = WaitTask
```

### Component Breakdown
* **`VirtualMachine`**: The supervisor that orchestrates the overall lifecycle, delegating to the parser and interpreter.
* **`WorkflowParser`**: Converts the JSON DSL representation into an Abstract Syntax Tree (AST) composed of `AstNode`s.
* **`Interpreter`**: Coordinates the AST traversal, executing nodes sequentially, managing branching, and tracking state.
* **`RuntimeContext`**: The memory arena for the workflow. It manages dynamic variables and output states passed between tasks.
* **`ITask` & Task Implementations**: Polymorphic execution plugins (e.g., `ActionTask`, `ScriptTask`, `CommandTask`) that perform the actual business logic asynchronously.

---

## 2. Work Quantification (Completed vs. Remaining)

### ✅ Completed Work
Based on the `sapo-engine` codebase, the foundational core engine is fully implemented.

1. **DSL Grammar Definition**: The standard BNF (`plugin-dsl-bnf.txt`) defining task types, expressions, control flow, and inputs/outputs is thoroughly documented.
2. **Parser & AST (`src/parser`)**: `WorkflowParser` is built to convert JSON text into memory-safe C++ AST structures.
3. **Core Runtime (`src/runtime`)**: 
   - `VirtualMachine` supervisor is working and executable (as seen in `main.cpp`).
   - `Interpreter` node traversal and memory management are implemented.
   - `RuntimeContext` environment is ready to handle state changes.
4. **Task Definitions (`src/tasks`)**: C++ implementations exist for nearly all task types defined in the grammar:
   - `ActionTask`, `ChoiceTask`, `CommandTask`, `ConditionTask`, `EventTask`, `LoopTask`, `ParallelTask`, `QueryTask`, `ScheduleTask`, `ScriptTask`, `Subflow`, `TerminateTask`, `TransformTask`, `WaitTask`.
5. **Build & Dependencies**: 
   - CMake is fully configured (`CMakeLists.txt`), downloading and linking `nlohmann/json` automatically.
   - `Catch2` test harness runner is integrated and configured.

### 🚧 Remaining Work to Build
While the core engine is robust, the actual ecosystem integrations and robust operational features need to be built.

1. **Plugins Implementation (`plugin-to-develop.txt`)**: 
   None of the standard plugins for the ecosystem have been developed yet. This involves writing action handlers and defining standard DSL blueprints for:
   - **Authentication**: 2 Factor Auth, Google Signin, Microsoft Signin, Session Manager.
   - **AI Providers**: OpenAI, Anthropic, Google Gemini plugins.
   - **Storage**: AWS S3, Google Drive, Cloudinary plugins.
   - **Utilities & Content**: NOS Importer, SMTP Plugin, Custom Fields, Google Maps, Contact Forms, RSS importer, Device Detector, Traffic/Analytics, Coupons, Taxes.
2. **Frontend Components (`[fe:component]`)**:
   As tagged in the plugins file, certain plugins (e.g., Google/Microsoft Signin, Device Detector) require UI components that are entirely pending.
3. **Comprehensive Test Suite**:
   Though `Catch2` is linked and a `TestRunner.cpp` exists, there are currently no extensive unit tests in the `tests/` directory verifying individual node behaviors, expressions, or edge cases.
4. **Networking/HTTP Core**:
   While `main.cpp` mocks an `http.post` call to Hubtel, the underlying `CommandTask` or generic network layer likely needs to be connected to a robust C++ HTTP client (like libcurl or cpr) to natively execute HTTP commands defined in the DSL.
5. **Expression Evaluator Engine**: 
   The DSL specifies an `expr` language for script evaluations (e.g., `0.2 * amount`). Implementing or integrating a robust expression evaluator or a scripting backend (like Lua, which is also mentioned in the BNF) is required.
