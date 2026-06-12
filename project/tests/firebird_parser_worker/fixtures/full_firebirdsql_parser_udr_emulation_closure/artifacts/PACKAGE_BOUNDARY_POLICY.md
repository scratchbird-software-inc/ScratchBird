# FirebirdSQL Package Boundary Policy

Status: draft
Search key: `FIREBIRD_PACKAGE_BOUNDARY_POLICY`

## Rule

Firebird parser and UDR products may share Firebird-owned code so same-dialect behavior cannot diverge. They must not depend on SBSQL, PostgreSQL, MySQL, or any other reference parser/UDR package.

## Allowed

- `sbp_firebird` and `sbup_firebird` may link `sbl_firebird_dialect`.
- `sbp_firebird` and `sbup_firebird` may link `sbl_firebird_wire` when a Firebird wire/service surface is implemented.
- Firebird-owned common code may include lexer, CST, AST, binder, lowering, diagnostics, catalog overlay, datatype mapping, and wire/service descriptors.
- Test-only CTest harnesses may build Firebird reference tools from reference source.

## Forbidden

- Linking Firebird parser/UDR packages against `sbl_sbsql_parser_pipeline` or `sbu_sbsql_parser_support`.
- Linking ScratchBird runtime products against reference-native Firebird tools or Firebird client libraries.
- Wrapping `libfbclient`, `libfirebird`, or any third-party Firebird client library in runtime parser/UDR products.
- Allowing the listener, server, or engine to own Firebird grammar, tokenization, binding, lowering, or reference catalog mapping.
- Letting any reference package become required for ScratchBird core startup.

## Validation

The `firebird_package_boundary_gate` and `firebird_tool_runtime_isolation_gate` must inspect build targets, link libraries, source includes, and runtime product dependency graphs.
