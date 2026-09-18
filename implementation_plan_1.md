# Standardizing USSD Flow with Sapo Engine DSL

The current USSD architecture relies on a highly bespoke JSON schema coupled tightly to a heavy C# state machine (`NaloUssdInteractionManager.cs`). To standardize this onto the Sapo DSL Engine, we must move away from a **"Menu & Option"** mental model and adopt Sapo's **"Task & Transition"** (AST node) graph model. 

## Deployment Architecture (Hybrid Mode)

To accommodate strict USSD protocol requirements alongside heavy processing tasks, the Sapo Engine will be deployed in a **Hybrid Architecture**:

1. **Synchronous API Layer (Interactive UI):** 
   - Deployed directly inside the C# Web API (via interop or microservice).
   - Responsible for rapidly executing nodes up to the next `ActionNode` to synchronously return menu text back to the Telecom provider within the tight 2-second timeout window.
2. **RabbitMQ Consumer Layer (Background Processing):**
   - A separate fleet of Sapo Engine workers acting as queue consumers.
   - Responsible for executing heavy payloads, `CommandNode` external HTTP calls, and `ScheduleNode` deferred-routines asynchronously without blocking the user's active session.

*Note: Sapo engine's `ContextMemory` will be seamlessly serialized to and deserialized from Redis, allowing a suspended workflow on the API to resume cleanly on a background worker, and vice versa.*

## Proposed Architectural Shift (Handling Complex Flows)

After reviewing `ussd-flow-1.json`, the proposed AST-based Sapo approach scales *better* for complex scenarios than the C# state machine. Complex structures in `ussd-flow-1.json` are decomposed elegantly into smaller AST nodes.

### Advanced Component Mapping

| USSD Schema Concept | Sapo Engine AST Node | Description |
| :--- | :--- | :--- |
| `Menu` (Title/Prompt) | `ActionNode` | Emits the USSD text prompt to the user and suspends state waiting for input. |
| `actionType: menu` | `ChoiceNode` (Switch/Case) | Evaluates the user's input (e.g., '1', '2') and jumps to the corresponding Next Node (`nextMenuId`). |
| `actionType: input` & `menu-input` | `ActionNode` + `ScriptNode` | Validates free-form input (like `inputType: decimal` or `number`). The `ActionNode` receives it, and the `ScriptNode` validates and saves it. |
| `actionType: invocation` | `CommandNode` (`http.get`/`post`) | Fetches or pushes data dynamically (replaces `flexConnectId`). Output variables map directly into Sapo Context Memory. |
| `actionType: deferred-routine` | `ScheduleNode` / Async | Sapo natively handles background/deferred tasks without blocking the main USSD loop. |
| `actionType: display` | `TerminateNode` / `EventEmitNode` | Outputs the final message (e.g. "Payment successful") to the user and cleans up the context. |
| `conditionalRendering` | `ConditionNode` / `ChoiceNode` | Skips menus or hides options based on context variables (e.g., `accountBalance > 0`). |
| `contextVariables` | Native Memory Injection | Translates seamlessly to `Context.setVariable()` before proceeding to the next node. |

## Translation Example

Here is how a complex interaction (Display Prompt -> User Input -> HTTP Invocation -> Deferred Routine) translates from legacy JSON into standard Sapo DSL:

```json
[
    {
        "type": "action",
        "id": "menu_get_amount",
        "prompt": "Please enter amount to pay:",
        "output": "user_amount_input"
    },
    {
        "type": "script",
        "id": "validate_amount",
        "expr": "if (!is_decimal($user_amount_input)) { jump_to = 'menu_get_amount'; }",
        "next": "route_validated"
    },
    {
        "type": "command",
        "id": "process_payment_invocation",
        "command": "http.post",
        "http_request": {
            "url": "https://api.gateway.com/pay",
            "body": { "amount": "$user_amount_input" }
        },
        "output": "payment_receipt",
        "next": "trigger_deferred_notification"
    },
    {
        "type": "schedule",
        "id": "trigger_deferred_notification",
        "cron": "in 5 minutes",
        "target": "subflow_check_status"
    }
]
```

## Migration Strategy (Phase 1)

1. **Dynamic In-Memory Translator**: Since the USSD flow is supplied in memory at runtime (e.g., from a database or caching layer), we will build a C# service (or an embedded C++ parser extension) that takes the in-memory USSD object model and dynamically translates it into an in-memory Sapo DSL JSON string (or directly into AST Nodes) on the fly.
2. **Sapo Suspend/Resume State**: Ensure the Sapo engine's `ContextMemory` can be serialized/deserialized seamlessly to Redis so that when the user replies, the VM resumes exactly at the next node.
3. **API Integration**: Deprecate the rigid `flexConnectId` database mapping and replace it entirely with inline `CommandNode` blocks utilizing the newly tested C++ Requests (`cpr`) logic.

Does this architectural map address the complexities found in your larger configuration files?
