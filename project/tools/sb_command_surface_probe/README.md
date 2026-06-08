# sb_command_surface_probe

`sb_command_surface_probe` verifies that a parser command surface lookup resolves to exactly one registry entry.

It is used by early parser/conformance tooling to catch missing command surfaces, duplicate search keys, and ambiguous operation mappings before parser packages are generated.

## Command

```text
sb_command_surface_probe --profile <profile> --lookup-key <surface-or-search-or-operation-key> --registry <logical-name=path> --report <path>
```

The probe loads registry files through the shared registry snapshot library, then matches the lookup key against `surface_key`, `search_key`, and `operation_key`.
