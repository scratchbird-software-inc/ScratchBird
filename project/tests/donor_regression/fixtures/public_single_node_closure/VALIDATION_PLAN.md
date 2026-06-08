# Public Single-Node Closure Validation Plan

Search key: `PUBLIC-SINGLE-NODE-CLOSURE-VALIDATION`

## Required Validation Flow

1. Verify the target gap snapshot has 110 rows and no duplicate `gap_id`.

```sh
python3 - <<'PY'
import csv
from pathlib import Path
path = Path('project/tests/donor_regression/fixtures/public_single_node_closure/public_proof/artifacts/TARGET_GAPS.csv')
rows = list(csv.DictReader(path.open()))
ids = [row['gap_id'] for row in rows]
assert len(rows) == 110, len(rows)
assert len(ids) == len(set(ids)), 'duplicate target gap id'
print('target_rows=110')
PY
```

2. Verify agent and hardening artifacts exist before implementation starts.

```sh
python3 - <<'PY'
from pathlib import Path
root = Path('project/tests/donor_regression/fixtures/public_single_node_closure/public_proof')
required = [
  'artifacts/TARGET_GAPS.csv',
  'artifacts/TARGET_EVIDENCE_MANIFEST.csv',
  'artifacts/AGENT_WRITE_SCOPE_MATRIX.csv',
  'artifacts/AGENT_STATUS.csv',
  'artifacts/AI_BUDGET_CONTINGENCY.md',
  'artifacts/HARDENING_REQUIREMENTS.md',
  'artifacts/PARSER_PROFILE_CLOSURE_MODEL.md',
  'artifacts/STORAGE_FORMAT_AND_PROVIDER_POLICY.md',
  'artifacts/DATATYPE_INDEX_EXECUTION_CLOSURE_MODEL.md',
  'artifacts/SECURITY_AUTH_AUDIT_CLOSURE_MODEL.md',
  'artifacts/WIRE_DRIVER_OPERATIONAL_CLOSURE_MODEL.md',
  'artifacts/DONOR_REGRESSION_POLICY.md',
  'artifacts/FULL_ROUTE_ACCEPTANCE_FIXTURE.md',
]
missing = [name for name in required if not (root / name).exists()]
if missing:
    raise SystemExit('missing: ' + ', '.join(missing))
print('hardening artifacts present')
PY
```

3. Run the MGA policy gate before declaring any transaction-sensitive parser,
   storage, driver, donor, or benchmark path compliant.

```sh
python3 ${PUBLIC_TOOL_ROOT}/skills/scratchbird-mga-transaction-authority/scripts/mga_policy_gate.py --repo . project/src project/tests project/tests/donor_regression/fixtures/public_single_node_closure/public_proof
```

4. Configure and run the target CTest labels after implementation lands.

```sh
cmake -S project -B build -DSB_BUILD_PUBLIC_SPEC_ZERO_GREY_GATE=ON -DSB_BUILD_TESTS=ON
ctest --test-dir build -L public_single_node_closure --output-on-failure
ctest --test-dir build -R public_single_node_target_zero_grey_gate --output-on-failure
ctest --test-dir build -R public_single_node_non_target_regression_gate --output-on-failure
```

5. Run full-route and donor regression gates.

```sh
ctest --test-dir build -L public_single_node_full_route_gate --output-on-failure
ctest --test-dir build -L donor_original_regression_gate --output-on-failure
ctest --test-dir build -L driver_lane_ctest_gate --output-on-failure
```

## Negative Checks

- No target row may close without a passing CTest label.
- Donor tools may be compatibility oracles only; they must not execute storage
  or transaction finality for ScratchBird.
- Cloud-provider credentials are not required for local emulator gates, but the
  provider interface and fail-closed credential behavior must exist.
- Parser profiles must be install-independent. No parser may depend on another
  dialect being installed.
- Driver/adaptor/tool tests must use declared wire/IPC routes rather than direct
  in-process shortcuts unless the test is explicitly a unit test for that layer.
