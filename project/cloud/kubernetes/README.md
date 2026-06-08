# ScratchBird Kubernetes Public Single-Node Operator Assets

Search key: `SB_K8S_PUBLIC_SINGLE_NODE_OPERATOR_ASSETS`

This directory contains the public single-node Kubernetes operator contract,
CRDs, sample manifests, dry-run validation, and deterministic reconciliation
gate assets for PCF-040 and PCF-041.

The assets intentionally do not require a Kubernetes cluster. The local gate
validates CRD schemas, rejects private cluster fields, builds dry-run lifecycle
plans, and proves that repeated reconcile calls produce the same evidence
reference.

Run:

```sh
python3 project/tests/cloud_ops/kubernetes_operator_lifecycle_gate.py --repo-root .
```

Public single-node status emitted by these assets is orchestration evidence
only. ScratchBird manager/server policy and MGA transaction inventory remain
the authority for lifecycle success, transaction finality, durability, and
recovery.
