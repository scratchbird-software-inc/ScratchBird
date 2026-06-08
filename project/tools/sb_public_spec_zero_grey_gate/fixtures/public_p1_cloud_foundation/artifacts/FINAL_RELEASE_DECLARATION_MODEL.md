# Final Release Declaration Model

Search key: `PUBLIC_P1_CLOUD_FOUNDATION_FINAL_RELEASE_DECLARATION_MODEL`

Final closure must emit:

- `artifacts/PUBLIC_P1_CLOUD_FOUNDATION_RELEASE_DECLARATION.json`
- `artifacts/PUBLIC_P1_CLOUD_FOUNDATION_RELEASE_DECLARATION.csv`

Required JSON fields:

- execution_plan search key
- generated timestamp
- source inventory SHA
- registry SHA
- target rows with gap ID, registry ID, title, status, CTest labels, evidence
  artifacts, implementation refs, and closure rule
- validation command list
- final public-open count, expected `24`
- remaining public-open rows outside this target set

The release declaration is evidence only. It does not replace canonical specs,
CTest gates, target evidence manifests, or the public gap registry.
