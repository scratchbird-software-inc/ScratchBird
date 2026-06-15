# Trust and Separation Architecture

## Purpose

This page describes the trust hierarchy and component separation that make
ScratchBird a Convergent Data Engine (CDE). The headline property is this:
**compromising any outer layer does not yield the ability to read, write, or
authorize data**, because durable authority lives only in the engine and is
never delegated outward through SQL text, parser output, driver calls, or
manager commands.

Understanding this architecture is a prerequisite for threat-modelling
ScratchBird deployments, evaluating what isolation a given deployment boundary
actually provides, and diagnosing why certain operations are refused even when
the outer layer appears to have accepted them.

### Security By Design, Not Bolted On

This separation is not a feature layered onto an existing database; it is the
premise the engine was built on. ScratchBird was designed so that a deployment
can be operated to a high-assurance, government-grade security posture **if the
operator chooses to implement it**, rather than requiring such controls to be
retrofitted later. The architecture assumes outer layers are hostile by
default: the engine does not trust the parser, the listener, the manager, or
the client driver, and it revalidates and fail-closes rather than extending
trust. The authentication, authorization, policy, masking, protected-material,
and audit mechanisms documented elsewhere in this guide exist precisely so that
an operator can dial the posture up to what their environment requires without
re-architecting the system.

Two consequences follow for readers. First, the strong controls are
**available from the start** but are largely **opt-in**: a minimal deployment
and a hardened one run the same engine, differing in how much of the security
surface the operator configures. Second, this guide documents the mechanisms
and their source-level enforcement; it makes **no certification claim** (no
FIPS, Common Criteria, or equivalent accreditation is asserted), and any
specific compliance regime must be validated independently against the target
build and configuration.

This page describes architectural intent as enforced by source-level controls
cited throughout. It is a **draft**; no claim here constitutes a production
security certification.

---

## Definitions

**Trust status toward data** — whether a component can directly read or write
durable database state. A component is "untrusted" toward data when it has no
direct path to storage: it can be compromised and still cannot reach or alter
the database files or the engine's internal authority records.

**Authority** — the set of source-documented invariants that decide whether a
request is admitted, executed, and committed. In ScratchBird, authority is held
exclusively by SBcore (the engine). Authority is not shared with, delegated to,
or overridable by drivers, listeners, parsers, or managers.

**SBLR** (ScratchBird Logical Representation) — the structured, typed,
engine-facing form that a parser produces for an accepted statement. SBLR is
_translation evidence_, not authority. The engine independently revalidates
every SBLR envelope before dispatching it.

**MGA** — ScratchBird's transaction and visibility authority model. MGA
transaction inventory is the engine's finality authority; parser packages and
drivers cannot modify or override it.

**Materialized authorization** — the process of evaluating the catalog's
durable grant, role, and group records against a principal UUID to produce an
`EngineMaterializedAuthorizationContext`. Authorization is materialized inside
the engine; it is not derived from parser context or driver claims.

**Fail-closed** — when required evidence is missing, stale, ambiguous, or
contradicted by an explicit denial, the engine refuses the operation. This is
the default posture at every authority boundary in ScratchBird.

---

## The Layered Model

The components form a nested pipeline from the network edge inward to the
engine. Each layer adds a trust boundary; only the innermost layer holds
durable authority.

| Layer | Product name | Role | Trust status toward data | Can do | Cannot do |
|-------|-------------|------|--------------------------|--------|-----------|
| Client driver / tool | Drivers listed in `project/drivers/DriverPackageManifest.csv` | Sends protocol frames; holds no durable state | **Untrusted** | Originate requests; hold credentials for the session handshake; render results | Write database files; commit or roll back transactions; bypass listener admission; escalate privilege beyond what the session's security context permits |
| Listener / network entry point | `SBgate` (`sb_listener`, CMake: `src/listener/`) | Accepts network connections; routes clients to a parser pool; manages parser worker processes | **Untrusted** | Route connections; spawn and supervise parser workers; enforce rate limits and TLS policy | Access storage; issue engine dispatch; override authorization; speak directly to the engine without routing through `SBsrv` |
| Parser worker package | `SBParser` (native SBsql v3, `sbp_native`); compatibility workers (`sbp_firebird`, `sbp_postgresql`, etc.) | Translates a client's language surface into an SBLR envelope; binds visible names through the session schema root | **Untrusted** (registered as `untrusted_translation_package_registration`; `ParserTrustMode::kUntrustedExternalProcess`) | Produce an SBLR envelope; render client-shaped diagnostics; request name resolution through `SBsrv` IPC | Write database files; decide transaction visibility; bypass server revalidation; authenticate locally (authentication must relay to `SBsrv`); promote SQL text to execution authority |
| IPC server / session boundary | `SBsrv` (`sb_server`, CMake: `src/server/`); session records in `ServerSessionRegistry` / `ServerSessionRecord` | Owns the IPC endpoint; performs SBLR revalidation; manages `ServerSessionRecord`s; dispatches admitted envelopes to the engine through `sb_server_engine_bridge` | Intermediary; performs **active revalidation** before the engine sees any parser output | Revalidate SBLR envelopes; enforce admission rules; host the engine via the private bridge adapter; mediate authentication relay; reject malformed, version-mismatched, or fail-closed envelopes | Hold storage state; own transaction finality; override engine authorization decisions |
| Engine | `SBcore` (`sb_engine`, public ABI frozen at `sb_engine_public_abi_1_0_0`) | Owns catalog UUID identity, descriptors, storage, MGA transaction inventory, materialized authorization, policy, diagnostics, and recovery | **Authority** | Execute admitted SBLR envelopes; materialize and enforce authorization; own MGA finality; own catalog state | Accept SQL text as authority; trust parser output without revalidation; expose private internal module targets as deployable products |
| Single-node manager | `SBmgr` (single-node manager; CMake target `sbmn_manager`, `src/manager/node/`) | Lifecycle, cluster discovery, admission, join/renewal, proxy-gate, management operations | **Untrusted toward data** (management channel, not storage authority) | Issue management commands; control listener orchestration; restart policy; config reload | Own transaction finality; speak directly to engine storage; escalate privilege beyond explicit `manager.auth` rights; bypass closed command validation |

The manager sits alongside the pipeline rather than inline. It controls the
deployment lifecycle but does not participate in the data path and has no path
to engine storage.

---

## The Authority Line

Durable authority in ScratchBird has four parts, all located inside SBcore.
Nothing outside SBcore holds, delegates, or overrides any of these.

### Execution authority: SBLR internal API only

Source: `project/docs/public_api/CORE_BETA_PUBLIC_API_ABI.md`, Invariants
section; `project/README.md`.

> Engine execution authority is `engine_sblr_internal_api_only`.

The public C ABI entry point for execution is `sb_engine_dispatch_sblr`
(`src/engine/public_abi.cpp`). There is no path by which a driver, listener,
parser, or manager can invoke engine execution outside this controlled boundary.

### SQL text is never runtime authority

Source: `project/docs/public_api/CORE_BETA_PUBLIC_API_ABI.md`; `project/README.md`
(forbidden list); `project/src/README.md` (forbidden: "SQL text as engine
execution authority").

> SQL text is never runtime authority inside the engine.

A parser may accept SQL text; the text itself confers nothing. The engine acts
only on a revalidated SBLR envelope. This makes it impossible to inject SQL
text at the IPC boundary and have the engine treat it as a direct execution
command.

### UUID identity and descriptor authority

Source: `project/docs/public_api/CORE_BETA_PUBLIC_API_ABI.md`.

> UUID identity and descriptor/operand authority remain internal authority.

Object names visible to parsers and drivers are resolver inputs only.
Durable identity is the UUID assigned by SBcore. A parser that produces a
name-shaped reference does not thereby obtain authority over the named object;
that authority is resolved inside the engine against the catalog UUID.

### MGA transaction inventory as finality authority

Source: `project/docs/public_api/CORE_BETA_PUBLIC_API_ABI.md`;
`src/storage/page/row_data_page.cpp` (`durable_mga_inventory_remains_authority=true`).

> MGA transaction inventory remains finality authority.

Transaction begin, commit, rollback, savepoint, visibility, cleanup, and
recovery state are all SBcore-owned. Drivers and parsers can _request_ these
operations; they cannot determine their outcome.

### Materialized authorization

Source: `security_model_overview.md`; `src/engine/internal_api/security/`.

Authorization is materialized from the catalog's durable grant, role, and group
records. A parser route that carries session identity does not grant rights; the
engine materializes and evaluates authorization independently at execution time.

---

## The Revalidation Boundary

The most concrete trust enforcement is the SBLR revalidation step that
`SBsrv` performs on every SBLR envelope it receives from a parser worker.

### Why revalidation is necessary

Parser workers run as `ParserTrustMode::kUntrustedExternalProcess`
(`src/listener/listener_config.hpp`, line 35–37, 56). The listener spawns them
as external processes. A compromised or maliciously crafted parser output is
therefore possible. Accepting parser-produced SBLR without independent
verification would mean the server's security model is only as strong as the
least-trusted parser.

Revalidation means the server/engine pair never trusts the parser's SBLR
uncritically. The admission step in `src/server/sblr_admission.cpp` re-decodes,
re-checks structure and version, evaluates family rules, and applies fail-closed
controls before dispatching to the engine.

### The diagnostic code

When a parser-submitted SBLR envelope fails server revalidation, the server
issues diagnostic `PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED`
(`src/server/sblr_admission.cpp`, multiple call sites including line 1375):

> "The SBLR envelope failed server revalidation."

Specific failure messages include version mismatches, missing or empty payloads,
unsupported envelope major version, and structural failures at the binary decode
step. None of these result in a fallback or a retry with reduced checks;
every case rejects the submission.

### What revalidation checks

The `sblr_admission.cpp` admission logic verifies:

- envelope version compatibility with this server;
- envelope structural integrity (binary decode must succeed);
- SBLR family admissibility (including fail-closed family rules via
  `IsFailClosedSblrFamily`);
- payload non-emptiness;
- absence of forbidden keys that would allow the parser to smuggle SQL text
  (`source_text`, `sql_text` — checked for duplicate injection).

### Authentication cannot be bypassed at the parser

Source: `src/parsers/sbsql_worker/auth/auth_relay.cpp`.

> "authentication must be relayed to sb_server; parser cannot authenticate
> locally"

A parser worker that attempts local authentication is refused. All
authentication relays through `SBsrv`.

---

## Module and Build Separation

### The server–engine bridge

Source: `src/server_engine_bridge/README.md`; CMake at
`src/server_engine_bridge/CMakeLists.txt`.

`sb_server_engine_bridge` is an INTERFACE library target (no compiled objects of
its own) that lets `sb_server` link the engine without exposing private engine
module names through the server product CMake boundary.

> "The public engine ABI remains `sb_engine`. Internal engine module targets
> remain private build dependencies and are not deployable products."

The implication for separation: the engine's internal modules are not
installable, not distributable, and not reachable except through the engine's
public ABI (`sb_engine`). A server-side change or compromise cannot bypass the
engine's internal authority model by linking a private engine module directly.

### The public engine ABI

The frozen public ABI (`sb_engine_public_abi_1_0_0`) is the only stable,
external-facing interface to the engine. Its C ABI symbols include
`sb_engine_dispatch_sblr` as the sole execution entry point. All other engine
interaction goes through session and transaction management symbols that also
enforce the SBLR-only execution invariant.

Packaged public headers are listed in
`project/docs/public_api/CORE_BETA_PUBLIC_API_ABI.md` under "Packaged Public
Headers". No internal engine module header is in that list.

### Forbidden boundaries in source

Source: `project/src/README.md`.

The implementation source rules establish four forbidden combinations that
encode the separation architecture in build policy:

| Forbidden | Why it matters |
|-----------|----------------|
| Parser code inside IPC | Keeps the IPC boundary clean; parsers are external processes, not IPC participants |
| Compatibility-specific engine behavior inside core engine modules | Prevents compat parsers from having special engine privileges or divergent behavior |
| SQL text as engine execution authority | Closes the most direct injection vector |
| Private cluster authority in public package paths | Prevents cluster-authority escalation through public routes |

### Cluster behavior is outside core

Source: `project/docs/public_api/CORE_BETA_PUBLIC_API_ABI.md`, Invariants.

> "Cluster-positive behavior is outside core and must route through a provider
> or fail closed in non-cluster builds."

The non-cluster refusal code `SBLR.CLUSTER.SUPPORT_NOT_ENABLED` is emitted by
`src/cluster_provider/cluster_provider.hpp` and `src/core/agents/agent_runtime.cpp`
when cluster-positive work is requested without an available provider. The
cluster private operation SBLR family (`sblr.cluster.private_operation.v3`) is
in the fail-closed family set (`src/server/sblr_admission.cpp`, line 87–89):
it cannot be admitted without cluster authority active.

The IPC lifecycle enforces the same boundary at runtime
(`src/server/server_ipc_lifecycle.cpp`, line 491–493):

> "The IPC endpoint is private to cluster authority and must fail closed."

---

## Fail-Closed Principle

ScratchBird's security model applies the fail-closed invariant at every
authority boundary: when required evidence is missing, unproven, stale, or
contradicted, the operation is refused rather than approximated or allowed.

The table below enumerates the verified fail-closed controls and their source
locations.

| Control identifier | Trigger | Source location |
|-------------------|---------|-----------------|
| `PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED` | SBLR envelope fails server revalidation (version mismatch, structural failure, empty payload, binary decode failure) | `src/server/sblr_admission.cpp` |
| `IsFailClosedSblrFamily` | SBLR envelope belongs to a fail-closed family; admitted only when corresponding authority is active | `src/server/sblr_admission.cpp` (line 724) |
| `sblr.cluster.private_operation.v3` | The only member of `kFailClosedSblrFamilies`; cluster-private operations fail closed without cluster authority | `src/server/sblr_admission.cpp` (line 87–89) |
| `ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED` | Cluster-positive work requested in a standalone (non-cluster) build | `src/storage/database/database_lifecycle.cpp`, `src/server/session_registry.cpp`, `src/server/config_policy_security_lifecycle.cpp`, others |
| `SERVER.SUPERVISION.FAIL_CLOSED` | Server supervision condition that requires closure | `src/server/config_policy_security_lifecycle.cpp` (line 459) |
| `decision:fail_closed_when_unproven` | Security lifecycle evidence tag: any unproven threat surface fails closed | `src/server/config_policy_security_lifecycle.cpp` (line 82) |
| `server_authority.unknown.fail_closed.v1` | Unknown server authority route contract; falls back to closed | `src/server/compatibility_server_authority.cpp` (line 331) |
| `server_authority.migration.unknown.fail_closed.v1` | Unknown migration route; fails closed | `src/server/compatibility_server_authority.cpp` (line 412) |
| `compatibility_function.unknown.fail_closed.v1` | Unknown compatibility function route; fails closed | `src/engine/functions/metadata/compatibility_function_surface_policy.cpp` (line 160) |
| `IPC.LIFECYCLE.CLUSTER_AUTHORITY_REQUIRED` | IPC endpoint is cluster-private but cluster authority is unavailable | `src/server/server_ipc_lifecycle.cpp` (line 492–493) |
| `SBLR.CLUSTER.SUPPORT_NOT_ENABLED` | Cluster-positive operation in a non-cluster build | `src/cluster_provider/cluster_provider.hpp`; `src/core/agents/agent_runtime.cpp` |
| `UDR.BRIDGE.SANDBOX_DENIED` | Bridge dispatch requests a physical page-copy, physical backup, or server-local file stream | `src/udr/sbu_sbsql_parser_support/sbu_sbsql_parser_support.cpp` (line 378) |

The common pattern is: "unproven means refuse". There is no escalating retry,
no degraded-mode fallback that reduces checking, and no implicit permit for
authority that was not positively established.

---

## Compromise Scenarios

The table below describes what is and is not reachable if a given layer is
compromised, grounded in the separation controls documented above. This is the
architecture's intent as established by the cited mechanisms; it does not claim
that all implementation paths have been independently audited.

| Compromised component | What is reachable from that position | What is not reachable | Controlling mechanism |
|----------------------|--------------------------------------|----------------------|----------------------|
| Client driver | Network connection to the listener; ability to send well-formed or malformed wire frames; credentials for the session that driver manages | Storage files; engine dispatch without listener/server admission; other sessions' transaction state; materialized authorization of other principals | Driver has no IPC path to the engine; no path to storage; all actions route through listener and server admission |
| `SBgate` listener | Parser worker pool; network connection handling; ability to spawn or kill parser workers; connection routing decisions | Direct IPC to engine; SBLR dispatch without server revalidation; authentication decisions (authentication relays to `SBsrv`); storage | Listener has no engine IPC endpoint; server (`SBsrv`) owns the IPC endpoint and the revalidation step; parser trust mode is `kUntrustedExternalProcess` regardless of listener state |
| Parser worker (`SBParser` or compatibility parser) | Ability to craft arbitrary SBLR envelopes submitted to `SBsrv` IPC; access to names visible in the session schema root | Bypassing `SBsrv` SBLR revalidation (`PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED`); authenticating locally; committing or rolling back transactions without engine admission; reading or writing storage pages; overriding materialized authorization | Parser is registered as `untrusted_translation_package_registration`; `ParserTrustMode::kUntrustedExternalProcess`; all submitted envelopes pass through `sblr_admission.cpp` checks before engine sees them; SQL text is not engine authority |
| `SBmgr` (single-node manager; CMake target `sbmn_manager`) | Lifecycle and config management commands; listener restart; management MCP surface (bounded by `manager.auth.mcp_secret_rights`) | Direct engine IPC; transaction finality; storage access; SBLR dispatch; overriding security contexts | Manager is a control-plane component, not an IPC server or storage authority; management commands require bounded idempotency keys and explicit rights; `ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED` refuses cluster-positive manager paths in standalone builds |

Note: these scenarios describe the trust boundary architecture. A compromise
that also involves physical access to storage files or the ability to inject
shared libraries into the engine process is outside the scope of this
architectural model.

---

## Cross-References

- [security_model_overview.md](./security_model_overview.md) — the three-layer
  model (authentication, authorization, deep enforcement) and the fail-closed
  invariant as applied to security principal operations
- [authentication_and_providers.md](./authentication_and_providers.md) —
  provider trust states, plugin admission, and how authentication evidence is
  normalized by the engine
- [../Getting_Started/architecture/engine_parser_boundary.md](../Getting_Started/architecture/engine_parser_boundary.md) —
  end-user explanation of the parser/engine boundary and why parsers are
  translators, not storage authorities
- [../Language_Reference/core_paradigms/security_and_sandboxing.md](../Language_Reference/core_paradigms/security_and_sandboxing.md) —
  fail-closed security model from the SBsql Language Reference perspective;
  materialized security, sandbox roots, row-level security, and masking
- [../Operations_Administration/parser_registration_and_routes.md](../Operations_Administration/parser_registration_and_routes.md) —
  how parser packages are registered, versioned, and routed; the `ParserHello`
  protocol and IPC protocol version enforcement
