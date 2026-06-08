# sb_diagnostic_catalog_probe

`sb_diagnostic_catalog_probe` verifies that a parser, binder, lowerer, or package diagnostic code resolves to exactly one supplied diagnostic catalog declaration.

It is intentionally format-tolerant while the final diagnostic catalog syntax is still being hardened. It recognizes declaration forms such as `diagnostic_code: SB-...`, `code: SB-...`, comma/table rows that begin with `SB-...`, and Markdown table rows containing `SB-...`.

## Command

```text
sb_diagnostic_catalog_probe --profile <profile> --diagnostic-code <code> --catalog <logical-name=path> [--optional-catalog <logical-name=path>] --report <path>
```

The probe fails closed when the requested diagnostic code is missing or declared more than once.
