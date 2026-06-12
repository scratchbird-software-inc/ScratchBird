# FirebirdSQL Execution_Plan Artifacts

Status: draft
Search key: `FIREBIRDSQL_EXECUTION_PLAN_ARTIFACTS`

This directory holds execution_plan execution artifacts for the FirebirdSQL parser, parser-support UDR, emulation, reference-native regression, and CTest closure effort.

The files currently present define required implementation artifacts and gates. Expanded Firebird surface matrices are required P0 outputs.

Firebird QA reference replay scaffold artifacts:

- `FIREBIRD_QA_REFERENCE_REPLAY_MANIFEST.csv`: `1949` candidate-indexed tests grouped by top-level path and replay family.
- `FIREBIRD_QA_REFERENCE_REPLAY_FAMILY_MANIFEST.csv`: `6` replay families covering `tests/bugs` and `tests/functional`.
- `FIREBIRD_QA_CANDIDATE_ASSET_HASH_MANIFEST.csv`: `171` candidate data, config, backup, database, and support-file hashes.

Required generated artifacts are listed in `FIREBIRD_REQUIRED_P0_ARTIFACTS.csv`.

Agent-managed execution uses `AGENT_IMPLEMENTATION_ORCHESTRATION_POLICY.md` and `AGENT_EXECUTION_STATUS.csv` to track slice ownership, five-minute refreshes, test cadence, blocker state, and escalation evidence once implementation starts.
