# Public Release Foundation Validation Plan

Search key: `PUBLIC-RELEASE-FOUNDATION-VALIDATION`

## Required Validation Flow

1. Regenerate the public gap registry from the implementation inventory.

```sh
python3 project/tools/sb_public_spec_zero_grey_gate/public_spec_zero_grey_gate.py write-registry \
  --inventory public_audit_summary \
  --closure-execution_plan-root project/tools/sb_public_spec_zero_grey_gate/fixtures/public_release_foundation/public_proof \
  --closure-execution_plan-root project/tests/donor_regression/fixtures/public_single_node_closure/public_proof \
  --gap-id-authority public_audit_summary \
  --out-json public_audit_summary \
  --out-csv public_audit_summary
```

2. Verify registry freshness.

```sh
python3 project/tools/sb_public_spec_zero_grey_gate/public_spec_zero_grey_gate.py audit \
  --inventory public_audit_summary \
  --registry-json public_audit_summary
```

2a. Verify stable gap IDs and closed-execution_plan preservation.

```sh
python3 project/tools/sb_public_spec_zero_grey_gate/public_spec_zero_grey_gate.py gap-id-authority \
  --registry-json public_audit_summary \
  --authority-csv public_audit_summary

python3 project/tools/sb_public_spec_zero_grey_gate/public_spec_zero_grey_gate.py closure-regression \
  --registry-json public_audit_summary \
  --closed-execution_plan-root project/tools/sb_public_spec_zero_grey_gate/fixtures/public_release_foundation/public_proof \
  --closed-execution_plan-root project/tests/donor_regression/fixtures/public_single_node_closure/public_proof \
  --max-public-open 29
```

3. Verify the target evidence manifest and hardening artifacts.

```sh
python3 - <<'PY'
import csv
from pathlib import Path
root = Path('project/tools/sb_public_spec_zero_grey_gate/fixtures/public_release_foundation/public_proof')
required = [
    'artifacts/TARGET_EVIDENCE_MANIFEST.csv',
    'artifacts/PREFLIGHT_BASELINE_INVENTORY.csv',
    'artifacts/PERSISTENT_FORMAT_MIGRATION_POLICY.md',
    'artifacts/FAULT_INJECTION_MATRIX.csv',
    'artifacts/CATALOG_PHYSICAL_INDEX_PROFILE.md',
    'artifacts/INFORMATION_PROJECTION_NAMING_DECISION.md',
    'artifacts/SYNONYM_OBJECT_SEMANTICS.md',
    'artifacts/TLS_FIXTURE_POLICY.md',
    'artifacts/CONSTRAINT_INDEX_DEPENDENCY_POLICY.md',
    'artifacts/FULL_ROUTE_ACCEPTANCE_FIXTURE.md',
    'artifacts/AGENT_WRITE_SCOPE_MATRIX.csv',
    'artifacts/AGENT_STATUS.csv',
]
missing = [path for path in required if not (root / path).exists()]
if missing:
    raise SystemExit('missing hardening artifacts: ' + ', '.join(missing))
with (root / 'artifacts/TARGET_EVIDENCE_MANIFEST.csv').open(newline='') as f:
    rows = list(csv.DictReader(f))
if len(rows) != 20:
    raise SystemExit(f'expected 20 target evidence rows, found {len(rows)}')
with (root / 'artifacts/AGENT_STATUS.csv').open(newline='') as f:
    agent_columns = list(csv.DictReader(f).fieldnames or [])
required_agent_columns = [
    'timestamp_utc',
    'agent',
    'phase',
    'current_slice',
    'status',
    'blocked_by',
    'last_update',
    'next_action',
    'evidence_refs',
]
if agent_columns != required_agent_columns:
    raise SystemExit(f'agent status schema mismatch: {agent_columns!r}')
print('public release foundation hardening artifacts present')
PY
```

4. Verify canonical SQL object synonym contract authority before
   implementation begins.

```sh
python3 -c "from pathlib import Path; manifest=Path('public_contract_snapshot').read_text(); required='chapters/catalog-schema/appendix-sql-object-synonym-semantics.md'; assert required in manifest, 'missing synonym spec from manifest'; assert Path('public_contract_snapshot').exists(); print('canonical synonym spec authority present')"
```

5. Verify the P4 MGA recovery proof model exists before recovery work starts.

```sh
python3 -c "from pathlib import Path; path=Path('project/tools/sb_public_spec_zero_grey_gate/fixtures/public_release_foundation/public_proof/artifacts/MGA_RECOVERY_PROOF_MODEL.md'); text=path.read_text(); required=['PUBLIC_RELEASE_FOUNDATION_MGA_RECOVERY_PROOF_MODEL','must not introduce WAL','PRF-041 must not start until PRF-040']; [(_ for _ in ()).throw(SystemExit('missing recovery proof text: '+needle)) for needle in required if needle not in text]; print('MGA recovery proof model present')"
```

6. Configure target release-gate tests.

```sh
cmake -S project -B build -DSB_BUILD_PUBLIC_SPEC_ZERO_GREY_GATE=ON
```

7. Run the target-set gates.

```sh
ctest --test-dir build -L public_release_foundation --output-on-failure
```

8. Run the public zero-grey sync gate.

```sh
ctest --test-dir build -R public_spec_gap_registry_sync_gate --output-on-failure
```

9. Run the target-only zero-grey gate. It must pass before this execution_plan can
   close.

```sh
ctest --test-dir build -R public_release_foundation_target_zero_grey_gate --output-on-failure
```

10. Run the full public zero-grey release gate and confirm that any remaining
   failures are outside this execution_plan target set. A full public gate failure is
   allowed at this stage only if `global_public_zero_grey_non_target_regression_audit`
   proves all remaining failures are non-target gaps.

```sh
ctest --test-dir build -R public_spec_zero_grey_release_gate --output-on-failure
ctest --test-dir build -R global_public_zero_grey_non_target_regression_audit --output-on-failure
```

## Mandatory Evidence

- Target gap registry with all target rows closed.
- Release gate records and conformance manifest records for every target gap.
- Target evidence manifest with passing CTest label and evidence references for
  every target gap.
- Canonical SQL object synonym contract listed in `MANIFEST.yaml` before
  synonym implementation gates run.
- Preflight baseline inventory proving the implementation did not start from an
  unknown state.
- Persistent-format migration policy for every new durable structure.
- Fault-injection matrix and passing fault gates.
- SQL object synonym semantics proving synonyms are first-class objects with
  target references bounded dereference parent remap and cycle diagnostics.
- Agent write-scope matrix and heartbeat log for any agent-managed execution.
- MGA recovery proof model proving recovery authority remains MGA-only before P4
  recovery implementation begins.
- CTest output proving every mandatory label exists and passes.
- Updated implementation inventory showing the target gaps as Implemented in
  Full.
- Updated public gap registry JSON and CSV.
- Final audit proving no target gap is still partial, drift, or not implemented.

## Negative Checks

- Introducing a WAL or redo-log recovery authority fails validation.
- Marking a gap closed without a passing CTest gate fails validation.
- Leaving TLS in config-only form fails validation.
- Creating catalog tables with human-readable name columns as authority fails
  validation.
- Exposing raw UUIDs from user-facing catalog views where the spec forbids them
  fails validation.
- Treating `sys.information_schema` as the canonical information projection
  authority fails validation; it must be only a legacy synonym resolving to the
  same final schema UUID as `sys.information`.
- Implementing SQL object synonyms as extra name rows fails validation.
- Creating children under a synonym object's UUID instead of the final target
  parent UUID fails validation.
- Following synonym chains beyond five hops or missing a cycle fails validation.
- Enforcing constraints outside MGA visibility and rollback rules fails
  validation.
- Accepting backup, delta, or PITR input without coverage proof fails
  validation.
- Closing this execution_plan while the target-only zero-grey gate fails is forbidden.
- Treating the full public zero-grey gate as required to pass for out-of-scope
  public gaps is forbidden; use the non-target regression audit instead.
