# P6 Donor Compatibility Closure Evidence

Search key: `PUBLIC_SINGLE_NODE_P6_DONOR_COMPATIBILITY_CLOSURE_EVIDENCE`

P6 closes `SB-PUBLIC-GAP-0089` through `SB-PUBLIC-GAP-0128` for the
single-node public release target set.

## Implementation Evidence

- `project/src/parsers/donor/DonorCompatibilityProfileManifest.csv` defines 28
  canonical donor rows: 25 runtime donor-emulation families and 3 commercial
  capability-reference families.
- `project/src/udr/packages/donor/DonorUdrBridgePolicyManifest.csv` defines the
  matching donor UDR bridge policy rows and keeps transaction, security,
  storage, and recovery authority owned by the ScratchBird engine.
- `docs/reference/donor_catalog_seeds/actual_per_family_seed_manifest_index.yaml`
  indexes 25 runtime donor catalog seed manifests and excludes commercial
  capability-reference families from runtime seed use.
- Eleven P6 seed manifests were added as profile-derived private manifests for
  the remaining public donor families that did not already have actual private
  seed manifests.
- Donor registry rollups now include every runtime donor family across catalog
  seed, datatype/version, diagnostic rendering, wire/API, and upstream
  regression import contracts.
- SQL Server, Oracle, and DB2 are represented only as capability-reference
  refusal/diagnostic surfaces. They do not provide hidden parser, runtime seed,
  inbound wire, storage, recovery, transaction, or security authority.
- `project/tests/donor_regression/donor_compatibility_gate.py` is wired into
  CTest through `project/tests/donor_regression/CMakeLists.txt` and validates
  profile, seed, surface, runtime bridge, family batch, capability-reference,
  original regression import evidence, and target-evidence wiring.

## Verification

Direct P6 donor compatibility gate sweep:

```text
core: passed
seed: passed
surface: passed
runtime: passed
relational: passed
analytic: passed
nosql: passed
distributed: passed
capref: passed
original-regression: passed
```

Observed inventory:

```text
Donor profile rows: 28
Donor UDR bridge policy rows: 28
Runtime donor seed families: 25
Profile-derived P6 seed manifests: 11
Capability-reference rows: 3
```

CTest, target-evidence, MGA policy, and audit-registry verification are recorded
in the phase tracker after the final P6 sweep.
