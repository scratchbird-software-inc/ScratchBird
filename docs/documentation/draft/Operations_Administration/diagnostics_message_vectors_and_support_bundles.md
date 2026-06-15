# Diagnostics, Message Vectors, And Support Bundles

## Purpose

When something goes wrong in ScratchBird — or when a parser refuses a statement, a database open is blocked, or the listener enters a degraded state — the engine produces structured diagnostic output. This chapter explains the diagnostic model, the channels through which diagnostics flow, the refusal classes operators can expect, the SBSQL diagnostic codes defined in the registry, and how to collect, review, and redact a support bundle before sharing it.

---

## The Message Vector Model

A **message vector** is a structured list of diagnostic records attached to an operation result. Rather than a single error string, ScratchBird operations produce a vector of records, each carrying:

- A `diagnostic_code` string (namespaced, dot-separated)
- A severity level (`ERROR`, `WARNING`, `INFO`, or similar)
- A human-readable detail string
- Optional structured fields

In the UDR call result struct, this is the `message_vector_json` field on `UdrCallResult`. A caller receives the full vector and can inspect each record independently.

Source: `src/udr/runtime/sb_udr_runtime.hpp:36-40`.

### Diagnostic Channels

Diagnostics are routed to named channels. Two channels are significant for operators:

| Channel | What flows through it |
|---|---|
| `diagnostic.canonical_message_vector` | General engine and parser diagnostics |
| `diagnostic.lifecycle.message_vector` | Lifecycle events, emulated statement boundaries, and startup/shutdown diagnostics |

The lifecycle channel is where refusals from emulated compatibility syntax appear. When you see `SBSQL.EMULATION.NON_FILE_OPERATION` or `SBSQL.EMULATION.REFERENCE_TOOL_NOT_EXECUTED`, they are routed through `diagnostic.lifecycle.message_vector`.

Source: `src/parsers/sbsql_worker/statement/statement_catalog.cpp:614,640,666,692`.

---

## Refusal Classes

A **refusal** is a controlled, expected operational outcome. It means the engine recognized a request, determined it cannot or should not fulfill it, and said so clearly. A refusal is not a crash, a timeout, or a silent drop.

ScratchBird defines several refusal classes for storage and page operations:

| Refusal State | Meaning |
|---|---|
| `refused` | Request was received and explicitly denied |
| `recovery_required` | The filespace or page agent is in a state that requires recovery before it can accept new work |
| `invalid_filespace_identity` | A boundary violation: the filespace identity presented is not valid for this operation |
| `invalid_page_family` | A boundary violation: the page family presented does not match the agent's domain |

Source: `src/storage/page/page_filespace_handoff.hpp:57-78`.

### UDR Bridge Refusal Codes

The SBsql parser support bridge declares the following refusal classes:

| Code | Trigger |
|---|---|
| `UDR.BRIDGE.CONTEXT_MISSING` | Required context packet absent |
| `UDR.BRIDGE.SECRET_MATERIAL_DENIED` | Secret material access not permitted from this surface |
| `UDR.BRIDGE.SANDBOX_DENIED` | Operation denied by sandbox policy |
| `UDR.BRIDGE.UNSUPPORTED` | Operation not supported by this provider |
| `UDR.BRIDGE.MISSING_CAPABILITY` | Required capability not registered |
| `UDR.BRIDGE.UNLICENSED` | Operation requires a license gate not satisfied |
| `UDR.BRIDGE.STREAM_INVALID` | Stream state is invalid for the requested operation |
| `UDR.BRIDGE.AUTH_FAILED` | Authentication check failed |
| `UDR.BRIDGE.IDEMPOTENCY_MISSING` | Idempotency token required but absent |
| `UDR.BRIDGE.CUTOVER_FAILED` | Cutover refused; validated compare evidence not present |

Source: `src/udr/sbu_sbsql_parser_support/sbu_sbsql_parser_support.cpp:312-322`.

---

## SBSQL Parser Diagnostic Codes

The SBSQL parser worker emits a defined set of diagnostic codes. Key codes operators encounter:

| Code | Condition |
|---|---|
| `SBSQL.EMULATION.NON_FILE_OPERATION` | Statement uses file-backed syntax (e.g., `BACKUP DATABASE` without `TO`, `NBACKUP`, shadow file operations) that has no filesystem side effect in SBsql |
| `SBSQL.EMULATION.REFERENCE_TOOL_NOT_EXECUTED` | Statement invokes a reference native tool (`GBAK`, `GFIX`, `GSTAT`, `GSEC`, `FBSVCMGR`, `FBTRACEMGR`) which is not executed by the SBsql parser |
| `SBSQL.SERVER.UNAVAILABLE` | Parser server is not available to process the request |
| `SBSQL.EXECUTION.REJECTED` | Execution was rejected by the engine |
| `SBSQL.RESOURCE.STATEMENT_TOO_LARGE` | Statement exceeds the allowed size limit |
| `SBSQL.RESOURCE.SBLR_ENVELOPE_TOO_LARGE` | SBLR envelope exceeds the allowed size limit |
| `SBSQL.RESOURCE.IDENTIFIER_TOO_LARGE` | Identifier exceeds the allowed size |
| `SBSQL.RESOURCE.LITERAL_TOO_LARGE` | Literal value exceeds the allowed size |
| `SBSQL.RESOURCE.PARAMETER_COUNT_EXCEEDED` | Statement has more parameters than permitted |
| `SBSQL.RESOURCE.AST_DEPTH_EXCEEDED` | Parse tree is too deeply nested |
| `SBSQL.COPY.TARGET_UUID_MISSING` | COPY stream operation is missing a target UUID |
| `SBSQL.COPY.DATA_ROW_INVALID` | A data row in a COPY stream is malformed |
| `SBSQL.COPY.NO_ROWS` | COPY stream completed with no rows |
| `SBSQL.COPY.EXECUTION_REJECTED` | COPY execution rejected by engine |
| `SBSQL.COPY.CLIENT_ABORTED` | Client aborted a COPY stream |
| `SBSQL.LIFECYCLE.MAPPED` | Statement successfully mapped to a lifecycle handler |
| `SBSQL.STATEMENT.EXACT_REFUSAL_REQUIRED` | Statement requires an exact refusal and no implicit fallback is permitted |
| `SBSQL.PARSER.EMPTY_STATEMENT` | Statement text is empty |
| `SBSQL.PARSER.STATEMENT_FAMILY_UNKNOWN` | Statement family is not recognized by the vertical-slice parser |

Sources: `src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp`, `src/parsers/sbsql_worker/wire/sbsql_test_wire.cpp`, `src/parsers/sbsql_worker/statement/statement_catalog.cpp`, `src/parsers/sbsql_worker/ast/ast.cpp`, `src/parsers/sbsql_worker/binder/binder.cpp`.

### Lexer and Encoding Codes

The lexer emits additional codes for malformed input:

- `SBSQL.LEXER.STRING_UNCLOSED`, `SBSQL.LEXER.IDENTIFIER_UNCLOSED`, `SBSQL.LEXER.DIRECTIVE_UNCLOSED`
- `SBSQL.LEXER.DELIMITED_IDENTIFIER_UNCLOSED`
- `SBSQL.LEXER.UUID_LITERAL_INVALID`
- `SBSQL.LEXER.INVALID_CONTROL`
- `SBSQL.ENCODING.INVALID_UTF8`
- `SBSQL.UNICODE.BIDI_CONTROL_FORBIDDEN`
- `SBSQL.UNICODE.COMBINING_MARK_WITHOUT_BASE`

These indicate the input text is malformed at the lexical level, before any semantic analysis.

---

## Startup and Format Diagnostics

Database open failures produce specific codes:

| Code | Meaning |
|---|---|
| `FORMAT.VERSION_DOWNGRADE_REFUSED` | The database's format version is newer than this build supports; opening would be a downgrade |
| `ENGINE.DBLC_FORMAT_DOWNGRADE_REFUSED` | Same refusal observed at the engine lifecycle layer |
| `SB-STARTUP-STATE-FORMAT-DOWNGRADE-REFUSED` | Startup state format version is newer than this build; open refused |
| `SB-STARTUP-STATE-MAGIC-INVALID` | Startup state block has an invalid magic marker |
| `SB-STARTUP-STATE-BODY-TOO-SMALL` | Startup state block is truncated |

Sources: `src/storage/disk/database_format.cpp:194`, `src/storage/database/database_lifecycle.cpp:4361`, `src/storage/database/startup_state.cpp:214-374`.

---

## Support Bundles

A **support bundle** is a structured snapshot of operational state collected for diagnostic review. It is the first thing an escalation recipient will ask for when investigating a service problem.

### What a Bundle Contains

The listener support bundle (`ListenerSupportBundleSnapshot`) includes:

- Listener configuration (after redaction of sensitive paths)
- Socket identity
- Current lifecycle state, drain state, and stop-requested flag
- Whether new connections are being accepted
- Accept sequence, open connection count, queue depth, pending handoff bindings
- Handoff complete and reject totals
- Parser pool status
- Metrics snapshot (JSON)
- Management decision log (last up to 64 events, each with timestamp, operation, outcome, diagnostic code, and safe detail)
- Runtime event log

Source: `src/listener/listener_support_bundle.hpp:34-51`.

The manager support bundle (`GenerateManagerSupportBundle`) adds:

- Manager configuration summary (with paths redacted)
- Status JSON and metrics JSON
- Audit file reference, metrics file reference
- Lifecycle state file and lifecycle journal file
- Agent observability JSON

Source: `src/manager/node/manager_support_bundle.hpp:20-35`.

### Collecting a Bundle

The `collect_support_bundle` UDR operation is available across parser support packages. When invoked it assembles the current state and writes it to the configured bundle directory.

For integrated collection across multiple components, the Python tool `tools/ceic_integrated_support_bundle.py` can collect from listener and manager in a single pass.

### Redaction

Before a support bundle leaves your environment, verify that it has been redacted. ScratchBird applies automatic redaction:

- `RedactListenerSupportabilityText` replaces sensitive content in free-text fields.
- `RedactManagerSupportBundleText` replaces path values with `[path-redacted]` and secret refs with `<redacted-secret-ref-present>`.
- Local path policy is recorded as `local_path_policy=redacted` in the bundle manifest.
- Keyring paths and restart executable paths are replaced with `<redacted-path-present>` rather than omitted entirely, so the reviewer knows they are configured without seeing the actual values.

Source: `src/listener/listener_support_bundle.hpp:53`, `src/manager/node/manager_support_bundle.cpp:56-169`.

**Review a bundle before sharing it.** Automated redaction handles known sensitive fields. Custom configuration that places secrets or hostnames in unexpected fields may not be caught. Open the bundle files and scan for values you would not want in a support ticket.

### Late Payload Redaction

For page-level operations, `late_payload_fetch` enforces a redaction gate: a security snapshot and redaction policy must be bound before a payload is fetched. If `redaction_required` is set, any attempt to retrieve the unredacted bytes without explicit authorization produces `storage.page.late_payload_fetch.unredacted_protected`. This ensures that protected material at the page level cannot bypass the redaction policy even during diagnostic collection.

Source: `src/storage/page/late_payload_fetch.cpp:148-212`.

---

## Operator Review Checklist

Before sharing a support bundle externally:

1. Verify the `redaction_profile` line in the bundle manifest.
2. Scan `config-redacted.txt` for any path or secret values that should not be visible.
3. Confirm the audit log entries in the bundle do not contain user data payloads.
4. Confirm the metrics JSON does not contain values derived from protected content.
5. If the bundle was collected during a security incident, treat it as potentially sensitive until reviewed by your security team.

---

## Related Pages

- [Troubleshooting](troubleshooting.md)
- [Monitoring, Health, And Readiness](monitoring_health_and_readiness.md)
- [Language Reference: Refusal Vectors](../Language_Reference/syntax_reference/refusal_vectors.md)
- [Getting Started: Diagnostics And Support Bundles](../Getting_Started/administration/diagnostics_and_support_bundles.md)
