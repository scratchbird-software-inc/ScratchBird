# P1 Parser/Profile Closure Evidence

Status: completed
Date: 2026-05-10

P1 closes the parser/profile/SBLR substrate for `SB-PUBLIC-GAP-0040` through
`SB-PUBLIC-GAP-0060` by adding or validating:

- deterministic psql/tool meta-command surface records and exact refusals;
- Native V3 management and public-profile cluster-private classification;
- lossless CST source-buffer metadata required by the AST/bound-AST route;
- expanded parser cache keys and invalidation hooks for name-resolution,
  resource, parser-package, disclosure, security-authority, cluster-policy,
  TTL, memory-pressure, connection, and transaction-context changes;
- static SBLR operation-matrix coverage against engine dispatch and trusted
  parser-support UDR evidence.

Passing gates:

- `ctest --test-dir build -L "parser_v3_management_syntax_gate|parser_profile_authority_gate|parser_v3_closure_gate|sblr_operation_matrix_gate|sblr_lowering_runtime_gate|trusted_udr_bridge_gate|parser_diagnostics_security_gate|parser_conformance_oracle_gate|parser_cache_invalidation_gate" --output-on-failure`
- `${PUBLIC_TOOL_ROOT}/skills/scratchbird-mga-transaction-authority/scripts/mga_policy_gate.py --repo . project/src/parsers project/src/engine/sblr project/src/udr project/tests/sbsql_parser_worker project/tests/donor_regression/fixtures/public_single_node_closure/public_proof`
- `python3 -m py_compile project/tests/sbsql_parser_worker/generated/matrix/sbsql_sblr_operation_matrix_static_gate.py`
- `git diff --check -- project/src/parsers/sbsql_worker project/tests/sbsql_parser_worker project/tests/donor_regression/fixtures/public_single_node_closure/public_proof`

CTest result:

```text
100% tests passed, 0 tests failed out of 10
```
