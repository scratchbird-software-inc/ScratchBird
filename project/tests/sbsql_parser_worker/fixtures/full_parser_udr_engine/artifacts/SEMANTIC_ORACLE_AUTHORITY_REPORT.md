# Semantic Oracle Authority Report

Status: complete
Search key: `FSPE-SEMANTIC_ORACLE_AUTHORITY_REPORT`

Owning slice: `FSPE-011C`

## Scope

This report records expected-result authority coverage for generated SBSQL
surface fixtures. The oracle map prevents generated tests from passing only
because test and implementation share the same wrong assumption.

Scope boundary:

- FSPE-011C proves authority and provenance for expected results.
- FSPE-011C does not execute every generated fixture.
- Differential replay remains owned by FSPE-011F.

## Coverage Counts

| Oracle input | Covered rows |
| --- | ---: |
| `SEMANTIC_ORACLE_AUTHORITY_MAP.csv` | 2,617 |
| `BATCH_ROW_MEMBERSHIP.csv` | 2,617 |
| `SURFACE_IMPLEMENTATION_BACKLOG.csv` | 2,617 |
| `SBSQL_TO_SBLR_OPERATION_MATRIX.csv` | 2,617 |

## Oracle Types

| Oracle type | Rows |
| --- | ---: |
| `canonical_spec_plus_sblr_matrix` | 1,988 |
| `promotion_or_canonical_refusal_decision` | 573 |
| `cluster_profile_and_standalone_refusal_policy` | 56 |

## Authority Rules

- Every oracle row has a stable `SBSQL-SURFACE-*` fixture ID.
- Every oracle row uses the stable `surface_id` as `source_search_key`.
- Every `oracle_source` points to `public_release_evidence`.
- Every `oracle_source` file exists after stripping any Markdown fragment.
- Every `source_search_key` resolves through the canonical surface registry and
  SBLR operation matrix.
- No oracle source points to `project/src`, `project/tests`, `build/`, `/tmp`, or generated implementation code.
- Every oracle row has a non-empty expected-result summary.
- Every oracle row reconciles with batch membership, surface backlog, and the SBLR operation matrix.
- Every oracle row status is `closed_by_semantic_oracle_authority_gate`.

## Validation

The FSPE-011C gate is:

```bash
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_semantic_oracle_authority_gate --output-on-failure
```

Observed validation:

- `sbsql_semantic_oracle_authority_gate`: 1/1 passed.
- Direct gate summary: `oracles=2617 canonical_spec=1988 promotion_or_refusal=573 cluster_profile=56`.
- `sbsql_parser_worker`: 26/26 passed.
- `p0_precode_validation.py --gate all`: all gates passed.
