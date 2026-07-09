# Firebird QA Source Pointers

Status: public acquisition pointer for local reference-regression replay.

- Reference project: FirebirdSQL Firebird QA
- Reference release: `v0.22.2`
- Source repository: `https://github.com/FirebirdSQL/firebird-qa.git`
- Recorded upstream HEAD: `84d137de5cfdc59ecf392b22db15f4d014a5a150`
- Local acquisition script: `project/tests/reference_regression/acquire_reference_regression_assets.py`
- Acquisition manifest: `project/tests/reference_regression/reference_regression_acquisition_sources.csv`
- Candidate index: `FIREBIRD_QA_CANDIDATE_TEST_INDEX.csv`
- Python test files: `1949`

The upstream regression payload is intentionally not tracked in the public
repository. Install it locally with the acquisition script before running the
original Firebird QA replay gates.
