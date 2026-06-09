# Backup, Restore, And Data Movement

## Purpose

This chapter defines operational data movement: backup, restore, import, export, CDC, replication, ETL, migration, validation, and refusal boundaries.

## Initial Coverage

- logical backup streams;
- logical restore streams;
- partial backup and restore where implemented;
- import and export;
- copy-style large streaming;
- CDC and replication;
- ETL workflows;
- migration staging and cutover;
- denied physical page-copy routes through parser compatibility surfaces;
- denied low-level repair or verification through parser compatibility surfaces;
- restore drills and validation queries.

## Core Rule

Logical streams are handled as admitted database work. Physical page-copy, low-level repair, and direct server-local file manipulation require native administrative authority and should not be inferred from parser compatibility syntax.

## Related Pages

- [Diagnostics, Message Vectors, And Support Bundles](diagnostics_message_vectors_and_support_bundles.md)
- [Release Validation Checklist](release_validation_checklist.md)
- [Getting Started: Backup, Restore, And Data Movement Overview](../Getting_Started/administration/backup_restore_and_data_movement_overview.md)
