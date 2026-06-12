# sb_parser_gen

Parser generator skeleton for the initial vertical slice.

Current scope:

- load registry inputs;
- compute deterministic registry snapshot hash;
- create generated artifact directory skeleton;
- write package manifest metadata.
- generate SBSQL parser-worker registry constants from the FSPE P0/P0L
  implementation artifacts with handler/refusal coverage assignments.
- generate the SBsql language element manifest used by multilingual
  resource contract gates.

Controlling contracts:

- `public_contract_snapshot`
- `public_input_snapshot`

SBSQL registry generation:

```text
python3 project/tools/sb_parser_gen/generate_sbsql_registry.py
```

Generated outputs are written under
`project/src/parsers/sbsql_worker/registry/generated/`.

SBsql language element manifest generation:

```text
python3 project/tools/sb_parser_gen/generate_sbsql_language_element_manifest.py --repo-root .
```

Generated manifest outputs are written under
`project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts/`.
