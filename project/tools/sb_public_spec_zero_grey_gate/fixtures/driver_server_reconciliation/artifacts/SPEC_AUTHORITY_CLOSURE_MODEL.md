# Spec Authority Closure Model

Search key: `DRIVER-SERVER-RECONCILIATION-SPEC-AUTHORITY-CLOSURE-MODEL`.

## Purpose

P1 closes contract authority before broad implementation work continues.
The goal is not prose coverage; the goal is implementable authority for every
driver checklist row that depends on server, wire, parser, security, metadata,
datatype, transaction, or diagnostics behavior.

## Required Spec Families

| Family | Required authority |
| --- | --- |
| Driver minimum | `appendix-driver-normalized-verification-minimum.md` plus `registries/driver-normalized-verification-checklist.yaml`. |
| Connect/session | Connect key registry, startup packet, server info, parameter status, ReadyForQuery, disconnect, ping/pong, reset, reauth, cancel, state notifications. |
| Auth/security | Auth method registry, provider registry, verifier-boundary status, support state, PEER OS evidence, MFA, token refresh, redaction, channel binding. |
| Parameter/result | ParameterDataPacket, ParameterDescription, RowDescription, metadata bitmap, generated keys discriminator, OUT/INOUT return path. |
| Type encoding | CanonicalTypeId values stitched into native wire execution representation with byte layout and null/empty distinction. |
| Execution | MultiResultEnvelope, batch, pipeline, array bind, copy/bulk reject events, LOB locator/chunk protocol, cursor behavior. |
| Transactions | MGA autocommit mapping, reset, reconnect, cancel, savepoint, 2PC/XA, limbo, dormant detach/reattach, finality query. |
| Diagnostics | CancelOutcomeToSqlstate, timeout/network/cancel distinction, MessageVector field mapping, localized message behavior. |
| Metadata | `sys.information` projections, ODBC/JDBC/.NET metadata fields, type/function/reserved-word lists, grants and case rules. |
| Observability | W3C trace context, client/app identity, per-parameter redaction map, driver/server timing and metrics surfaces. |

## Closure Rule

A spec family is complete only when:

- The controlling file is under `public_release_evidence`.
- The file is listed in `MANIFEST.yaml`.
- It contains stable search keys for implementation audit.
- It defines invalid-state behavior and diagnostics.
- It maps to checklist row ids.
- It preserves engine auth/authorization and MGA finality authority.

## Explicitly Forbidden Closure

- Citing `public_audit_output` as behavior authority.
- Citing implementation line numbers instead of stable search keys.
- Saying a capability is "driver-only" when server behavior is required.
- Allowing implementation-ahead behavior to remain undocumented because tests
  happen to pass.
