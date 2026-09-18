# Re-architecting USSD Session Flow with Sapo DSL Engine

This document outlines the approach for replacing the complex, legacy manual state machine (`NaloUssdInteractionManager.cs`) with the standardized Sapo DSL Engine inside the `WssdApi` C++ project.

## Goal Description

We will transition the USSD flow execution logic to the new `sapo` DSL Engine while ensuring high concurrency and strict architectural boundaries. The C++ `UssdSessionService` will act as a highly concurrent orchestrator that leverages the precompiled Sapo Engine library.

### Key Requirements Addressed:
1. **Compiled Artifact**: We will link the precompiled `libsapo_core.a` static library into `WssdApi` rather than compiling the Sapo DSL source files directly within the project. This keeps the `WssdApi` lightweight and cleanly separated.
2. **High Concurrency with `drogon::Task`**: The orchestrator must handle millions of concurrent users seamlessly. We will wrap the Sapo VirtualMachine executions in `drogon::Task` coroutines to ensure non-blocking scalability.
3. **Redis State & Navigation Cursor**: The navigation state stored in Redis will explicitly track the user's current menu and selected option. On every request, the `RuntimeContext` will be hydrated from Redis, handed over to Sapo for execution, and upon suspension/completion, the newly mutated context will be synced back to Redis under the session ID.

## Proposed Changes

---

### Build Configuration

#### [MODIFY] [CMakeLists.txt](file:///Users/emmanueladdo-odame/Documents/cpp_kitchen/WssdApi/CMakeLists.txt)
- Remove any direct inclusion of Sapo source files (if present).
- Add the `sapo-engine/include` directory to `target_include_directories`.
- Link `WssdApi` against the precompiled `/Users/emmanueladdo-odame/Documents/cpp_kitchen/sapo-engine/build/libsapo_core.a`.

---

### UssdSessionService

#### [MODIFY] [UssdSessionService.h](file:///Users/emmanueladdo-odame/Documents/cpp_kitchen/WssdApi/domain_services/ussd_sessions/UssdSessionService.h)
- Inject the Redis client dependency (e.g., `drogon::nosql::RedisClientPtr`).
- Include the necessary Sapo headers (`<sapo/runtime/VirtualMachine.hpp>`, `<sapo/runtime/Context.hpp>`) and `NaloToSapoTranslator.hpp`.
- Ensure all handlers return `drogon::Task<...>` to support coroutines.

#### [MODIFY] [UssdSessionService.cc](file:///Users/emmanueladdo-odame/Documents/cpp_kitchen/WssdApi/domain_services/ussd_sessions/UssdSessionService.cc)
- **`handleNaloUssdSessionInteraction` Refactoring**:
    - **Initiation Phase** (`USERDATA` starts with `*`):
        - `co_await` the fetch of the USSD Service Flow from PostgreSQL using the USSD Code.
        - Call `NaloToSapoTranslator::Translate(ussdFlowArray)` to generate the Sapo blueprint.
        - Initialize a `sapo::runtime::RuntimeContext`, populating it with initial session data (`MSISDN`, `NETWORK`).
        - Execute `VirtualMachine::runBlueprint()` (wrapped appropriately to maintain coroutine thread safety if Sapo Engine performs blocking operations, though in-memory AST execution is typically fast enough to run synchronously inline).
        - Retrieve the generated prompt from the Context.
        - Initialize the navigation cursor (e.g., current menu ID) within the context.
        - `co_await` saving the serialized state (`context.serializeState()`) and the JSON blueprint to Redis using the `SESSIONID` as the key.
        - Return the prompt to the user.

    - **Continuation Phase**:
        - `co_await` the fetch of the saved serialized `RuntimeContext` and Sapo blueprint from Redis using the `SESSIONID`.
        - Instantiate the `RuntimeContext` and load the state (`context.deserializeState()`).
        - Read the current navigation cursor to determine where the user is.
        - Inject the user's new input (`USERDATA`) into the appropriate context variable.
        - Execute `VirtualMachine::resumeBlueprint()`.
        - Retrieve the new prompt and update the navigation cursor within the context.
        - `co_await` saving the updated context back to Redis.
        - Respond with the new prompt, or terminate if the session is complete.

## Verification Plan

### Automated Tests
- Ensure `libsapo_core.a` links correctly without undefined symbol errors during the `WssdApi` build process.

### Manual Verification
- Start a local instance of the application.
- Trigger a mock Nalo USSD webhook POST request for the initiation phase.
- Verify the system properly translates the flow, suspends, stores the cursor and state in Redis, and returns the correct prompt.
- Send a follow-up request to verify the `drogon::Task` properly pulls state from Redis, resumes execution, and syncs the new mutated state back.
