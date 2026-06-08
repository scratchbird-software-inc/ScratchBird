# Donor Database Compatibility

## Purpose

ScratchBird can include donor parser packages. A donor parser is a compatibility layer for a specific database family or protocol surface. It is not a universal parser and should not know about unrelated donor dialects.

## How Donor Compatibility Works

```mermaid
flowchart LR
    DonorClient[Donor client] --> DonorParser[Matching donor parser]
    DonorParser --> Bound[Bound ScratchBird request]
    Bound --> Engine[SBcore]
    Engine --> Result[Engine result]
    Result --> DonorParser
    DonorParser --> DonorClient
```

The donor parser is responsible for donor syntax, donor defaults, donor catalog projections, and donor-style diagnostics where implemented. The engine remains responsible for ScratchBird storage, transactions, security, and catalog identity.

## Parser Isolation

Each donor parser is standalone. A Firebird-style parser should support Firebird-style behavior. A PostgreSQL-style parser should support PostgreSQL-style behavior. A MySQL-style parser should support MySQL-style behavior. Installing one parser should not imply that another parser is present.

## Compatibility Scope

Compatibility can include:

- SQL syntax;
- datatypes;
- catalog views;
- wire protocol behavior;
- metadata queries;
- backup or restore streams where supported and safe;
- CDC, replication, or ETL surfaces where implemented;
- procedural language features where implemented;
- donor-specific object defaults.

Compatibility can also include explicit refusal. For example, unsafe server-local file access or low-level repair operations may be denied by policy even if a donor tool has a command shape for them.

## Careful Claims

Do not infer compatibility from a directory name, parser profile, or test fixture alone. Compatibility should be read from current implementation status, proof gates, and parser-specific documentation.

## Related Pages

- [../architecture/engine_parser_boundary.md](../architecture/engine_parser_boundary.md)
- [../../Language_Reference/data_types/type_system_overview.md](../../Language_Reference/data_types/type_system_overview.md)
