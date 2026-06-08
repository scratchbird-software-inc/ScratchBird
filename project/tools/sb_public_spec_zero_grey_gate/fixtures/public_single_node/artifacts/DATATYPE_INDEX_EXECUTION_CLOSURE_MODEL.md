# Datatype And Index Execution Closure Model

Search key: `PUBLIC_SINGLE_NODE_DATATYPE_INDEX_EXECUTION_CLOSURE_MODEL`

Datatype and index execution closure requires testable behavior, not registry
rows alone.

Required behavior:

- Mandatory 128-bit numeric backend is used where the spec requires it.
- Casts and comparisons are canonical and profile-aware.
- Domain method binding is catalog-backed and fail-closed when absent.
- Datatype catalog and metrics rows are real conformance surfaces.
- Index DDL creates catalog descriptors and backing structures under MGA.
- Insert/update write optimization profiles preserve constraint and MGA
  visibility semantics.
- Index metrics are emitted and validated through `sys.metrics` surfaces.
