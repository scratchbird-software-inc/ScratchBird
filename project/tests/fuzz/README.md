# Fuzz Tests and Coverage Inventories

The `fuzz` CTest label is reserved for tests that generate or mutate inputs and
execute those inputs against a target.

`public_durable_codec_fuzz_coverage_inventory.py` does not execute mutated
inputs. It inventories source and property-test coverage for durable codecs and
is registered under `source_contract`, `coverage_inventory`, and
`evidence_gate` instead.
