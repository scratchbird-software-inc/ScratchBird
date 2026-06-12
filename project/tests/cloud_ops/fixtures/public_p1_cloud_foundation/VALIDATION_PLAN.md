# Public P1 Protected Material and Cloud-Ops Foundation Validation Plan

Search key: `PUBLIC-P1-PROTECTED-MATERIAL-CLOUD-OPS-FOUNDATION-VALIDATION`

## P0 Validation

```sh
python3 project/tools/sb_public_spec_zero_grey_gate/public_spec_zero_grey_gate.py audit \
  --inventory public_audit_summary \
  --registry-json public_audit_summary

python3 project/tools/sb_public_spec_zero_grey_gate/public_spec_zero_grey_gate.py gap-id-authority \
  --registry-json public_audit_summary \
  --authority-csv public_audit_summary

python3 project/tools/sb_public_spec_zero_grey_gate/public_spec_zero_grey_gate.py closure-regression \
  --registry-json public_audit_summary \
  --closed-execution_plan-root project/tools/sb_public_spec_zero_grey_gate/fixtures/public_release_foundation/public_proof \
  --closed-execution_plan-root project/tests/reference_regression/fixtures/public_single_node_closure/public_proof \
  --max-public-open 29
```

## Implementation Validation

Each implementation phase must add CTest registrations for its gate labels:

```text
protected_material_catalog_gate
cloud_provider_capability_registry_gate
cloud_identity_kms_secretless_gate
kubernetes_operator_lifecycle_gate
edge_cache_cdn_invalidation_gate
public_p1_cloud_foundation_full_route_gate
```

Minimum required CTest flow:

```sh
cmake -S project -B build -DSB_BUILD_PUBLIC_SPEC_ZERO_GREY_GATE=ON
ctest --test-dir build --output-on-failure -R \
  'protected_material_catalog_gate|cloud_provider_capability_registry_gate|cloud_identity_kms_secretless_gate|kubernetes_operator_lifecycle_gate|edge_cache_cdn_invalidation_gate|public_p1_cloud_foundation_full_route_gate'
```

## Registry Closure Validation

After all five targets are implemented, regenerate the registry with all closure
overlays:

```sh
python3 project/tools/sb_public_spec_zero_grey_gate/public_spec_zero_grey_gate.py write-registry \
  --inventory public_audit_summary \
  --closure-execution_plan-root project/tools/sb_public_spec_zero_grey_gate/fixtures/public_release_foundation/public_proof \
  --closure-execution_plan-root project/tests/reference_regression/fixtures/public_single_node_closure/public_proof \
  --closure-execution_plan-root project/tests/cloud_ops/fixtures/public_p1_cloud_foundation/public_proof \
  --gap-id-authority public_audit_summary \
  --out-json public_audit_summary \
  --out-csv public_audit_summary
```

The final registry must report:

```text
public_spec_gap_registry total=221 public_required=201 public_open=24
status.implemented_in_full=177
status.not_implemented=0
status.partial=24
status.private=20
```

Then run:

```sh
ctest --test-dir build --output-on-failure -R \
  'public_spec_gap_registry_sync_gate|public_spec_gap_id_authority_gate|public_spec_closed_execution_plan_regression_gate'
```

The closure regression maximum must be updated to `24` only after this execution_plan
is fully closed and an intentional commit records the new baseline.
