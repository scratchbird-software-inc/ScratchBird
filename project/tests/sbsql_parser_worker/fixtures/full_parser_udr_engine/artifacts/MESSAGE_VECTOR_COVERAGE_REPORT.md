# Message Vector Coverage Report

Status: complete
Search key: `FSPE-010A-MESSAGE-VECTOR-COVERAGE-REPORT`

## Coverage Summary

The FSPE-010A gate validates `MESSAGE_VECTOR_COVERAGE_BACKLOG.csv` as the diagnostic/message-vector coverage registry seed for this execution_plan slice.

Validated coverage:

- 41 unique diagnostic-code rows.
- Required origins: `agent`, `server`, `engine`, `parser`, `udr`, `listener`, `manager`.
- Required diagnostic prefixes: `AGENT`, `SERVER`, `ENGINE`, `SBSQL`, `UDR`, `LISTENER`, `MANAGER`.
- Required subsystems: physical device, filespace, page manager, parser IPC, security, syntax, literal, resource, encoding, binding, context, authority, resolver, admission, runtime, streaming, catalog, transaction, datatype, function, optimizer, metrics, endpoint, control, and cleanup.
- Every row has message-vector fields, parser rendering template, redaction policy, conformance fixture ID, and status.

## Runtime Shape Evidence

`sb_message_vector_error_surface_conformance` validates the current runtime surfaces:

- Parser-local message vectors include code, severity, message, component, and fields, and SBSQL rendering includes component/fields.
- Parser-support UDR missing-context refusal emits `UDR.SBSQL.CONTEXT_MISSING` with structured fields.
- Server diagnostics render JSON-line message vectors with code, message key, lowercase severity, safe message, and fields.
- SBPS message-vector payloads use `SBMV` binary framing, record CRCs, severity byte, message key, safe message, and TLV fields.
- Engine public ABI diagnostics expose stable C views with symbolic code, message key, severity, struct size, and ABI version.

## No Raw String-Only Diagnostic Gate

The paired `sbsql_no_raw_string_diagnostic_gate` label runs the same conformance probe. It verifies that parser, UDR, server, SBPS, and engine diagnostic paths used by this slice carry structured message-vector data rather than raw string-only error text.

## Result

FSPE-010A reports zero unregistered seed diagnostic rows under the current execution_plan backlog and validates parser rendering for the covered message-vector surfaces.
