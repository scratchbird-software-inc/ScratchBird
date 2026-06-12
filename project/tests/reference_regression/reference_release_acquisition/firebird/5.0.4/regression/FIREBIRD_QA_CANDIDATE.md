# Firebird QA Candidate Regression Root

Status: candidate indexed for reference replay scaffold
Search key: `FIREBIRD_5_0_4_QA_CANDIDATE_REGRESSION_ROOT`

## Candidate

- Path: `in_tree_reference_test_asset`
- HEAD: `84d137de5cfdc59ecf392b22db15f4d014a5a150`
- Working tree: clean at inspection time
- Test root: `in_tree_reference_test_asset`
- Python test files: `1949`
- License file: `in_tree_reference_test_asset`
- License SHA-256: `613c085f268577fad6124fde112339d0f80f51cf3b2f0fc7dae06bfd9fbafca6`
- README SHA-256: `cfaefa28f7333dacf0f8831fe1414e819bdb36472e1f1b94c1be708d9e96629e`
- Test index: `FIREBIRD_QA_CANDIDATE_TEST_INDEX.csv`
- Reference replay manifest: `FIREBIRD_QA_REFERENCE_REPLAY_MANIFEST.csv`
- Reference replay family manifest: `FIREBIRD_QA_REFERENCE_REPLAY_FAMILY_MANIFEST.csv`
- Candidate asset hash manifest: `FIREBIRD_QA_CANDIDATE_ASSET_HASH_MANIFEST.csv`

## Fit

The candidate appears to be the modern Firebird QA suite used by Firebird source
CI. It contains a pytest plugin, test databases, files, backups, configs, and
engine tests.

## Replay Scaffold

The replay scaffold keeps every candidate test in scope and records each test as
`candidate_indexed_pending_replay` under
`firebird_original_regression_replay_gate`. The family manifest groups the
candidate rows by top-level path and replay family:

- `tests/bugs/core`: `1481`
- `tests/bugs/gh`: `436`
- `tests/functional/gtcs`: `1`
- `tests/functional/intfunc/unlist`: `8`
- `tests/functional/sqlancer`: `22`
- `tests/functional/tabloid`: `1`

The asset hash manifest records `171` candidate non-Python support files used by
the replay scaffold, including backup, data, database, config, and support
files.
