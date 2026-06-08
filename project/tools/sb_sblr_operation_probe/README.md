# sb_sblr_operation_probe

`sb_sblr_operation_probe` verifies that a parser or command-registry SBLR operation resolves to exactly one supplied internal-operation catalog declaration.

It also enforces the ScratchBird architecture invariant that the engine executes SBLR/internal operations, not SQL text. Operation names or catalog rows that describe raw SQL execution as an engine operation fail closed.

## Command

```text
sb_sblr_operation_probe --profile <profile> --sblr-operation <operation> --catalog <logical-name=path> [--optional-catalog <logical-name=path>] --report <path>
```

The probe is intentionally format-tolerant while the final SBLR operation catalog syntax is being hardened. It recognizes declaration forms such as `sblr_operation: SBLR_SHOW_VERSION`, `sblr_opcode: ...`, `internal_operation: ...`, and Markdown table cells beginning with `SBLR_`, `SBOP_`, or `OP_`.
