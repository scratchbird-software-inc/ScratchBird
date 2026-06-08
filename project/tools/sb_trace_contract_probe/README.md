# sb_trace_contract_probe

`sb_trace_contract_probe` checks that generated tooling evidence contains required trace fields and does not leak forbidden terms for the selected profile.

It is intentionally generic because parser, registry, fixture, package, and conformance tooling all need the same traceability rule: every evidence artifact must say what profile it belongs to, what snapshot or manifest it used, and what object it is proving.

## Command

```text
sb_trace_contract_probe check --profile <profile> --evidence <path> --contract <name> --report <path> [--required-field <field>] [--forbidden-term <term>]
```

## Built-in contracts

The probe adds required fields for known contracts:

- `registry_snapshot`
- `command_surface`
- `package_gate`
- `fixture_manifest`
- `parse_vertical_slice`

Under `public_node`, it also rejects private cluster authority terms.

The `parse_vertical_slice` contract now requires engine API bridge evidence so accepted parser fixtures prove the command reached the engine-owned dispatch request boundary rather than stopping at parser-synthesized lowering output.
