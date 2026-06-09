# Filespaces And Storage

## Purpose

This chapter defines operator-facing storage concepts: filespaces, database files, storage placement, storage health, and storage-related diagnostics.

## Initial Coverage

- first database file and primary filespace concepts;
- filespace identity;
- filespace create, attach, detach, promote, move, and remove concepts where implemented;
- storage permissions;
- temporary storage;
- storage health checks;
- low-space and disk-full behavior;
- refusal when storage state is unsafe;
- relationship between storage diagnostics and support bundles.

## Operator Questions

- Where are durable files allowed to live?
- Which filespace is required to open the database?
- What happens if a filespace is missing or unavailable?
- Which operations are native administrative operations rather than parser compatibility operations?

## Related Pages

- [Database Lifecycle](database_lifecycle.md)
- [Diagnostics, Message Vectors, And Support Bundles](diagnostics_message_vectors_and_support_bundles.md)
- [Language Reference: Filespace](../Language_Reference/syntax_reference/filespace.md)
