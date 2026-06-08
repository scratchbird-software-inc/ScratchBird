# Public/Private Boundary Matrix

Search key: `PUBLIC_P1_CLOUD_FOUNDATION_PUBLIC_PRIVATE_BOUNDARY_MATRIX`

Public single-node implementation must not expose or execute private cluster
semantics.

| Surface | Public behavior |
| --- | --- |
| Cluster CRDs | absent or deterministic `SB-K8S-CLUSTER-FIELD-REFUSED` |
| Read replica routing | refused in public single-node cloud/operator paths |
| Cluster route epochs/fence tokens | not accepted as public finality authority |
| Multi-directional replication | refused in public operator profiles |
| Cluster KMS partition policy | refused unless cluster build enables private scope |
| Cluster metrics roots | excluded from public closure evidence |
| Edge finality proofs from cluster | refused; local MGA commit finality only |

Tests must assert that public builds do not silently accept private cluster
fields or route through cluster transaction code.
