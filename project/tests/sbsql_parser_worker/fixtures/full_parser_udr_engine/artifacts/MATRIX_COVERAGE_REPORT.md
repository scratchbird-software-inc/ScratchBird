# Matrix Coverage Report

Status: complete
Search key: `FSPE-MATRIX-COVERAGE-REPORT`
Generated: 2026-05-07 20:18:10 EDT

## Authority Note

Canonical behavior authority remains the manifest-listed parser-v3 contracts. The CSV matrices used here are implementation-packet inputs from the manifest-listed `public_input_snapshot` packet and are used as deterministic implementation guidance, not as independent normative behavior files.

## Frozen Inputs

| File | Rows | SHA-256 |
| --- | ---: | --- |
| `public_input_snapshot` | 2617 | `ce5ccae0a9bf9f0622a10ca5211a170b082a065dfae63d26358f3b56daee6dfa` |
| `public_input_snapshot` | 2617 | `4f33896ac4e129ca6f74df7aef16cd9ef2fdd501922d1221e67c3547a59c0cc4` |
| `public_input_snapshot` | 932 | `d5b1ff07f356f0170598ecc6cd201fed74bb777fd42b5ec241b7674a21e8c682` |
| `public_input_snapshot` | 312 | `f391f1c9192cc61312c5e54b7290eac6b2f3e696810161071fb16002428463b9` |

## Row Counts

- SBSQL surface rows: 2617.
- SBSQL to SBLR operation rows: 2617.
- Engine gap rows: 932.
- Reference alias rows: 312.

## Surface Status Counts

| Status | Rows |
| --- | ---: |
| `native_now` | 2580 |
| `cluster_private` | 37 |

## Cluster Scope Counts

| Matrix | Scope | Rows |
| --- | --- | ---: |
| surface | `noncluster_or_profile_scoped` | 2560 |
| surface | `cluster_private` | 57 |
| engine_gap | `noncluster_or_profile_scoped` | 816 |
| engine_gap | `cluster_private` | 116 |

## Engine Gap Type Counts

| Source gap type | Rows |
| --- | ---: |
| `gap` | 696 |
| `spec_silent` | 75 |
| `needs_deeper_read` | 51 |
| `deferred` | 49 |
| `user_verify` | 23 |
| `open_item` | 20 |
| `blocked` | 11 |
| `tbd` | 4 |
| `native_future` | 3 |

## Generated P0 Outputs

| Artifact | Rows |
| --- | ---: |
| `artifacts/SURFACE_IMPLEMENTATION_BACKLOG.csv` | 2617 |
| `artifacts/NATIVE_FUTURE_PROMOTION_AUDIT.csv` | 0 |
| `artifacts/ENGINE_GAP_IMPLEMENTATION_BACKLOG.csv` | 932 |
| `artifacts/REFERENCE_ALIAS_COVERAGE_BACKLOG.csv` | 312 |
| `artifacts/REGISTRY_FAMILY_BATCHING_PLAN.csv` | 77 |

## Coverage Result

Every row from each frozen input matrix has a generated backlog row. Surface rows are also assigned to exactly one registry-family batch.
