# sb_package_gate

`sb_package_gate` is the deterministic public/private package boundary gate for generated and staged ScratchBird artifacts.

It is intentionally standalone. It does not execute parser code, engine code, SBLR, or cluster authority. It classifies a package artifact list and rejects anything that cannot be shipped by the requested profile.

## Command

```text
sb_package_gate check --profile <public_node|private_cluster|dev_only|test_only> --manifest <path> --artifact-list <path> --report <path>
```

The public profile explicitly permits the public engine internal API and parser engine bridge artifacts, but still rejects private cluster authority terms, trusted-parser evidence, and private cluster source or manager artifacts.

## Artifact list format

The artifact list is newline-oriented:

```text
project/src/core/catalog/README.md
public_tool,project/tools/sb_registry_lint/main.cpp
```

If a class is declared before the comma, the gate verifies that the declared class matches the inferred class. Unknown artifacts fail closed.

## Public-node rule

`public_node` rejects private contracts, private cluster source, private managers, private configs, private tests, private docs, enterprise deployment artifacts, generated-private artifacts, build artifacts, and unknown files.
