# Database Lifecycle Baseline Inventory

Status: draft baseline
Search key: `DATABASE-LIFECYCLE-BASELINE-INVENTORY`

This inventory records current authority inputs, implementation anchors, and test anchors for the full database lifecycle execution_plan. It is execution evidence for `DBLC-000`; it does not define product behavior.

## Authority Baseline

The controlling authority chain is:

1. `public_contract_snapshot` with search key `SPEC-RECON-0003-MANIFEST-ZERO-GREY`.
2. `public_contract_snapshot` with search keys `SPEC-RECON-0001-AUTHORITY-ZERO-GREY` and `MGA-EXT-AUTHORITY-ORDER`.
3. Manifest-listed normative chapters, registries, accepted decisions, and implementation packets for bounded implementation guidance.
4. Current code and tests as evidence only until reconciled through the audit matrix.
5. This execution_plan as execution control only.

MGA authority is fixed by `DR-MGA-0001`: durable transaction inventory is the single-node finality authority. Parser state, donor state, WAL/redo/undo, CRUD text events, wall-clock order, UUID order, and file timestamps are not finality authority.

## Implementation Anchors

| Surface | Current anchors | Baseline status |
| --- | --- | --- |
| Database lifecycle storage | `project/src/storage/database/database_lifecycle.hpp`; `project/src/storage/database/database_lifecycle.cpp` | Current create/open/shutdown/recovery/repair/drop evidence exists and requires reconciliation to the complete lifecycle state machine. |
| Startup lifecycle evidence | `project/src/storage/database/startup_state.hpp`; `project/src/storage/database/startup_state.cpp` | Startup state, ownership, clean shutdown, tx1/tx2, and recovery classification evidence exists and requires full lifecycle closure. |
| Filespace lifecycle | `project/src/storage/filespace/filespace_lifecycle.hpp`; `project/src/storage/filespace/filespace_lifecycle.cpp`; `project/src/storage/filespace/filespace_secondary.cpp` | Filespace role/state/evidence behavior exists and must be coupled to database lifecycle and UUID registration. |
| MGA transaction inventory | `project/src/transaction/mga/transaction_inventory.hpp`; `project/src/transaction/mga/transaction_inventory.cpp`; `project/src/transaction/mga/transaction_horizon.cpp`; `project/src/transaction/mga/transaction_recovery.cpp` | Durable transaction inventory and horizon/recovery behavior exist and remain the finality authority. |
| Server hosted lifecycle | `project/src/server/engine_host.cpp`; `project/src/server/lifecycle.cpp`; `project/src/server/startup.cpp` | Hosted database open/auto-create and server lifecycle behavior exists but must close isolation, drain, ownership, and lifecycle state propagation gaps. |
| Session and auth lifecycle | `project/src/server/session_registry.hpp`; `project/src/server/session_registry.cpp`; `project/src/engine/internal_api/security/authentication_api.*`; `project/src/engine/internal_api/security/auth_provider_model.cpp` | Auth/session handoff exists but requires proof that the engine/security authority decides authentication and authorization. |
| Listener and parser pool lifecycle | `project/src/listener/listener_runtime.*`; `project/src/listener/parser_pool.*`; `project/src/server/listener_orchestrator.*` | Listener launch, handoff, management, parser pool, drain, stop, and restart behavior exists and requires database-scoped association and failure closure. |
| Parser/server IPC route | `project/src/wire/parser_server_ipc/`; `project/src/server/ipc_server.cpp`; `project/src/server/sblr_dispatch_server.*`; `project/src/parsers/sbsql_worker/` | SBPS/SBSQL route evidence exists but must close full SBWP/TLS SBSQL parser IPC server engine route and no parser authority bypass. |
| Catalog identity and bootstrap roots | `project/src/core/catalog/catalog_records.*`; `project/src/core/catalog/bootstrap_schema_roots.hpp`; `project/src/engine/internal_api/catalog/name_registry.cpp` | Catalog records and bootstrap roots exist and must close `sys.catalog`, `sys.information`, identity resolver, UUID, generation, and index-profile requirements. |
| Database-local agents | `project/src/core/agents/agent_runtime.*`; `project/src/core/agents/agent_engine_lifecycle.*` | Agent policy/state infrastructure exists and must close database engine lifecycle agent startup, shutdown, safe mode, and authority boundaries. |

## Current Test Anchors

| Test area | Current anchors | Baseline status |
| --- | --- | --- |
| Create/bootstrap | `project/tests/sbsql_parser_worker/sbsql_database_create_schema_bootstrap_gate.cpp`; `project/tests/conformance/storage/database_create_open_show.yaml` | Existing coverage is partial and must expand to full tx1 bootstrap, system schemas, policy, metrics, resource seeds, catalog UUIDs, and tx2 activation. |
| MGA regression | `project/tests/mga_transaction_regression/`; `project/tests/conformance/transactions/` | Required for all transaction-sensitive lifecycle closure. |
| Listener/server route | `project/tests/listener/`; `project/tests/sbsql_parser_worker/sbsql_full_route_execution_smoke.cpp`; `project/tests/sbsql_parser_worker/sbsql_sbwp_tls_engine_auth_route_smoke.py` | Existing route tests prove important pieces but must be mapped to lifecycle labels and expanded where route composition is incomplete. |
| Engine public ABI | `project/tests/engine_public_abi/`; `project/tests/engine_public_abi/public_abi_symbol_gate.cpp` | Must stay included for lifecycle API/ABI closure. |
| Firebird and donor parser | `project/tests/firebird_parser_worker/`; `project/tests/sbsql_parser_worker/` | Donor lifecycle mapping must remain separate from native lifecycle authority and cannot add cross-dialect dependencies. |

## Baseline Unknowns Closed For P0

No unknown authority class is admitted for this execution_plan. Any later source discovered during implementation must be classified as one of:

- manifest-listed contract;
- manifest-listed registry;
- accepted decision record;
- implementation packet;
- current implementation evidence;
- current test evidence;
- execution_plan execution evidence;
- finding/audit evidence.

If a source cannot be classified, the affected slice records `AUTHORITY.FILE_NOT_LISTED` or `AUTHORITY.CONTRADICTION_UNRESOLVED` in the failure inventory and pauses before runtime behavior is inferred.
