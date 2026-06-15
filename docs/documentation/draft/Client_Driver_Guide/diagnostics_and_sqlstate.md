# Diagnostics and SQLSTATE

## Purpose

This page describes the `native_sqlstate` diagnostic-mapping profile — the profile used by
all standard ScratchBird drivers except FlightSQL. It explains how the engine's structured
message vectors and refusal vectors surface as SQLSTATE codes and errors at the client, and
what retry behavior is appropriate for each class.

Sources used: `project/drivers/DriverPackageManifest.csv`,
`project/drivers/driver/python/BASELINE_REQUIREMENT_MAPPING.md`,
`project/drivers/driver/go/S1_CONN_IMPLEMENTATION.md`,
and documentation at
`docs/documentation/draft/Language_Reference/syntax_reference/refusal_vectors.md`,
`docs/documentation/draft/Operations_Administration/diagnostics_message_vectors_and_support_bundles.md`.

This is a **draft**. Components are in `beta_2` / `release_candidate` status.

---

## Diagnostic Profiles

| Profile | Manifest Value | Used by |
| --- | --- | --- |
| Native SQLSTATE | `native_sqlstate` | All drivers and adaptors except FlightSQL |
| gRPC status + SQLSTATE | `grpc_status_sqlstate` | flightsql driver only |

Source: `DriverPackageManifest.csv` column `diagnostic_mapping_profile`.

---

## Engine Message Vector Model

The ScratchBird engine produces **message vectors** — structured lists of diagnostic records
rather than single error strings. Each record in the vector carries:

- A `diagnostic_code` string (namespaced, dot-separated).
- A severity level (`ERROR`, `WARNING`, `INFO`).
- A human-readable detail string.
- Optional structured fields.

Source: `docs/documentation/draft/Operations_Administration/diagnostics_message_vectors_and_support_bundles.md` —
"A message vector is a structured list of diagnostic records attached to an operation result."

These flow through named diagnostic channels:

| Channel | Content |
| --- | --- |
| `diagnostic.canonical_message_vector` | General engine and parser diagnostics |
| `diagnostic.lifecycle.message_vector` | Lifecycle events, emulated statement boundaries, startup/shutdown diagnostics |

---

## SQLSTATE Surfacing in native_sqlstate Drivers

Under the `native_sqlstate` profile, the engine's `kError` SBWP message carries a SQLSTATE
field, a detail string, and an optional hint string. Drivers map this to their host language's
error hierarchy.

The Python driver's error mapping (`src/scratchbird/errors.py`) defines the following lanes:

| SQLSTATE Range / Code | Driver Error Class | Retry Boundary |
| --- | --- | --- |
| `40001`, `40P01` | Serialization / deadlock error | Fresh statement boundary only — never auto-replay a whole transaction |
| `08xxx` | Connection class error | Reconnect or reopen — do not retry the statement without restoring the connection |
| All other | Application error | No automatic replay |

Source: `project/drivers/driver/python/BASELINE_REQUIREMENT_MAPPING.md` — "`retry_scope_for_sqlstate(...)`
makes the retry boundary explicit: `40001`/`40P01` => fresh statement only, `08xxx` => reconnect
or reopen only, everything else => no automatic replay."

The Go driver confirms the same `0A000` SQLSTATE for fail-closed auth method rejection:
"admitted but unsupported or broker-required methods (`MD5`, `PEER`, `REATTACH`) now fail
closed with `0A000`".

Source: `project/drivers/driver/go/S1_CONN_IMPLEMENTATION.md`.

---

## Refusal Vector Classes

SBsql defines three top-level refusal classes. These surface as SQLSTATE and error text in
the `native_sqlstate` profile:

| Refusal Class | Meaning | Retry Expectation |
| --- | --- | --- |
| `unsupported` | The surface, option, route, shape, profile, build flag, or provider operation is not available in this build or for this target. | Only after changing the statement, build profile, provider, or feature set. |
| `denied` | Blocked by authorization, sandboxing, policy, safety, recovery state, resource admission, descriptor rules, stream rules, or data-protection rules. | Only after the blocking authority condition changes. |
| `unlicensed` | The surface and route are recognized, but the running product profile or admitted provider reports that the capability is not licensed. | Only with a product profile or provider that licenses the capability. |

Source: `docs/documentation/draft/Language_Reference/syntax_reference/refusal_vectors.md` — "High-Level Classes".

The engine never silently drops a refused request. Refusal is a controlled, expected
operational outcome.

---

## UDR Bridge Refusal Codes

When SBsql statements cross the UDR bridge, the following codes may appear in the
message vector:

| Code | Trigger |
| --- | --- |
| `UDR.BRIDGE.CONTEXT_MISSING` | Required context packet absent |
| `UDR.BRIDGE.SECRET_MATERIAL_DENIED` | Secret material access not permitted from this surface |
| `UDR.BRIDGE.SANDBOX_DENIED` | Operation denied by sandbox policy |

Source: `docs/documentation/draft/Operations_Administration/diagnostics_message_vectors_and_support_bundles.md`.

---

## Storage Refusal States

Storage and page operations can produce the following refusal states:

| State | Meaning |
| --- | --- |
| `refused` | Request was received and explicitly denied |
| `recovery_required` | The filespace or page agent requires recovery before accepting new work |
| `invalid_filespace_identity` | The filespace identity presented is not valid for this operation |
| `invalid_page_family` | The page family presented does not match the agent's domain |

Source: `docs/documentation/draft/Operations_Administration/diagnostics_message_vectors_and_support_bundles.md` —
page_filespace_handoff refusal states.

---

## Error Hierarchy in Drivers

The `native_sqlstate` profile expects drivers to expose a DB-API-compatible error hierarchy:

- Protocol errors carry `SQLSTATE`, `DETAIL`, and `HINT` fields from the `kError` SBWP message.
- Parser-failure fallback behavior is defined for cases where SQLSTATE is absent or
  the server closed the connection before delivering an error.

Source: `project/drivers/driver/python/BASELINE_REQUIREMENT_MAPPING.md` — `ERR` row: "DB-API
error hierarchy, SQLSTATE-to-error-class mapping, retry-boundary classification (`statement` vs
`reconnect` vs `none`), protocol error message shaping (`SQLSTATE`/`DETAIL`/`HINT`), and
parser-failure fallback behavior."

---

## Cross-References

- [wire_protocol_sbwp.md](wire_protocol_sbwp.md) — `kError` message (code `0x48`) in the SBWP message type table
- [Language Reference: Refusal Vectors](../Language_Reference/syntax_reference/refusal_vectors.md) — full refusal vector specification
- [Operations Administration: Diagnostics, Message Vectors, and Support Bundles](../Operations_Administration/diagnostics_message_vectors_and_support_bundles.md) — operator-side diagnostics and support bundle collection
