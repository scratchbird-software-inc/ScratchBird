# Fault Injection Tests

Fault-injection test source belongs here and must not be compiled into
production behavior except through guarded diagnostic builds.

Only tests that inject a runtime fault may carry the `fault_injection` CTest
label. `public_crash_fault_source_contract_matrix.py` is a source-contract
coverage inventory and deliberately carries only `source_contract`,
`coverage_inventory`, and `evidence_gate` taxonomy labels.
