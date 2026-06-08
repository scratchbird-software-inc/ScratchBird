# sb_parser_gen

Parser generator skeleton for the initial vertical slice.

Current scope:

- load registry inputs;
- compute deterministic registry snapshot hash;
- create generated artifact directory skeleton;
- write package manifest metadata.
- generate SBSQL parser-worker registry constants from the FSPE P0/P0L
  implementation artifacts with handler/refusal coverage assignments.

Controlling contracts:

- `public_contract_snapshot`
- `public_input_snapshot`

SBSQL registry generation:

```text
python3 project/tools/sb_parser_gen/generate_sbsql_registry.py
```

Generated outputs are written under
`project/src/parsers/sbsql_worker/registry/generated/`.
