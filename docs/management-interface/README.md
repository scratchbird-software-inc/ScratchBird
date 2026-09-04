# ScratchBird management interface

`DPC_OPERATOR_UI_DOCS` identifies the product-facing management contract for
frontend developers and operators. This material describes stable management
surfaces, their authority boundaries, and the evidence that a client may show.
It does not make an administrative client an engine authority.

Use [MANAGEMENT_SURFACE_REFERENCE.md](MANAGEMENT_SURFACE_REFERENCE.md) for the
field and operation contract. Use [OPERATOR_WORKFLOWS.md](OPERATOR_WORKFLOWS.md)
for read, control, support-bundle, and index-management examples.

## Authority model

The `SELECT` examples are management adapter
projections. They expose engine-produced evidence; they do not reproduce
planning, catalog, security, resource, transaction, or recovery decisions in a
client. Engine MGA owns
visibility and transaction finality. The parser, reference adapter, client,
storage shortcuts, and WAL are not independent finality authorities.

ScratchBird users are always in a session transaction. Read-only management
queries and mutating controls therefore carry the current session, transaction,
snapshot, catalog, security, policy, and resource authority. A UI must preserve
the returned message vector and finality state rather than infer success from a
row count or transport acknowledgement.

The principal engine entry points are:

- `EngineInspectPerformanceOptimizationSurface`
- `EngineShowManagement`
- `EnginePrepareSupportBundle`
- `EngineIndexManagementOperation`

The principal result and route identities are
`rs.performance_optimization_surface.v1`, `observability.show_management`,
`performance_optimization_surface`, and
`index.management.route_surface.v1`.

## Cluster presentation

Every management control reports either `no-cluster` or
`cluster-enabled-stub` when cluster execution is unavailable. A cluster-owned
request must preserve the exact refusal `SBLR.CLUSTER.SUPPORT_NOT_ENABLED`; a
local fallback is not permitted.
