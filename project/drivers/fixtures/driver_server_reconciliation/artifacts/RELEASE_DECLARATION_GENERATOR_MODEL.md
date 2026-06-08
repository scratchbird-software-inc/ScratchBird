# Release Declaration Generator Model

Search key: `DRIVER_SERVER_RELEASE_DECLARATION_GENERATOR`.

## Purpose

Produce an honest machine-readable release declaration from the same evidence
used by the execution_plan gates.

## Required Inputs

- `artifacts/TARGET_CHECKLIST_ROWS.csv`.
- Per-lane driver and adapter row-status YAML files.
- `artifacts/IMPLEMENTATION_AHEAD_CLASSIFICATION.csv`.
- CTest result summary.
- Server-verification packet results.
- Full-route benchmark and performance-budget results.
- Fuzz and fault-injection results.
- Packaging and documentation sample results.
- `public_audit_summary`.
- Public zero-grey registry output.

## Required Output States

- `supported`.
- `fail_closed`.
- `conditional_n/a`.
- `not_implemented`.

## Closure Rule

`DSR-052` closes only when generated release declaration state matches the
tracker, inventory, row-status manifests, implementation-ahead register, and
CTest evidence with no grey or undocumented states.
