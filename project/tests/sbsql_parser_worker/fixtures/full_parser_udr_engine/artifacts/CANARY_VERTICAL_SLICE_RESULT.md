# Canary Vertical Slice Result

Status: complete
Search key: `FSPE-CANARY-VERTICAL-SLICE-RESULT`
Generated: 2026-05-07 20:57:59 EDT
Owning slice: `FSPE-001A`

## Result Summary

| Field | Value |
| --- | --- |
| Registry/linter artifact id | `project/src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.manifest` |
| Canary CTest label | `sbsql_canary_vertical_slice_gate` |
| Native statement result | `SBSQL-E4E0E6EB328C` / `create_table_stmt` resolved through generated registry, batch membership, oracle assignment, and handler coverage. |
| Expression/runtime result | `SBSQL-971C709406A0` / `@` resolved through generated registry, batch membership, oracle assignment, and handler coverage. |
| Cluster-private standalone fail-closed result | `SBSQL-39C545BEBF5A` / `cluster_publish_options` resolved to cluster parser gate, server admission gate, engine fail-closed/profile rule, and `diagnostic.cluster_profile_fail_closed`. |
| Native-now closure decision result | `SBSQL-DF502F8DF4FA` / `Accept` resolves as `native_now` through `parser.expression_runtime.function`, `lowering.expression_runtime.function`, and `engine.rule.sblr_expression_runtime_v3` with retained closure-action evidence. |
| Donor alias mapping/rendering result | `apache_ignite:query_select` maps to native `select` or exact diagnostic refusal with `donor_profile_message_vector_rendering`. |
| Message-vector evidence | `SERVER.ADMISSION.REFUSED` has rendering template, redaction policy, and fixture id `MSGV-SERVER-ADMISSION-REFUSED`. |
| Full-route smoke evidence | `sb_listener_sbp_sbsql_server_engine_execution_smoke` passed under the canary label. |
| Failure inventory rows | none |
| Closure decision | `FSPE-001A` complete; `FSPE-002` may open. |

## Validation

Commands run:

```text
cmake -S project -B build/sbsql_parser_worker_validation -DSB_BUILD_SBSQL_PARSER_WORKER=ON -DSB_BUILD_SBSQL_PARSER_WORKER_TESTS=ON -DSB_BUILD_SBU_SBSQL_PARSER_SUPPORT=ON
cmake --build build/sbsql_parser_worker_validation --target sbp_sbsql_canary_vertical_slice_probe
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_canary_vertical_slice_gate --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation --output-on-failure
```

Results:

- `sbsql_canary_vertical_slice_gate`: `2/2` passed.
- Focused SBSQL parser-worker validation shard: `17/17` passed.

## Boundary Notes

The canary proves the registry/linter/oracle/message-vector/donor/profile/full-route machinery works on representative rows. It does not implement broad lexer, CST/AST, expression, statement, binder, lowering, UDR, server, or engine closure; those remain owned by `FSPE-002` and later slices.
