# sb_conformance_plan_probe

`sb_conformance_plan_probe` creates deterministic parser conformance run-plan evidence from a fixture manifest and a registry snapshot.

It does not execute parser code. It binds fixture inventory to registry snapshot evidence so a future runner can prove which fixtures were planned against which command surface.

Accepted parser command fixtures plan against the `engine_api_bridge` stage. This keeps conformance aligned with the engine-owned internal API boundary rather than parser-only lowering output.

## Command

```text
sb_conformance_plan_probe create --profile <profile> --fixture-manifest <path> --registry-snapshot <path> --out <path>
```

The plan fails closed when the fixture manifest or registry snapshot is missing required evidence fields, when no fixtures are available, or when public-node inputs contain private cluster authority terms.
