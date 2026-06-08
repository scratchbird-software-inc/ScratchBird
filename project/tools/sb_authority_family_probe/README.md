# sb_authority_family_probe

`sb_authority_family_probe` verifies that a command-registry authority family resolves to exactly one supplied authority/permission catalog declaration.

The probe is intentionally format-tolerant while the final authority catalog syntax is being hardened. It recognizes declaration forms such as `authority_family: OBS_METRICS_READ_FAMILY`, `permission_family: ...`, `grant_target: ...`, `right_family: ...`, and Markdown table cells that look like authority family identifiers.

## Command

```text
sb_authority_family_probe --profile <profile> --authority-family <family> --catalog <logical-name=path> [--optional-catalog <logical-name=path>] --report <path>
```

The probe fails closed when the requested authority family is missing, declared more than once, or exposes private cluster authority under the `public_node` profile.
