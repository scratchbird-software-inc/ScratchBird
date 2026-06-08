# Source Size Maintainability Report

Status: complete
Search key: `FSPE-SOURCE_SIZE_MAINTAINABILITY_REPORT`
Owning slice: `FSPE-014A`
Date: 2026-05-08

## Policy

FSPE-014A enforces source-size limits for parser, UDR, server, wire IPC, and engine implementation files so closure does not create unmaintainable monoliths.

| Source class | Limit | Rule |
| --- | --- | --- |
| Handwritten implementation C/C++ files | 128 KiB | Any non-generated non-exception source above this limit fails the gate. |
| Generated implementation files under `generated/` | 3 MiB | Generated registry and generated fixture sources are allowed larger deterministic data payloads. |
| Deterministic seed-data exception | 2.5 MiB | `project/src/engine/functions/registry/function_seed_registry.cpp` is allowed as FSPE-013A canonical seed data, not handwritten algorithmic logic. |

## Largest Sources

| Size bytes | Path | Classification |
| --- | --- | --- |
| 2559201 | `project/src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.cpp` | generated registry |
| 2262145 | `project/src/engine/functions/registry/function_seed_registry.cpp` | deterministic seed-data exception |
| 62250 | `project/src/server/ipc_server.cpp` | handwritten |
| 57575 | `project/src/server/sblr_dispatch_server.cpp` | handwritten |
| 56514 | `project/src/engine/internal_api/backup_archive/backup_archive_api.cpp` | handwritten |
| 53535 | `project/src/engine/sblr/sblr_dispatch.cpp` | handwritten |
| 51829 | `project/src/server/ipc_tester.cpp` | handwritten |
| 43620 | `project/src/engine/internal_api/mga_relation_store/mga_relation_store.cpp` | handwritten |
| 43113 | `project/src/parsers/sbsql_worker/ipc/sbps_client.cpp` | handwritten |
| 38377 | `project/src/engine/public_abi.cpp` | handwritten |

## Runnable Gate

| Command | Result |
| --- | --- |
| `python3 project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/source_size_maintainability_gate.py --repo-root .` | passed |

## Result

No non-exception handwritten parser, UDR, server, wire IPC, or engine source file exceeds the FSPE-014A threshold. FSPE-014B can open as the next serialized slice.
