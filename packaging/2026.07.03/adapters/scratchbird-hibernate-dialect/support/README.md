# ScratchBird Hibernate Dialect (Deterministic Contract)

This package provides deterministic contract coverage for `ECOSYS-403`.

## Scope

- Hibernate dialect class for ScratchBird (`ScratchBirdDialect`)
- ScratchBird JDBC URL guardrails for enterprise policy parity
- Full canonical ScratchBird ingress/auth/bootstrap URL option normalization
- ScratchBird type contribution map for ORM metadata paths
- JDBC metadata to Hibernate column-definition mapping helpers
- Transaction/savepoint SQL lifecycle contract helpers

## Run tests

```bash
cd lanes/active/adapters/scratchbird-hibernate-dialect
mvn -q test
```

## Runtime note

This package focuses on deterministic contract behavior. Full live JPA bootstrap,
entity lifecycle, and migration/runtime matrix coverage remains DSN/runtime-gated.

## Examples

- `examples/hibernate.properties.example`
- `examples/ScratchBirdEntityLifecycleExample.java`
- `examples/migration-mapping.sql`
