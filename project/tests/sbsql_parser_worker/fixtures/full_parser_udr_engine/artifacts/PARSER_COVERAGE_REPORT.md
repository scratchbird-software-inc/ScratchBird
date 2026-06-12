# Parser Coverage Report

Status: complete
Search key: `FSPE-PARSER_COVERAGE_REPORT`

Owning slice: `FSPE-011`

## Scope

This report records the generated full-surface coverage matrix for FSPE-011.
The coverage gate is mechanical: it verifies generated registry rows and execution_plan
artifact rows agree with the canonical SBSQL matrices before later slices expand
durable fixture trees.

This report does not claim semantic execution for every parser, CST, AST,
BoundAST, SBLR, UDR, server, and engine route. Durable fixture execution remains
owned by FSPE-011A through FSPE-011F.

FSPE-014G later adds `sbsql_exhaustive_e2e_regression_gate`, which consumes the
FSPE-011F replay artifacts and makes registered-surface route coverage,
executable/refusal classification, expression datatype/function/operator
coverage, reference/engine backlog closure, and the dynamic UDR-to-SBLR procedure
path repeatable under CTest.

## Coverage Counts

| Coverage input | Covered rows |
| --- | ---: |
| Generated surface registry | 2,617 |
| `SBSQL_SURFACE_REGISTRY.csv` | 2,617 |
| `SBSQL_TO_SBLR_OPERATION_MATRIX.csv` | 2,617 |
| `SURFACE_IMPLEMENTATION_BACKLOG.csv` | 2,617 |
| `BATCH_ROW_MEMBERSHIP.csv` | 2,617 |
| `SEMANTIC_ORACLE_AUTHORITY_MAP.csv` | 2,617 |
| `SBSQL_ENGINE_GAP_MATRIX.csv` / `ENGINE_GAP_IMPLEMENTATION_BACKLOG.csv` | 932 |
| `REFERENCE_ALIAS_TO_SBSQL_SURFACE_MATRIX.csv` / `REFERENCE_ALIAS_COVERAGE_BACKLOG.csv` | 312 |
| `MESSAGE_VECTOR_COVERAGE_BACKLOG.csv` | 41 |
| `REGISTRY_FAMILY_BATCHING_PLAN.csv` | 77 batches |
| SBLR operation families | 19 |

## Gate Behavior

`sbsql_generated_full_surface_conformance` validates:

- Every generated registry row has non-empty parser, UDR, lowering, server-admission, engine-rule, diagnostic, oracle, fixture, final-acceptance, and closure-action metadata.
- Canonical surface, status, operation-matrix, backlog, batch-membership, and oracle rows match the generated registry by stable surface ID.
- Every operation row has the expected SBLR execution envelope plus required context, binding steps, result shape, and diagnostics.
- Every non-cluster engine gap is closed by an engine API/SBLR family gate and every cluster-private gap is closed by the cluster fail-closed gate.
- Every reference alias row maps to a native SBSQL surface or exact diagnostic through the reference alias backlog.
- Every message-vector row has concrete diagnostic code, rendering template, redaction policy, and conformance fixture metadata.

## Result

The FSPE-011 coverage gate passed with:

```text
FSPE-011 generated full-surface conformance passed: surfaces=2617 operation_rows=2617 operation_families=19 engine_gaps=932 reference_aliases=312 message_vectors=41 batches=77
```

Next slice: `FSPE-011A` reusable generated fixture sharding and regression test-bed policy.
