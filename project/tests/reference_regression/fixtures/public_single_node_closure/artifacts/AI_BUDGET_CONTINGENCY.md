# AI Budget Contingency

Search key: `PUBLIC_SINGLE_NODE_AI_BUDGET_CONTINGENCY`

## Current State

P0, P1, P2, P3, and P4 are complete. P5+ implementation has not started.

The exact target set is frozen in `artifacts/TARGET_GAPS.csv` with 110 rows.
The tracker is `TRACKER.csv`. Agent heartbeat state is
`artifacts/AGENT_STATUS.csv`.

P0 validation passed:

- `public_single_node_target_evidence_manifest_gate`
- `public_single_node_hardening_gate`

The final target-zero-grey gate is registered and correctly fails closed while
later-phase target rows remain pending.

P1 validation passed:

- `parser_v3_management_syntax_gate`
- `parser_profile_authority_gate`
- `parser_v3_closure_gate`
- `sblr_operation_matrix_gate`
- `sblr_lowering_runtime_gate`
- `trusted_udr_bridge_gate`
- `parser_diagnostics_security_gate`
- `parser_conformance_oracle_gate`
- `parser_cache_invalidation_gate`
- `mga_policy_gate`

P2 validation passed:

- `storage_page_layout_gate`
- `filespace_lifecycle_gate`
- `cloud_storage_emulator_gate`
- `database_lifecycle_tx_gate`
- `foreign_filespace_quarantine_gate`
- `storage_metrics_gate`
- `mga_policy_gate`

P3 validation passed:

- `datatype_commercial_closure_gate`
- `numeric_128_backend_gate`
- `cast_comparison_gate`
- `domain_method_binding_gate`
- `index_catalog_ddl_gate`
- `index_family_matrix_gate`
- `insert_update_write_profile_gate`
- `index_metrics_gate`
- `mga_policy_gate`

P4 validation passed:

- `security_auth_gate`
- `authorization_privilege_gate`
- `audit_event_gate`
- `encryption_at_rest_gate`
- `security_policy_gate`
- `auth_plugin_manifest_gate`
- `tls_regression_gate`
- `mga_policy_gate`

## If AI Quota Runs Out

Resume from this order:

1. Open `README.md`, `TRACKER.csv`, `ACCEPTANCE_GATES.csv`, and
   `artifacts/AGENT_STATUS.csv`.
2. Verify `artifacts/TARGET_GAPS.csv` still has 110 rows.
3. Start with `PSDW-050` / P5 wire driver operational closure.
4. Preserve stable `SB-PUBLIC-GAP-*` IDs. Do not regenerate the registry in a
   way that renumbers open public gaps.

## Immediate Next Action

If the user says `continue` or `implement P5`, begin P5 only:

- Spawn wire/driver agents if agent capacity is available.
- Keep server lifecycle, listener/SBCT/parser-pool, local IPC/SBWP, driver
  package manifest, driver lanes, tools/adaptors, and routing/idempotency work
  separate.
- Update `AGENT_STATUS.csv` after each agent launch or five-minute heartbeat.

## Stop Conditions

Stop only for:

- Spec contradiction.
- Security authority contradiction.
- MGA authority contradiction.
- Reference regression requiring missing local source/toolchain decision.
- Merge conflict or user edit conflict in the same paths.
- Test failure that requires a design decision rather than a code correction.
