# Firebird Operational Failure Inventory Policy

Status: draft
Search key: `FIREBIRD_OPERATIONAL_FAILURE_INVENTORY_POLICY`

## Rule

Full and donor-native runs collect all failures before debugging. The harness must preserve enough evidence to rerun and triage every failing case.

## Required Failure Record Fields

- CTest name and label set.
- Firebird surface row id.
- Donor tool name and arguments when applicable.
- ScratchBird endpoint and profile.
- Raw stdout and stderr paths.
- Normalized output path.
- Exit status and signal.
- Status vector and canonical diagnostic vector.
- Expected classification.
- Actual classification.
- Rerun command.
- Cleanup status.

## Required Gate

`firebird_operational_failure_inventory_gate` fails if any failed CTest lacks a complete failure record.
