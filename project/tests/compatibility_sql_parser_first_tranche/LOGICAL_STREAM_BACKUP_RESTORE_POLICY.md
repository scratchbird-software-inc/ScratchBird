# Compatibility Logical Remote Stream Backup and Restore Policy

Search key: `SB_TEST_COMPATIBILITY_LOGICAL_REMOTE_STREAM_BACKUP_RESTORE_POLICY`

This tracked test policy records the compatibility-by-compatibility backup/restore boundary
that must survive public release packaging and full CTest runs.

A compatibility parser may support full or partial backup and restore only when the
operation is a remote client stream and the stream is logical: metadata, DDL,
rows, documents, keys, vectors, graph entities, time-series points, or another
compatibility-level record stream that can be translated into SBLR or parser-support
UDR calls.

Every parser must deny server-local file access by default. Physical page-copy
backup and restore are always denied from parser authority. Denied physical
forms include database-file copies, nbackup/page-copy streams, basebackup,
SSTables, tablets, snapshots, RDB/AOF files, raw filespaces, object-store
backup repositories, and equivalent storage artifacts.

An admitted logical restore stream is a source of instructions and data. The
parser decodes compatibility records and lowers them to ScratchBird calls. The engine
owns catalog mutation, data mutation, transaction grouping, visibility,
durability, cleanup, and recovery.

An admitted logical backup stream is a remote sink. The parser requests
authorized engine reads from the single connected emulated compatibility database and
renders compatibility-compatible logical records back to the client. Full and partial
streams are allowed only where the compatibility normally exposes that scope.

The matrix for each compatibility parser is:

`project/tests/compatibility_sql_parser_first_tranche/logical_stream_backup_restore_policy_matrix.csv`

The implementation route is not considered complete until the relevant compatibility
parser has a real SBLR/parser-support surface:

```text
required_logical_stream_backup_restore_surface
```

Firebird must explicitly allow compatibility-real `gbak` logical remote service
streams that use `stdout` for backup and `stdin` for restore/create/replace,
and deny `nbackup` plus server-local or file-path `gbak` forms. PostgreSQL,
MySQL, MariaDB, Vitess, TiDB, and YugabyteDB must distinguish client-stream
logical import/export from server-local file access.
