# sb_result_shape_probe

`sb_result_shape_probe` verifies that a parser or command-registry result shape resolves to exactly one supplied catalog declaration.

The probe is intentionally format-tolerant while the final result-shape catalog syntax is being hardened. It recognizes declaration forms such as `result_shape: name`, `shape_key: name`, `output_shape: name`, `shape: name`, and simple Markdown table cells that look like shape identifiers.

## Command

```text
sb_result_shape_probe --profile <profile> --result-shape <shape> --catalog <logical-name=path> [--optional-catalog <logical-name=path>] --report <path>
```

The probe fails closed when the requested result shape is missing or declared more than once.
