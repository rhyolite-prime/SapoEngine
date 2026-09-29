# ShortCodeExpress

**Visual USSD builder for Sapo Engine.** Design USSD services on a drag-and-drop canvas and ship the *exact* Sapo DSL workflow blueprint that runs on the Sapo virtual machine — no translation layer, no drift.

Built with **Nuxt 4 + Tailwind CSS**, **Vue Flow**, **Apache ECharts**, and a TypeScript port of the Sapo DSL serializer/parser/validator and a faithful simulator of the C++ `Interpreter` runtime semantics.

---

## What you get

| Area | Highlights |
| --- | --- |
| **Instant provisioning (DX)** | Sign up in seconds and get a live `sk_live_…` API key. Buy a short code through a Ghana-flavoured **webcheckout** (MTN/Vodafone/AT MoMo prompt or card) — pick your own code (`*714*42#`) or let the system assign one (default). Every purchase auto-deploys a **starter flow** (dynamic menus, live API call, MoMo charge) so the code is **dialable within 5 minutes**. Top up session packs (5k–150k) through the same checkout. |
| **Live USSD gateway** | `POST /api/ussd/dial` + `POST /api/ussd/input` (cookie or Bearer API key) run the code's released blueprint on the SapoSimulator with quotas enforced, usage stats recorded, and a phone-style **dialer UI** (`/dial`) with keypad + live session variables. |
| **Signed webhooks** | HMAC-signed (`x-sapexp-signature`, Stripe-style `t=…,v1=…`) events: `payment.succeeded` (webcheckout **and** in-flow charges), `shortcode.assigned`, `session.started/completed/failed`, `flow.http_request`, `sessions.topped_up`. Built-in test receiver, delivery log with payloads, resend, and a test-event button. |
| **Sandbox rails** | `/api/mock/pay` (always approves → fires `payment.succeeded`) and `/api/mock/account` (per-MSISDN pseudo balance) let flows exercise real money paths safely. Test cards ending `0000` and MoMo numbers ending `000` decline. |
| **Visual builder** | Drag-and-drop canvas (Vue Flow) with all Sapo node kinds: menus, prompts, HTTP commands, conditions, retries, timeouts, try/catch/finally, loops, parallel, switch, script, variable, filter, database, exit. Handles enforce Sapo routing semantics (`next`, `error`, `option:`, `case:`, `then/else`, `body/catch/finally`). |
| **1:1 blueprint fidelity** | The canvas *is* the blueprint. `graphToBlueprint()` emits Sapo DSL JSON (nodes, `prompt_config`, `next_tasks`, `execute_condition`, try frames, loop `next`, `on_error`…); `blueprintToGraph()` is its exact inverse. Import any Sapo blueprint (e.g. from `examples/`) and edit it visually. |
| **Live blueprint view** | Blueprint drawer with live JSON, one-click copy, import/export round-trip. |
| **USSD phone simulator** | In-app phone frame running a TypeScript simulator that mirrors the C++ `Interpreter` (advanceTryPhase, handleError with `on_error` → nearest catch → finally → propagate, loop `on_item_error: continue`, if frame semantics). Drive menus, inspect screens and variables step by step. |
| **Build & release (Azure-style)** | Every save builds a validated artifact with a date-based build number (`YYYYMMDD.N`), compile errors and warnings listed like Azure Pipelines. Promote builds to releases (`vMAJOR.MINOR.PATCH` auto-tag), supersede, and **one-click rollback** to any prior release with a full audit trail. |
| **Team collaboration** | Invite teammates by email with owner / editor / viewer roles; pending invites show a copyable magic-link and can be rescinded. |
| **Quotas & billing** | Plans (starter / growth / scale) with monthly USSD session quotas per short code or business, overage rates, session top-up packs, invoices, and projected next invoice. |
| **Dashboard** | Apache ECharts analytics: sessions over time, top flows, short codes provisioned, quota consumption, revenue split. |

## Run it

```bash
cd shortcode-express
npm install
npm run dev          # http://localhost:3000
```

Sign in as any seeded demo user (e.g. `ama@shortcode.express`) — the login page lists them. The database (`.data/`) is seeded on first boot from the Sapo Engine `examples/` blueprints (daccu, momo, kyc) so the builder, simulator, and dashboard have real flows to work with.

```bash
npm run build && node .output/server/index.mjs   # production
```

## Architecture

```
app/
  pages/            dashboard (ECharts), flows, builder/[id], shortcodes, billing, team,
                    developers (keys/webhooks/docs), signup, onboarding (5-min wizard),
                    checkout/[id] (webcheckout), dial (live phone)
  components/builder/  Palette, CanvasNode, Inspector, BuildPanel, Simulator, BlueprintDrawer
  components/EChart.vue  thin ECharts SSR-safe wrapper
  composables/useBuilder.ts  canvas state ↔ graph model
  utils/nodeVisuals.ts      kind icons + colors
shared/
  types.ts          Flow, Build, Release, ApiKey, CheckoutSession, WebhookDelivery, …
  utils/sapo.ts     PALETTE + canvas↔blueprint converters + validator (TS port of serializer/parser)
  utils/sel.ts      Sapo expression language parser (TS port)
  utils/vm.ts       SapoSimulator — mirrors src/runtime/Interpreter.cpp semantics
server/
  api/              flows, builds, releases, rollback, team invites, shortcodes (+suggest/available),
                    billing, checkout (create/pay), keys, webhooks (+echo receiver), ussd (dial/input),
                    mock (pay/account sandbox rails), simulate/http
  utils/db.ts       file-backed store + SESSION_PACKS + plans, seeded from ../examples
  utils/ussd.ts     live gateway: sessions, quota enforcement, usage stats, webhook firing
  utils/webhooks.ts signed (HMAC) outbound delivery + delivery log
  utils/starter.ts  auto-deployed starter flow (canvas graph → blueprint → build → release)
```

### Fidelity guarantee

The serializer/importer pair is unit-verified against the real example blueprints: `blueprintToGraph → graphToBlueprint` round-trips `examples/daccu_ussd_service.json`, `examples/momo_service.json`, and `examples/kyc_verification.json` back to semantically identical blueprints (try/catch/finally chains, if `then`/`else` arrays with `next` resume targets, loop bodies, option routing via `next_tasks` + `execute_condition`). The simulator runs the same blueprints with the engine's frame-stack semantics.

## Notes

- Auth is a lightweight demo cookie (`sce_user`); swap in a real IdP for production.
- The `simulate/http` endpoint is a dev proxy so `http.*` commands can call real APIs from the browser without CORS pain.
