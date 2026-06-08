# Final Audit

Search key: `PUBLIC_SINGLE_NODE_PARSER_STORAGE_DATATYPE_SECURITY_WIRE_DONOR_FINAL_AUDIT`

This audit closes the targeted public single-node execution_plan rows only. Public
rows outside `artifacts/TARGET_GAPS.csv` remain visible in the global registry
as non-target open work.

## Final Gate Evidence

- P1 through P6 phase evidence artifacts are present and referenced by
  `artifacts/TARGET_EVIDENCE_MANIFEST.csv`.
- `public_single_node_full_route_gate` selects four concrete route tests:
  SBSQL listener/parser/server execution, SBWP/TLS engine-auth route,
  database lifecycle full route, and MGA transaction full route.
- `public_single_node_target_zero_grey_gate` passes for all 110 target rows.
- `public_single_node_non_target_regression_gate` passes and reports 29
  remaining public open rows outside this execution_plan target set.
- Donor original-regression import evidence gates pass for the public donor
  profile batches and commercial capability-reference rows.
- MGA policy guard passed over the P6 donor closure files after donor evidence
  was added.

## Verification Commands

```text
ctest --test-dir build -L public_single_node_full_route_gate --output-on-failure
4/4 tests passed
```

```text
ctest --test-dir build -L "donor_original_regression_gate|public_single_node_target_zero_grey_gate|public_single_node_non_target_regression_gate" --output-on-failure
8/8 tests passed
```

```text
python3 project/tools/sb_public_spec_zero_grey_gate/public_spec_zero_grey_gate.py audit --inventory public_audit_summary --registry-json public_audit_summary
public_required=201 public_open=29
```

```text
python3 project/tools/sb_public_spec_zero_grey_gate/public_spec_zero_grey_gate.py target-gate --inventory public_audit_summary --registry-json public_audit_summary --execution_plan-root project/tests/donor_regression/fixtures/public_single_node_closure/public_proof
SB-PUBLIC-TARGET-ZERO-GREY-PASSED
```

## Result

The execution_plan target set is closed with evidence. The global public registry is
not fully closed because 29 public rows were deliberately outside this execution_plan
scope.
