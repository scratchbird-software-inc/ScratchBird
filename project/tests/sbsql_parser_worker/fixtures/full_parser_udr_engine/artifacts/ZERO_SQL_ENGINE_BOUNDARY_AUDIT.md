# Zero SQL Engine Boundary Audit

Status: complete
Search key: `FSPE-ZERO_SQL_ENGINE_BOUNDARY_AUDIT`
Owning slice: `FSPE-014B`
Date: 2026-05-08

## Boundary Rule

ScratchBird engine execution remains SBLR/internal-procedure only. SQL text, donor syntax, parser ASTs, wire protocol frames, and unresolved names must terminate in parser, parser-support UDR, server admission, or API bridge layers before engine execution.

Final invariant wording:

- Engine executes SBLR/internal procedures only.
- No raw SQL text is admitted as engine execution authority.
- MGA remains Alpha recovery authority.

## Evidence

| Boundary | Evidence |
| --- | --- |
| Server admission rejects raw SQL text | `project/src/server/sblr_admission.cpp` checks `ContainsSqlTextMarker` and refuses with `SBLR.SQL_TEXT_FORBIDDEN` / `raw_sql_forbidden`. |
| Server admission requires UUID-resolved envelopes | `project/src/server/sblr_admission.cpp` requires `parser_resolved_names_to_uuids=true` and current `SBLRExecutionEnvelope.v3`. |
| Engine internal API refuses SQL text | `project/src/engine/internal_api/engine_internal_api.cpp` rejects `envelope.contains_sql_text` with `SB-ENGINE-API-SQL-TEXT-NOT-ACCEPTED`. |
| Engine internal API does not trust parser authority | `project/src/engine/internal_api/engine_internal_api.cpp` retains parser-not-trusted validation and UUID-resolution requirements. |
| Engine SBLR envelope rejects SQL text | `project/src/engine/sblr/sblr_engine_envelope.cpp` rejects `contains_sql_text` with SBLR SQL-text diagnostics. |
| Parser-support UDR parses and lowers only | `project/src/udr/sbu_sbsql_parser_support/sbu_sbsql_parser_support.cpp` builds CST/AST/BoundAST, lowers to SBLR, verifies the envelope, and does not dispatch engine operations or open databases. |
| Acceleration/import APIs fail closed | Engine acceleration and import helpers detect `raw_sql` or `sql_text` tokens and reject them rather than executing them. |
| Canonical boundary spec updated | `public_contract_snapshot` now points to current implementation, test, and evidence paths. |

## Runnable Gate

| Command | Result |
| --- | --- |
| `python3 project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/zero_sql_engine_boundary_gate.py --repo-root .` | passed |

## Result

No SQL execution API token is present in `project/src/engine`, and the required server, UDR, SBLR, and engine refusal evidence is present. FSPE-014B is complete and FSPE-014C can open as the next serialized slice.
