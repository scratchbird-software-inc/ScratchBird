# sb_registry_snapshot_probe

`sb_registry_snapshot_probe` emits deterministic parser registry evidence JSON from one or more registry files.

It is a tooling probe only. It does not execute commands, validate engine authority, or dispatch SBLR.

## Command

```text
sb_registry_snapshot_probe --profile <profile> --registry <logical-name=path> [--optional-registry <logical-name=path>] --report <path>
```

Required registry files that cannot be read are errors. Optional registry files that cannot be read are warnings. The report includes extracted surface entries, diagnostics, and a deterministic snapshot hash.
