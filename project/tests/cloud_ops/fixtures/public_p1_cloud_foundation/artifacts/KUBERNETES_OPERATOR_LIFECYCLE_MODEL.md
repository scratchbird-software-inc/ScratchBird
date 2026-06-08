# Kubernetes Operator Lifecycle Model

Search key: `PUBLIC_P1_KUBERNETES_OPERATOR_LIFECYCLE_MODEL`

The implementation must add public single-node operator assets and tests without
activating private cluster behavior.

Required CRDs:

- `ScratchBirdDatabase`
- `ScratchBirdFilespace`
- `ScratchBirdBackup`
- `ScratchBirdRestore`
- `ScratchBirdCloudProviderProfile`
- `ScratchBirdKmsProfile`
- `ScratchBirdRoutePolicy`
- `ScratchBirdSupportBundle`
- `ScratchBirdUpgrade`

Cluster-oriented CRDs or fields must be either absent from public builds or
present only with deterministic fail-closed diagnostics.

Required behavior:

- CRD schemas validate with checked-in tooling.
- Dry-run reconciliation emits planned lifecycle operations and evidence IDs.
- Reconciliation is idempotent.
- Operator status is evidence, not engine finality.
- Engine/database lifecycle commands still pass through manager/listener/server
  policy and engine authority.
