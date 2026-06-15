# AI Integration Guide

**DRAFT — Early Beta documentation. Subject to revision.**

This manual describes the ScratchBird AI integration layer: an MCP-oriented
service component that connects AI workflows to ScratchBird's native
parser/compiler and execution paths.

---

## What ScratchBird AI Is

ScratchBird AI is the AI integration layer for ScratchBird. It provides:

- An MCP-oriented service and tool registration surface.
- A compile/execute split orchestration path that routes SQL through the
  ScratchBird parser and compiler before engine submission.
- An HTTP adapter and local HTTP bridge for connecting AI clients to a running
  ScratchBird server.
- Safe-by-default policy controls with read-only defaults and approval-gated
  mutation paths.
- Governance helpers: durable approval ledger, audit bundle attestation,
  in-process quotas, rate limits, and cost attribution.
- Remote MCP session lifecycle with a broad set of advertised authentication
  families.
- A machine-readable dialect capability matrix.

Current release track: **early beta / technical Beta 1** (`0.1.0`).
Status timestamp: April 20, 2026.

---

## Native-Only Support Boundary

ScratchBird AI supports **ScratchBird native engine workflows only**.

- Non-native dialect requests are rejected with explicit policy errors.
- Emulated external engines (PostgreSQL, MySQL, Firebird emulation lanes) are
  out of scope for this component's AI layer.
- The ScratchBird engine execution boundary is `ServerSession`. SQL must be
  compiled to SBLR (ScratchBird Bytecode and Logical Representation) before it
  is submitted to the engine.

This boundary is enforced in the router, capability matrix, and compatibility
negotiation path. It is not configurable.

For a detailed explanation, see [overview_and_support_boundary.md](./overview_and_support_boundary.md).

---

## Manual Map

| Chapter | Contents |
| --- | --- |
| [overview_and_support_boundary.md](./overview_and_support_boundary.md) | What the layer does and explicitly does not do; native-only enforcement; engine boundary |
| [architecture_adapter_and_bridge.md](./architecture_adapter_and_bridge.md) | Adapter modes (mock/http/hybrid), local HTTP bridge, compile/execute split, transport and front-door modes |
| [mcp_tools_and_control_surface.md](./mcp_tools_and_control_surface.md) | Full MCP tool inventory organized by family; native control surface families |
| [governance_quotas_and_audit.md](./governance_quotas_and_audit.md) | Safe-by-default policy, approval-gated mutation, approval ledger, audit bundles, quotas, circuit breaker |
| [remote_mcp_and_authentication.md](./remote_mcp_and_authentication.md) | Remote MCP session lifecycle; advertised auth families; transport modes |
| [runtime_configuration.md](./runtime_configuration.md) | All adapter and bridge environment variables; runtime profile model; secret provider |
| [getting_started.md](./getting_started.md) | Install, validate, run bridge, run MCP stack — distilled from Ubuntu and Windows install guides |
| [release_status_and_known_gaps.md](./release_status_and_known_gaps.md) | Current status, known gaps, support matrix, conformance gates — dated early-beta framing |
| [troubleshooting.md](./troubleshooting.md) | Diagnostics for bridge boot failures, auth errors, compatibility failures, approval errors |

---

## Reading Model

Read this manual in chapter order for a first-time bring-up. For an operator
who already has the stack running:

1. [runtime_configuration.md](./runtime_configuration.md) for environment
   variable reference.
2. [governance_quotas_and_audit.md](./governance_quotas_and_audit.md) for
   approval and audit controls.
3. [troubleshooting.md](./troubleshooting.md) for live failure diagnosis.

---

## Related Manual Cross-Links

- Getting started with ScratchBird as a whole: see `../Getting_Started/`
- Operations and administration topics: see `../Operations_Administration/`
- SBsql language reference: see `../Language_Reference/`

---

## Source Authority

This manual was assembled from the following source documents under
`project/ai/`:

- `README.md` — support policy, current surface, environment variables, repo layout
- `docs/guides/GETTING_STARTED_EARLY_BETA.md`
- `docs/guides/INSTALL_UBUNTU_BETA1.md`
- `docs/guides/INSTALL_WINDOWS_BETA1.md`
- `docs/guides/RUNTIME_CONFIGURATION_AND_GOVERNED_OPERATION.md`
- `docs/guides/LIVE_BRIDGE_TROUBLESHOOTING.md`
- `docs/releases/INITIAL_EARLY_BETA_RELEASE_2026-02-18.md`
- `docs/releases/EARLY_BETA_CONFORMANCE_GATES.md`
- `docs/releases/BETA1_SUPPORT_MATRIX_2026-04-18.md`
- `docs/releases/BETA1_REMAINING_LIVE_TASKS_2026-04-18.md`
- `docs/status/EARLY_BETA_STATUS_2026-03-07.md` (content refreshed 2026-04-20)
- `docs/status/EARLY_BETA_KNOWN_GAPS_2026-03-07.md` (content refreshed 2026-04-20)
- `capability/capability-matrix.v0.json` and `capability-matrix.schema.json`
- `src/scratchbird_ai/mcp_server.py`
- `src/scratchbird_ai/remote_sessions.py`
- `src/scratchbird_ai/scratchbird_core_surface.py`
- `src/scratchbird_ai/http_bridge.py`
- `examples/http-bridge.env.example`
