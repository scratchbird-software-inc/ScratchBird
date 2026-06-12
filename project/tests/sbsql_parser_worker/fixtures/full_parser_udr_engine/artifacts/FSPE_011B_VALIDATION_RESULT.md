# FSPE-011B Validation Result

Status: complete
Search key: `FSPE_011B_VALIDATION_RESULT`

## Scope

This artifact records validation evidence for FSPE-011B reference alias mapping and
reference-facing rendering coverage.

Scope boundary:

- FSPE-011B validates reference alias matrix/backlog/rendering fixture coverage and
  reference-profile rendering policy.
- FSPE-011B does not give reference SQL text, reference command text, parser AST, or
  BoundAST engine authority.
- Engine execution remains SBLR/internal API only.

## Validation Commands

Passed:

```bash
cmake --build build/sbsql_parser_worker_validation --target sbp_sbsql_reference_alias_rendering_conformance -j 4
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_reference_alias_rendering_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure
```

## Observed Results

- `sbsql_reference_alias_rendering_conformance`: 1/1 passed.
- `sbsql_reference_alias_mapping_conformance`: 1/1 passed through the same reference rendering gate.
- `sbsql_parser_worker`: 25/25 passed with the reference alias rendering gate included.
- Direct gate summary: `reference_aliases=312 reference_profiles=24 alias_kinds=13 fixture_policies=13`.

## Scope Boundary

This gate validates reference alias mapping/rendering contract shape, fixture-policy
coverage, exact diagnostic metadata, and engine-authority separation. It does
not claim real reference SQL parsing, catalog mutation, engine effects, or semantic
result execution for every alias row.

## Remaining FSPE-011B Closure Gaps

None. `FSPE-011C` is the next serialized execution_plan slice.
