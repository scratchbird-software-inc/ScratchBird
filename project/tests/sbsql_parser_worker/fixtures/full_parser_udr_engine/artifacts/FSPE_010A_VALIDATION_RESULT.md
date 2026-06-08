# FSPE-010A Validation Result

Status: passed_with_shared_symbol_gate_caveat
Search key: `FSPE-010A-MESSAGE-VECTOR-ERROR-SURFACE-VALIDATION`

## Implemented Scope

- Materialized `sb_message_vector_error_surface_conformance` as a runnable CTest label.
- Added the paired `sbsql_no_raw_string_diagnostic_gate` label to the same conformance executable.
- Validated the message-vector backlog for non-empty diagnostic codes, message-vector fields, parser rendering templates, redaction policy, conformance fixture IDs, unique codes, required origins, required code prefixes, and required subsystems. The backlog now contains 41 rows after FSPE-012B malicious-input diagnostic additions.
- Extended SBSQL parser diagnostic rendering to include structured component and field evidence instead of only severity/code/message text.
- Aligned parser-support UDR missing-context refusals to `UDR.SBSQL.CONTEXT_MISSING` with structured `udr_function`, `required_context_fields`, and `operation_uuid` fields.
- Validated parser JSON/rendering, UDR JSON refusal shape, server JSON-line shape, SBPS binary `SBMV` shape and CRCs, and public engine ABI diagnostic views.

## Validation Commands

```bash
cmake -S project -B build/sbsql_parser_worker_validation
cmake --build build/sbsql_parser_worker_validation --target sb_message_vector_error_surface_conformance
ctest --test-dir build/sbsql_parser_worker_validation -L sb_message_vector_error_surface_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_no_raw_string_diagnostic_gate --output-on-failure
cmake --build build/sbsql_parser_worker_validation --target sbu_sbsql_parser_support_probe
ctest --test-dir build/sbsql_parser_worker_validation -L sbu_sbsql_parser_support_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -E sb_engine_public_abi_symbol_gate --output-on-failure
```

## Results

- `sb_message_vector_error_surface_conformance`: passed `1/1`.
- `sbsql_no_raw_string_diagnostic_gate`: passed `1/1`.
- `sbu_sbsql_parser_support_conformance`: passed `1/1`.
- `sbsql_parser_worker` focused shard: passed `13/13`.
- Focused validation shard excluding `sb_engine_public_abi_symbol_gate`: passed `25/25`.

## Validation Caveat

The unrestricted default build target attempted to build `sb_engine_shared` and failed while linking non-PIC static engine objects into `libsb_engine.so`:

```text
relocation R_X86_64_PC32 against symbol ... can not be used when making a shared object; recompile with -fPIC
```

Because `libsb_engine.so` was not produced, the unrelated `sb_engine_public_abi_symbol_gate` test failed with:

```text
nm: '${PROJECT_ROOT}/build/sbsql_parser_worker_validation/libsb_engine.so': No such file
```

This is outside the FSPE-010A diagnostic/message-vector labels and outside the `sbsql_parser_worker` shard. It remains a build infrastructure issue for the shared engine target; FSPE-010A validation passed on the runnable diagnostic/message-vector gates.

## Boundary Evidence

- The new conformance probe is in `project/tests/sbsql_parser_worker/generated/diagnostics/sbsql_message_vector_error_surface_conformance.cpp`.
- The CTest label wiring is in `project/tests/sbsql_parser_worker/CMakeLists.txt`.
- Parser rendering still renders message vectors; it does not acquire diagnostic authority.
- Server and engine remain diagnostic producers; the parser renders structured message vectors.
- UDR parser support remains fail-closed under missing engine context and does not execute SQL or mutate engine state.
