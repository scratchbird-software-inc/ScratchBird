# sb_fixture_manifest_probe

`sb_fixture_manifest_probe` creates deterministic fixture inventory evidence for parser conformance work.

The single-fixture linter validates one fixture. This probe walks a fixture root, classifies every YAML/JSON fixture, checks profile compatibility, and emits a stable manifest hash so conformance runs can prove exactly which fixtures were included.

## Command

```text
sb_fixture_manifest_probe --profile <profile> --fixture-root <path> --report <path>
```

## Classification

The probe infers fixture classes from path and content:

- `accepted`
- `refusal`
- `alias`
- `package_manifest`

Unknown fixture classes fail closed. Profile-specific fixture directories or `profile: ...` fields must match the requested profile, except that `private_cluster` may include `public_node` fixtures.
