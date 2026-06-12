# Replication Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It documents the grammar production for `REPLICATION` while preserving the ScratchBird authority model: parsing recognizes shape, binding resolves descriptors and UUID catalog identity, SBLR admits the operation route, and local apply work commits or rolls back through MGA.

Generation task: `ebnf_replication_statement`

Parent reference: [Backup, Restore, Replication, And Migration](../backup_restore_replication_migration.md)

## Production

```ebnf
replication_stmt ::=
    REPLICATION replication_action replication_payload? replication_option_list? ;

replication_action ::=
      CREATE
    | ALTER
    | START
    | STOP
    | DRAIN
    | RESET
    | VALIDATE
    | SHOW
    | DESCRIBE
    | DROP
    | CHANGEFEED ;

replication_payload ::=
      ROUTE qualified_name
    | SOURCE replication_endpoint
    | TARGET replication_endpoint
    | CHANGEFEED qualified_name ;

replication_option_list ::=
    WITH replication_option ("," replication_option)* ;

replication_option ::=
      SOURCE replication_endpoint
    | TARGET replication_endpoint
    | INCLUDE TABLE table_ref
    | INCLUDE SCHEMA schema_ref
    | EXCLUDE TABLE table_ref
    | START AT replication_boundary
    | STOP AT replication_boundary
    | MODE SNAPSHOT
    | MODE CONTINUOUS
    | APPLY
    | NO APPLY
    | IDEMPOTENCY KEY expression
    | ORDER BY ordering_token_ref
    | QUARANTINE INVALID ROWS
    | RETRY retry_profile ;
```

## Meaning

`replication_stmt` recognizes route and changefeed management. It can describe source and target endpoints, included scopes, ordering evidence, idempotency, apply mode, quarantine behavior, and lifecycle actions. It does not make external ordering tokens or endpoint state into local transaction authority.

## Binding Requirements

| Element | Binding requirement |
| --- | --- |
| Route | Durable route metadata visible to the caller. |
| Endpoint | Registered stream, database, bridge, or target descriptor admitted by policy. |
| Scope | Authorized database, schema, table, or query scope. |
| Ordering | Ordering-token descriptor used as evidence, not finality authority. |
| Idempotency | Replay-safe key descriptor where apply or retry can occur. |
| Apply mode | Local transaction and quarantine behavior. |

## Used By

| Parent production | Purpose |
| --- | --- |
| `archive_replication_migration_statement` | Places `REPLICATION` in the administrative data-movement family. |

## Admission Notes

- Publish routes read committed local changes through an admitted boundary.
- Apply routes write through engine-owned SBLR operations and MGA finality.
- Ambiguous ordering or missing idempotency must fail closed.
- `SHOW` and `DESCRIBE` redact endpoint and credential details where required.
