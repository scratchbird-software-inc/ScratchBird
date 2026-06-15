# Overview and Support Boundary

**DRAFT — Early Beta documentation. Subject to revision.**

## Purpose

This chapter explains what the ScratchBird AI integration layer does, what it
explicitly does not do, and where the native-only boundary is drawn and
enforced.

---

## What the Layer Does

ScratchBird AI is the AI integration layer for the ScratchBird Convergent Data
Engine (CDE). It provides a managed path between AI clients and the ScratchBird
native engine without exposing raw database connections or bypassing
ScratchBird's compile/execute contract.

Key responsibilities:

- **MCP-oriented orchestration.** The layer exposes a set of canonical tools
  through the Model Context Protocol (MCP) surface. AI clients discover
  capabilities through the tool registry and invoke tools rather than issuing
  SQL directly.

- **Compile/execute split.** Query text is compiled to a ScratchBird artifact
  identifier before execution. The compile step validates syntax and produces a
  traceable artifact; the execute step runs the pre-compiled form. These are
  separate tool invocations.

- **Safe-by-default policy.** Read operations are available through the
  `execute_readonly_query` and `run_query` tool paths without additional
  approval. Write/mutation operations require an explicit approval token and
  pass through the durable approval ledger.

- **Dialect capability matrix.** The layer publishes a machine-readable matrix
  of supported dialects and their per-capability flags. As of the current
  release, only the `native` dialect appears in the matrix.

- **HTTP adapter and local bridge.** An HTTP adapter layer provides
  `mock`, `http`, and `hybrid` operation modes. In `http` and `hybrid` modes,
  a local HTTP bridge process forwards compile, execute, and metadata requests
  to a real ScratchBird server.

- **Governance helpers.** Durable approval ledger, audit bundle generation and
  replay, attestation (HMAC or external reference), in-process quotas and
  rate limits, cost attribution, and HTTP retry/circuit-breaker behavior.

- **Remote MCP sessions.** A session lifecycle for remote MCP clients with
  token negotiation, expiry, and a broad set of supported authentication
  families.

---

## What the Layer Does Not Do

The following are explicitly out of scope for this component:

- **Non-native dialect AI support.** The PostgreSQL, MySQL, and Firebird
  emulation lanes may be present in a ScratchBird deployment profile, but AI
  support for those lanes is not part of this component. Non-native dialect
  requests are rejected with explicit policy errors.

- **Direct raw SQL execution.** Queries pass through the compile/execute split.
  The layer does not provide a raw SQL pass-through path that bypasses
  compilation.

- **Production-grade authorization depth.** Fine-grained authorization and
  hard multi-tenant isolation are not complete in the current early-beta
  surface. The layer has token-based auth and a local approval ledger, but
  not a full enterprise IAM integration.

- **Third-party signing infrastructure.** The audit attestation path supports
  HMAC-SHA256 and external-reference modes. A PKI-backed or externally
  notarized signing infrastructure is not shipped in this release.

- **Automatic live certification for runtime modes not currently exposed.** The
  `manager_proxy`, `local_ipc`, and `embedded_local_only` runtime modes are
  admitted in ScratchBird core and implemented here, but live certification
  evidence for those modes depends on the test environment exposing them. The
  primary certified path is `listener_direct`.

- **Packaged public installer.** The current release ships source-first Beta 1
  instructions, not a packaged installer.

---

## The Native-Only Support Boundary

The native-only boundary is enforced in three places:

1. **Router.** The service router rejects requests that arrive with a
   non-native dialect identifier.

2. **Capability matrix.** The dialect capability matrix schema (`capability-matrix.schema.json`)
   constrains `propertyNames` to `["native"]`. A matrix with any other dialect
   key will not validate.

3. **Compatibility negotiation.** The compatibility negotiation path fails
   closed for any declared server, parser, or driver/runtime version that is
   outside the configured supported window.

The enforcement is not advisory: rejected requests return explicit policy error
codes, not silently degraded results.

---

## The Engine Boundary

The ScratchBird engine execution boundary is `ServerSession`. SQL must be
compiled to SBLR (ScratchBird Bytecode and Logical Representation) before it
is submitted to the engine. The AI layer enforces this by separating the
`compile_query` step from the `execute_compiled` step. The compile step
produces a `compile_artifact_id`; the execute step accepts that identifier,
not raw query text.

Practical implications:

- Query text that cannot be compiled is rejected at the compile step with a
  structured error. Bounded compile-repair can strip common wrapper noise
  (markdown code fences, `sql:` / `query:` prefixes), but it does not rewrite
  semantics.

- The engine's own retrieval metadata is discoverable through the
  `opensearch_meta.*` catalog namespace, which the AI layer treats as a
  first-class introspection surface.

- The runtime mode used for a given connection (`listener_direct`,
  `manager_proxy`, `local_ipc`, `embedded_local_only`) determines the
  transport path. The AI layer maps the `SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP`
  setting to the appropriate transport family.

---

## Capability Matrix (Current State)

The capability matrix at `capability/capability-matrix.v0.json` is the
machine-readable source of truth for dialect capabilities. As of version
`2026-04-20.1`:

| Dialect | Status | read_select | write_dml | ddl | transactions | prepare_bind | metadata_introspection | vector_ops | graph_ops |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `native` | baseline | true | true | true | true | true | true | true | true |

The `status` values the schema admits are: `unavailable`, `experimental`,
`partial`, `baseline`, `full`. The `native` dialect is at `baseline` as of
the current release.

The matrix is validated by `tools/validate_capability_matrix.py` as part of
the standard local validation sequence.
