# ScratchBird Prisma Adapter (Deterministic Scaffold)

This package provides deterministic building blocks for ECOSYS-401:

- ScratchBird datasource URL guardrails for Prisma-style configs
- ScratchBird-to-Prisma scalar/native type mapping helpers
- Metadata-to-`schema.prisma` model generation utility for introspection-style flows
- Deterministic reflection round-trip contract helper
- Deterministic migration plan builder for Prisma migration layout

## Run tests

```bash
cd lanes/active/adapters/scratchbird-prisma-adapter
node --test
```

## Scope notes

- This is not yet a full Prisma provider runtime.
- It provides adapter-level contract logic and deterministic tests to de-risk
  datasource validation, scalar mapping, schema generation, and migration/reflection workflows.
- Connection URL parsing now accepts the full canonical ScratchBird
  ingress/auth/bootstrap alias family, including manager-proxy, token/assertion,
  channel-binding, and dormant-reattach options.

## Example workflow

```bash
cd lanes/active/adapters/scratchbird-prisma-adapter
node examples/migration-reflection-workflow.js
```
