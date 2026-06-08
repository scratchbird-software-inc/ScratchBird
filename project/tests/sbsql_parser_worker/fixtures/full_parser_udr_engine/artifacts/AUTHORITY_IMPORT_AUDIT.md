# Authority Import Audit

Status: complete
Search key: `FSPE-AUTHORITY-IMPORT-AUDIT`
Generated: 2026-05-07 20:32:37 EDT

## Finding

The execution_plan may use the SBSQL canonicalization CSV files as deterministic implementation-packet guidance, but they are not standalone product-behavior authority.

## Authority Chain

| Layer | Status | Use |
| --- | --- | --- |
| `public_contract_snapshot` | manifest authority rules | Defines that files are authoritative only when listed in `MANIFEST.yaml`. |
| `public_contract_snapshot` | controlling inventory | Lists parser-v3 canonical specs and the `sbsql-canonicalization/README.md` implementation packet. |
| `public_contract_snapshot` | canonical behavior | Normative parser, profile, diagnostics, UDR, SBLR, and conformance behavior when manifest-listed. |
| `public_input_snapshot` | implementation packet | Imports the CSV matrices as deterministic implementation guidance and traceability inputs. |
| Execution_Plan P0 artifacts | execution control | Convert matrix rows into implementation backlogs, batches, fixtures, and validation assignments. |

## Rules For Workers

- If a matrix row conflicts with a manifest-listed canonical contract, stop and record the contradiction; do not let the matrix silently win.
- Execution_Plans do not define product behavior. They define execution, traceability, acceptance, and evidence requirements.
- Implementation references in execution_plan artifacts must use file paths plus search keys or row ids, not line numbers.
- The old `ScratchBird` repository remains frozen reference evidence only unless explicitly targeted by a separate task.

## Acceptance

P1+ slices may consume generated P0 backlogs only as work assignment and traceability. Runtime behavior must still trace to manifest-listed canonical specs or explicit canonical refusal/profile policy.
