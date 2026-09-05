# Tests

Implementation tests, conformance tests, compatibility tests, fault-injection
tests, performance tests, and fixtures live here.

## Behavioral test taxonomy

CTest names and labels describe behavior that the test itself executes:

- `fuzz`: generated or mutated inputs are executed against a target;
- `fault_injection`: a runtime fault is actually injected;
- `crash_reopen`: a process is terminated and durable state is reopened;
- `soak`: a workload executes for a sustained duration or iteration budget.

Tests that only read source files, require tokens, hash artifacts, or inventory
coverage use exactly one of `repository_policy`, `source_contract`, or
`evidence_inventory`, plus `source_token_check` and `non_behavioral`. They do
not carry `fuzz`, `fault_injection`, `crash_reopen`, `soak`, or sanitizer
qualification labels. Their filenames and output schemas must state that they
do not execute the stronger behavior. Directory placement indicates the
subject being inventoried, not proof that every file in that directory is
behavioral.

Evidence is reported in descending order of strength: runtime observable
behavior, durable/reopen verification, process-level verification,
model/property testing, static contract checks, then source-token checks. See
`../docs/testing/EVIDENCE_REPORTING.md` for the reporting and promotion rules.
