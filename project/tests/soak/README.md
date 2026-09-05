# Soak Tests and Coverage Inventories

The `soak` CTest label is reserved for tests that execute a workload for a
sustained duration or iteration budget.

`public_release_soak_coverage_inventory.py` does not execute the referenced
workloads. It inventories their expected budgets and evidence contracts and is
registered under `source_contract`, `coverage_inventory`, and `evidence_gate`
instead.
