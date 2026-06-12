# FSPE-006 Validation Result

Status: complete
Search key: `FSPE-006-BINDER-AUTHORITY-CONFORMANCE-VALIDATION`
Generated: 2026-05-07 23:34:47 EDT
Owning slice: `FSPE-006`

## Implemented Outputs

- Expanded `BoundStatement` into an authority-safe BoundAST evidence carrier with parser package, registry snapshot, session/database, epoch, surface, SBLR operation, result-shape, diagnostic-shape, resource-contract, rights, profile/edition gate, trace, descriptor/policy, and authority-step metadata.
- Updated `BindAst` to consume FSPE-005 AST statement metadata while preserving parser-only syntax evidence boundaries.
- Added fail-closed gates for unauthenticated binding, public name-resolution requirements, missing server resolver endpoint, unresolved server resolver evidence, cluster-private profile authority, and exact-refusal surfaces.
- Added generated-style binder authority conformance probe at `project/tests/sbsql_parser_worker/generated/binder/sbsql_binder_authority_conformance_probe.cpp`.
- CTest label: `sbsql_binder_authority_conformance`.

## Coverage

The conformance probe validates:

- BoundAST header fields preserve parser API/protocol version, parser package UUID/version/build ID, registry snapshot, catalog/security/descriptor epochs, and source-derived trace evidence;
- parser statement surface metadata survives AST-to-boundary handoff without granting parser-owned catalog, security, transaction, cluster, storage, execution, or UUID authority;
- parser-visible object-bearing statements fail closed unless public server resolver evidence is supplied;
- resolved UUID evidence is accepted only when supplied as external resolver input;
- unauthenticated statements fail closed while preserving syntax evidence metadata;
- security statements carry required server security authority steps but do not make parser-side rights decisions;
- transaction statements carry required server transaction authority steps but do not perform transaction semantics;
- cluster-private statements fail closed without cluster profile authority;
- descriptor, policy, rights, profile/edition, resource, result-shape, diagnostic-shape, conformance, and trace metadata are explicit for later server admission and SBLR lowering slices.

## Validation

Commands run:

```text
cmake -S project -B build/sbsql_parser_worker_validation
cmake --build build/sbsql_parser_worker_validation --target sbp_sbsql_binder_authority_conformance_probe -j 4
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_binder_authority_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation --output-on-failure
```

Results:

- `sbsql_binder_authority_conformance`: `1/1` passed.
- Focused SBSQL parser-worker validation shard: `22/22` passed.

## Boundary Notes

FSPE-006 closes only the parser binder boundary. Closure means syntax-only AST from FSPE-005 is transformed into authority-safe BoundAST evidence using server/engine-owned catalog, name-resolution, descriptor, policy, security/right/scope, transaction, cluster-proof, UDR, acceleration, and diagnostic authority keys. BoundAST may carry validated external UUID evidence, descriptor references, policy references, required-rights metadata, transaction/resource metadata, result-shape metadata, diagnostic shape, conformance case, and trace evidence required for server admission.

FSPE-006 does not make the engine a SQL parser, does not execute SQL text, does not perform verifier-admitted SBLR lowering, does not execute engine behavior, and does not grant parser-side security authority. Names remain user/parser-layer syntax until resolved through public server/engine authority; UUID/name authority is server/engine-owned. The binder must not infer hidden-vs-missing object existence beyond safe public diagnostics/message vectors.

ScratchBird MGA remains the controlling transaction, visibility, versioning, recovery, cleanup, retention, archive, and cluster reconciliation model. No WAL or reference recovery authority is introduced by this binder slice.
