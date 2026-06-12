# Known Risk Burn-Down Report

Status: complete
Search key: `FSPE-KNOWN_RISK_BURN_DOWN_REPORT`
Owning slice: `FSPE-014D`
Date: 2026-05-08

## Corrected Issues

| Issue | Closure evidence |
| --- | --- |
| FSPE-010B streaming scope was too broad for the parent slice | FSPE-010B1 through FSPE-010B9 were added and closed with child validation evidence. |
| Public ABI symbol gate concern | `sb_engine_public_abi_symbol_gate` was kept in scope through engine/public ABI validation evidence and not excluded from closure. |
| Security redaction side-channel leakage | FSPE-012G added diagnostic field filtering, hidden metadata projection, cache authority dimensions, and side-channel fixtures. |
| Upgrade compatibility gate was static-only at first | FSPE-012H added production database format/open compatibility evidence, canonical `FORMAT.VERSION_UNSUPPORTED`, decryption-required refusal, seed-pack policy checks, and CTest wiring. |
| Fixed UUID catalog seed mismatch | FSPE-013A reconciled engine standard function seed rows with canonical fixed UUID and name lookup packets. |
| Spec synchronization gate was missing | FSPE-013 added a deterministic parser-v3/SBSQL canonicalization synchronization audit. |
| Developer handoff map was header-only | FSPE-013B populated all 15 SBSQL surface families and added a map gate. |
| Source-size risk from generated seed data | FSPE-014A added source-size thresholds and recorded `function_seed_registry.cpp` as a deterministic seed-data exception. |
| Boundary appendix contained placeholder paths | FSPE-014B replaced the placeholder implementation, test, and evidence paths with current paths and added a zero-SQL gate. |
| Regression rerun process was not published | FSPE-014C published configure, build, CTest, audit, fixture, triage, retention, and cleanup guidance. |

## Remaining Non-Blocking Risks

| Risk | Rationale | Follow-up trigger |
| --- | --- | --- |
| Global reference exact-extraction manifest drift exists outside parser-v3/SBSQL closure scope | FSPE-013 scoped manifest validation to parser-v3 and SBSQL canonicalization authority because unrelated reference exact-extraction manifest entries are a broader contract-corpus maintenance concern. | Open a reference-manifest reconciliation execution_plan before claiming global reference exact-extraction release readiness. |
| Broad default `cmake --build ...` may include unrelated non-parser linkage targets | FSPE-014C publishes targeted parser-worker regression commands; the parser-worker label passes 39/39 in the validation build. | Resolve before declaring full repository all-target release build readiness. |
| `function_seed_registry.cpp` is a large deterministic seed-data file | FSPE-014A records it as a generated/seed-data exception with a 2.5 MiB cap; it is not handwritten algorithmic logic. | Split into generated data shards if it grows beyond the exception cap or if maintainability reviews require smaller seed files. |

## Closure Assertions

- No accepted SBSQL surface is hidden behind a residual risk.
- Zero open non-cluster engine gaps are masked by this report.
- No parser authority leak is accepted as a known risk.
- No missing message-vector path is accepted as a known risk.
- No WAL recovery or SQL-in-engine authority is accepted as a known risk.

## Runnable Gate

| Command | Result |
| --- | --- |
| `python3 project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/known_risk_burn_down_gate.py --repo-root .` | passed |

## Result

Known parser/UDR/server/engine closure risks are either corrected or documented as non-blocking with follow-up triggers. FSPE-014D is complete and FSPE-014E can open as the next serialized slice.
