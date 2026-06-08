# Deterministic Refusal Matrix

Search key: `DRIVER_SERVER_DETERMINISTIC_REFUSAL`.

## Purpose

Prevent unsupported, guarded, not-applicable, or not-yet-implemented
driver/server behavior from closing as a silent skip, fake pass, generic error,
or parser-only shortcut.

The canonical authorities are:

| Authority | Search key |
| --- | --- |
| `public_contract_snapshot` | `DRIVER-CORE-CHECKLIST-COVERAGE-BINDING` |
| `public_contract_snapshot` | `DRIVER-NORMALIZED-VERIFICATION-CHECKLIST-2026-05-09` |
| `public_contract_snapshot` | `DRIVER-DETERMINISTIC-REFUSAL-SQLSTATE` |
| `public_contract_snapshot` | `diagnostic_shape_registry` |
| `public_contract_snapshot` | canonical message-vector contract |

## Required States

| State | Closure behavior | Runtime behavior |
| --- | --- | --- |
| `supported` | Row can close only with current spec, implementation, route, and test evidence. | Execute through the declared route and preserve engine finality evidence. |
| `fail_closed` | Required rows remain open/release-blocking unless a manifest-listed release-scope spec excludes them. | Refuse before side effects with deterministic diagnostic, SQLSTATE/native status, message vector, finality state, and retryability. |
| `conditional_n/a` | Allowed only for registry `conditional` rows with exact host API/runtime citation. | If a caller can still request the surface, return deterministic `FEATURE.CONDITIONAL_NOT_APPLICABLE`; otherwise no runtime surface is advertised. |
| `not_implemented` | Release-blocking unless explicitly outside the target release scope. | Refuse with `FEATURE.NOT_IMPLEMENTED_RELEASE_BLOCKING`; do not emulate through parser, donor, or hidden storage. |

## Required Refusal Fields

Every row whose state is not `supported` MUST define:

| Field | Requirement |
| --- | --- |
| `checklist_row_id` | Stable id from `driver-normalized-verification-checklist.yaml`. |
| `refusal_state` | One of `fail_closed`, `conditional_n/a`, or `not_implemented`. |
| `surface` | Driver, adapter, wire, parser IPC, manager/listener, or engine API surface where the refusal is observed. |
| `route` | Direct listener, manager proxy, local IPC, embedded, adapter, tool, or no-runtime-route. |
| `sqlstate` | SQLSTATE from `cancel-outcome-sqlstate.yaml` or ScratchBird-native status when SQLSTATE is not applicable. |
| `diagnostic_code` | Stable private diagnostic code. |
| `message_vector` | Canonical message-vector payload with the fields in the next section. |
| `finality_state` | `not_applicable`, `no_state_change`, `statement_aborted_transaction_state_reported_by_engine`, `unknown_until_engine_finality_report`, or `session_invalidated`. |
| `retryability` | `no`, `application_decision`, `idempotent_only_after_finality_query`, or `retry_after_capability_or_policy_change`. |
| `security_visibility` | Redaction class and public-safe message key. |
| `release_decision` | `closed`, `open_release_blocking`, or `excluded_by_manifest_listed_release_scope`. |
| `test_ref` | CTest name or planned CTest label. |

## Message Vector Minimum

Every deterministic refusal message vector MUST carry:

| Message-vector field | Required value |
| --- | --- |
| `canonical_source_kind` | `diagnostic_vector`. |
| `message_family` | `diagnostic`. |
| `severity` | `error` unless the row has no runtime route and is reported only in release evidence. |
| `required_outcome` | `reject_request`, `block_release_gate`, or `defer_until_capability_available`. |
| `retryability` | Same value as the refusal row. |
| `finality_state` | Same value as the refusal row. |
| `redaction_class` | `public_safe`, `policy_controlled`, or `security_controlled`. |
| `details` | Include `checklist_row_id`, `surface`, `route`, `diagnostic_code`, `sqlstate`, and `operator_action`. |

The parser boundary never owns the refusal decision. Parser workers receive only
the message vector or message-vector set emitted by the server/engine path.

## SQLSTATE And Finality Mapping

| Refusal condition | SQLSTATE/native status | Diagnostic code | Finality | Retryability |
| --- | --- | --- | --- | --- |
| Required surface intentionally fail-closed before execution | `0A000` | `FEATURE.REQUIRED_SURFACE_FAIL_CLOSED` | `no_state_change` | `retry_after_capability_or_policy_change` |
| Guarded implementation ahead of contract | `0A000` | `FEATURE.GUARDED_UNTIL_SPECIFIED` | `no_state_change` | `retry_after_capability_or_policy_change` |
| Conditional row not expressible by host API/runtime | `0A000` or no-runtime-route native evidence | `FEATURE.CONDITIONAL_NOT_APPLICABLE` | `not_applicable` | `no` |
| Not implemented in target release scope | `0A000` | `FEATURE.NOT_IMPLEMENTED_RELEASE_BLOCKING` | `no_state_change` | `retry_after_capability_or_policy_change` |
| Refusal after request admission where execution finality is unknown | Registry-specific SQLSTATE such as `57014` or `08006` | Outcome-specific code from `cancel-outcome-sqlstate.yaml` | `unknown_until_engine_finality_report` | `idempotent_only_after_finality_query` |

## Closure Rule

`DSR-019` closes only when refusal behavior is canonical spec authority and the
row-status gate rejects every required row without a deterministic `supported`
or `fail_closed` outcome. A `fail_closed` outcome is deterministic evidence, not
proof that a required checklist row is complete.
