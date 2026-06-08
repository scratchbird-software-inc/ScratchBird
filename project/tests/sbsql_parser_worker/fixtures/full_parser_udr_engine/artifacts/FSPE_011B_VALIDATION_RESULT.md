# FSPE-011B Validation Result

Status: complete
Search key: `FSPE_011B_VALIDATION_RESULT`

## Scope

This artifact records validation evidence for FSPE-011B donor alias mapping and
donor-facing rendering coverage.

Scope boundary:

- FSPE-011B validates donor alias matrix/backlog/rendering fixture coverage and
  donor-profile rendering policy.
- FSPE-011B does not give donor SQL text, donor command text, parser AST, or
  BoundAST engine authority.
- Engine execution remains SBLR/internal API only.

## Validation Commands

Passed:

```bash
cmake --build build/sbsql_parser_worker_validation --target sbp_sbsql_donor_alias_rendering_conformance -j 4
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_donor_alias_rendering_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure
```

## Observed Results

- `sbsql_donor_alias_rendering_conformance`: 1/1 passed.
- `sbsql_donor_alias_mapping_conformance`: 1/1 passed through the same donor rendering gate.
- `sbsql_parser_worker`: 25/25 passed with the donor alias rendering gate included.
- Direct gate summary: `donor_aliases=312 donor_profiles=24 alias_kinds=13 fixture_policies=13`.

## Scope Boundary

This gate validates donor alias mapping/rendering contract shape, fixture-policy
coverage, exact diagnostic metadata, and engine-authority separation. It does
not claim real donor SQL parsing, catalog mutation, engine effects, or semantic
result execution for every alias row.

## Remaining FSPE-011B Closure Gaps

None. `FSPE-011C` is the next serialized execution_plan slice.
