# SBLR Runtime Component Naming

Files ending in `*_runtime.cpp` or `*_runtime.hpp` are runtime support modules.
The suffix does not assert that the named operation is dispatched, logically
implemented, physically implemented, durable, or production-qualified. A
module may contain only request, descriptor, and result codecs.

Capability maturity is declared per operation in
`../internal_api/SBLR_API_OPERATION_MATRIX.yaml` and defined in
`../../../docs/public_api/IMPLEMENTATION_MATURITY.md`. Reviewers must follow the
matrix's execution path before treating a runtime module as operation evidence.

CREATE TABLE is the canonical example:

- `sblr_ddl_create_table_runtime.cpp` is a request/descriptor/result codec.
- `sblr_dispatch.cpp` routes the public `ddl.create_table` operation.
- `internal_api::EngineCreateTable` owns the catalog/MGA behavior in
  `../internal_api/ddl/create_api.cpp` and its included implementation units.

`engine.op.ddl_create_fdw` is the contrasting codec-only example. Its current
registry implementation is `sblr_ddl_create_fdw_runtime.cpp`, so the operation
matrix classifies it as `codec_contract` rather than inferring an FDW executor.

Codec-only tests prove serialization contracts. They do not prove routing,
state mutation, restart survival, or release qualification.
