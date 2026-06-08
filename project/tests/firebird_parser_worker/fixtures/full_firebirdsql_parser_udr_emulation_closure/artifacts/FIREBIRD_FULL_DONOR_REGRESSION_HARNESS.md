# Firebird Full Donor Regression Harness

## Purpose

This artifact defines the repeatable Firebird QA replay harness required for
closure of the Firebird parser, UDR, bridge, and emulation execution_plan.

The harness uses the acquired Firebird QA candidate manifest as the controlling
inventory. Each candidate row is hashed, statically inspected, and either
replayed through the ScratchBird Firebird parser endpoint probe or recorded as a
gap row with preserved evidence.

## CTest Gates

- `fbqa_full_original_regression_inventory_gate` creates a complete row-by-row
  inventory for every acquired Firebird QA candidate and writes evidence files.
- `fbqa_full_original_regression_final_gate` uses the same harness in final
  mode and succeeds only when the failure inventory contains zero rows.

The CTest names intentionally avoid the fast `firebird_` name pattern so the
full donor QA replay does not silently enter smoke runs. The labels retain the
Firebird donor-native and original-regression families for explicit CTest label
selection.

## Evidence Outputs

Each run writes:

- `fbqa_full_original_regression_case_results.csv`
- `fbqa_full_original_regression_failure_inventory.csv`
- `FBQA_FULL_ORIGINAL_REGRESSION_SUMMARY.json`
- `FBQA_FULL_ORIGINAL_REGRESSION_REPLAY_REPORT.md`
- `raw/`, `normalized/`, and `scripts/` evidence directories

The failure inventory follows `OPERATIONAL_FAILURE_INVENTORY_POLICY.md` and can
be used as the burn-down list for parser, UDR, bridge, catalog, and emulation
work.

## Closure Rule

Firebird execution_plan closure requires the final gate to pass with all candidate
rows replayed or mapped to implemented, emulated, or diagnostic behavior and
with zero remaining failure inventory rows.

## Current Verification

On 2026-05-08 the inventory and final gates processed all `1,949` candidate rows. After
terminator-aware replay splitting, target-aware catalog mutation handling,
Firebird `q`-quoted string handling, frontend `CONNECT` classification, Python
driver SQL extraction, action-script extraction, and temporary SQL file
extraction, `isql` `IN` and `OUT` abbreviation handling, runtime
template-fragment detection, trace-only Python SQL capture, donor QA
skip/platform/version mark handling, SQL input-file tracing, and
parenthesized query expression classification, plus live runtime operation
replay for service attachment, backup/restore, trace, metadata, socket,
connection-info, donor utility, and environment surfaces, the result is:

- `1,949` parser-probe passes
- `0` failure inventory rows
- `0` parser-probe rejection rows
- `224` donor-marked no-op rows admitted as Firebird 5 Linux profile skips
- `74` live QA runtime rows replayed as implemented emulated operation scripts
- `58` trace-derived live runtime rows through `python_trace_live_runtime`
- `16` static live-marker rows through `live_runtime_emulation`

The final gate passes with zero failure inventory rows. Evidence is recorded in
`build/engine_listener_storage_release_gate/tests/firebird_parser_worker/fbqa_full_original_regression_final_gate/FBQA_FULL_ORIGINAL_REGRESSION_SUMMARY.json`.
