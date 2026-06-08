# Fuzz And Fault Injection Matrix

Search key: `DRIVER_SERVER_FUZZ_FAULT_INJECTION`.

## Purpose

Exercise security and state-machine boundaries that normal route tests do not
stress.

## Required Families

| Family | Required coverage |
| --- | --- |
| Wire frame fuzz | Short frame, oversized frame, invalid length, invalid enum, duplicate field, invalid checksum or tag. |
| Connect key fuzz | Unknown key, duplicate key, conflicting alias, invalid redaction class, invalid type, overlong value. |
| Auth fault injection | Replay, token expiry, channel-binding mismatch, missing OS peer evidence, wrong verifier boundary, redaction leak. |
| TLS downgrade | Plaintext on TLS-required route, weak protocol request, certificate mismatch, hostname mismatch. |
| Session state races | Cancel during prepare, cancel during execute, reset during transaction, disconnect during stream, pool return during active cursor. |
| Stream and LOB faults | Truncated chunk, duplicate chunk, out-of-order chunk, chunk after close, backpressure violation. |
| MGA finality faults | Timeout before engine finality, retry after unknown finality, reconnect after prepared transaction, dormant reattach. |
| Adapter faults | Missing DSN key, unsupported host API option, invalid metadata request, adapter lifecycle interruption. |

## Closure Rule

`DSR-025` closes only when fault cases fail closed without crash, auth bypass,
undocumented route changes, or MGA finality drift.
