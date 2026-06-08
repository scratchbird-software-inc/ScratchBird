# Firebird Donor-Native Tool Regression Plan

Status: draft
Search key: `FIREBIRD_DONOR_NATIVE_TOOL_REGRESSION_PLAN`

## Purpose

Use original Firebird tools and original Firebird regression inputs as an external compatibility pressure test for ScratchBird Firebird emulation.

The donor tools are test-only artifacts. They must not become ScratchBird product dependencies.

## Initial Tool Candidates

| Tool surface | Firebird source area | Test purpose |
| --- | --- | --- |
| `isql` | `project/tests/donor_regression/donor_release_acquisition/firebird/5.0.4/source/src/isql` | SQL, PSQL, DDL, DML, metadata, result rendering, diagnostics. |
| `gbak` | `project/tests/donor_regression/donor_release_acquisition/firebird/5.0.4/source/src/burp` | Backup/restore service surface classification and non-file emulation or authority-violation diagnostics. |
| `gfix` / `alice` | `project/tests/donor_regression/donor_release_acquisition/firebird/5.0.4/source/src/alice` | Validation, repair, shutdown, sweep, and maintenance service classification. |
| `gstat` | `project/tests/donor_regression/donor_release_acquisition/firebird/5.0.4/source/src/utilities/gstat` | Statistics and metadata/report surface classification. |
| `nbackup` | `project/tests/donor_regression/donor_release_acquisition/firebird/5.0.4/source/src/utilities/nbackup` | Physical/incremental backup non-file emulation or authority-violation diagnostics. |
| `fbsvcmgr` | `project/tests/donor_regression/donor_release_acquisition/firebird/5.0.4/source/src/utilities/fbsvcmgr` | Service manager SPB/action response behavior. |
| `fbtracemgr` | `project/tests/donor_regression/donor_release_acquisition/firebird/5.0.4/source/src/utilities/fbtracemgr` | Trace service report and emulation behavior. |
| `gsec` | `project/tests/donor_regression/donor_release_acquisition/firebird/5.0.4/source/src/utilities/gsec` | Emulated user/role/security service behavior. |

## Regression Sources

The current release evidence packet has unresolved upstream regression roots:

`project/tests/donor_regression/donor_release_acquisition/firebird/5.0.4/regression/SOURCE_POINTERS.md`

FFPE-000D must locate/import the original Firebird QA/regression suite. Missing original regression roots are blockers and may not be closed until acquired. Firebird examples and SQL assets in the source tree may be used as secondary coverage but are not a substitute for the original regression suite.

## Classification

Every donor-native case must be classified as:

- `pass_exact`
- `pass_normalized`
- `emulated_expected`
- `authority_violation_expected`
- `invalid_input_expected`

## Required Outputs

- `FIREBIRD_DONOR_TOOL_BUILD_MANIFEST.csv`
- `FIREBIRD_DONOR_NATIVE_REGRESSION_MANIFEST.csv`
- `FIREBIRD_DONOR_NATIVE_REPLAY_REPORT.md`
- `FIREBIRD_DONOR_NATIVE_DIFF_ORACLE_REPORT.md`
- `FIREBIRD_DONOR_TOOL_RUNTIME_ISOLATION_REPORT.md`

## Closure Rule

The donor-native lane is incomplete while any donor-visible original regression case is unclassified, unexecuted, or missing an expected-result rule.
