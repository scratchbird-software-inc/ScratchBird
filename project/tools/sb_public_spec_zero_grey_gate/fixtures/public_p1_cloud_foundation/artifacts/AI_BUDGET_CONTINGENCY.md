# AI Budget Contingency

Search key: `PUBLIC_P1_CLOUD_FOUNDATION_AI_BUDGET_CONTINGENCY`

If AI budget or runtime is interrupted, resume in this order:

1. Run `git status --short` and do not revert unrelated user changes.
2. Read `TRACKER.csv` and continue at the first non-completed slice.
3. Keep protected material before cloud identity/KMS because KMS references and
   secretless verifier state require protected-versioned material authority.
4. Keep cloud provider capability before operator and edge/CDN because provider
   support/refusal depends on the provider profile registry.
5. Before final closure, regenerate the registry with all three closure
   execution_plan overlays and stable `public_spec_gap_id_authority.csv`.

Required validation after resuming:

```sh
python3 project/tools/sb_public_spec_zero_grey_gate/public_spec_zero_grey_gate.py audit \
  --inventory public_audit_summary \
  --registry-json public_audit_summary

ctest --test-dir build --output-on-failure -R \
  'protected_material_catalog_gate|cloud_provider_capability_registry_gate|cloud_identity_kms_secretless_gate|kubernetes_operator_lifecycle_gate|edge_cache_cdn_invalidation_gate|public_p1_cloud_foundation_full_route_gate'
```
