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
coverage use `source_contract`, `coverage_inventory`, and `evidence_gate`
instead. Their filenames and output schemas must state that they do not execute
the stronger behavior. Directory placement indicates the subject being
inventoried, not proof that every file in that directory is behavioral.
