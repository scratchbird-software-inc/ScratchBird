# AI Budget Contingency

Search key: `DRIVER-SERVER-RECONCILIATION-AI-BUDGET-CONTINGENCY`.

## Resume Rule

If execution is interrupted, resume from the first `pending` or `in_progress`
row in `TRACKER.csv`. Do not infer completion from code presence. Completion
requires the artifact listed in `outputs` plus the acceptance text for that row.

## Current Checkpoint

P0 execution_plan creation artifacts exist or are being finalized:

- `artifacts/TARGET_CHECKLIST_ROWS.csv`
- `artifacts/TARGET_EVIDENCE_MANIFEST.csv`
- `artifacts/IMPLEMENTATION_AHEAD_CLASSIFICATION.csv`
- `artifacts/AGENT_WRITE_SCOPE_MATRIX.csv`
- `artifacts/AGENT_STATUS.csv`
- `artifacts/AI_BUDGET_CONTINGENCY.md`
- `artifacts/HARDENING_REQUIREMENTS.md`
- `artifacts/SPEC_AUTHORITY_CLOSURE_MODEL.md`
- `artifacts/WIRE_SESSION_SPEC_CLOSURE_MODEL.md`
- `artifacts/SECURITY_AUTH_RECONCILIATION_MODEL.md`
- `artifacts/DATATYPE_RESULT_METADATA_CLOSURE_MODEL.md`
- `artifacts/DRIVER_LANE_EVIDENCE_MODEL.md`
- `artifacts/FULL_ROUTE_DRIVER_ACCEPTANCE_FIXTURE.md`

## Fast Validation Commands

```bash
python3 project/tools/sb_public_spec_zero_grey_gate/public_spec_zero_grey_gate.py driver-checklist \
  --registry public_contract_snapshot \
  --target-rows project/drivers/fixtures/driver_server_reconciliation/public_proof/artifacts/TARGET_CHECKLIST_ROWS.csv
```

```bash
ctest --test-dir build --output-on-failure -R driver_server_reconciliation_checklist_structure_gate
```

```bash
ctest --test-dir build --output-on-failure -R database_lifecycle_security_auth_audit_p4_conformance
```

## Low-Budget Execution Strategy

1. Do not start broad implementation sweeps until P1 spec authority artifacts are
   complete.
2. Prefer one agent per disjoint write scope from `AGENT_WRITE_SCOPE_MATRIX.csv`.
3. Update `AGENT_STATUS.csv` after each agent handoff and at five-minute
   heartbeat intervals.
4. Prioritize high-risk drift first: PEER/ident, multi-resultset, auth provider
   registry, SBLR operation matrix, ParameterDescription, and bulk reject events.
5. If only one final action is possible, run the checklist structure gate and
   record the first failing tracker row.

## Do Not Do

- Do not mark any checklist row closed without CTest or lane-native evidence.
- Do not let driver code rely on implementation-ahead behavior without spec
  authority.
- Do not replace MGA finality with driver inference, parser state, donor tools,
  or WAL-style assumptions.
- Do not collapse driver/adaptor/tool lanes into one generic pass; every lane
  needs row-status evidence.
