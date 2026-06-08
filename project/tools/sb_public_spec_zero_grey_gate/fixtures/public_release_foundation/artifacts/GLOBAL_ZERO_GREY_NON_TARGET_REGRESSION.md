# Global Zero-Grey Non-Target Regression Audit

Search key: `PUBLIC_RELEASE_FOUNDATION_GLOBAL_ZERO_GREY_NON_TARGET_REGRESSION`

## Result

`global_public_zero_grey_non_target_regression_audit` passed after all 20 target
rows were synchronized to `implemented_in_full` in the public gap registry.

The full `public_spec_zero_grey_release_gate` still fails with 138 open public
release-required entries. That failure is expected at this stage because those
open rows are outside the public release foundation target set.

## Evidence

Passed:

```sh
ctest --test-dir build -R "public_spec_gap_registry_sync_gate|public_release_foundation_target_evidence_manifest_gate|release_gate_record_conformance|public_release_foundation_target_zero_grey_gate|global_public_zero_grey_non_target_regression_audit" --output-on-failure
```

Expected failure:

```sh
ctest --test-dir build -R public_spec_zero_grey_release_gate --output-on-failure
```

The expected failure reported:

- Public release-required entries: 201
- Public open entries: 138
- Implemented in full entries: 63
- Private entries: 20

## Regression Decision

No target gap remains open. The remaining open public gaps are non-target rows
reserved for subsequent execution-plans. This audit therefore satisfies
`PRF_G11_GLOBAL_NON_TARGET_REGRESSION_CLEAN`.
