# Database Lifecycle

## Purpose

This chapter defines the operational lifecycle of a ScratchBird database from creation through open, attach, detach, close, reopen, verification, refusal, and recovery-required states.

## Initial Coverage

- create database;
- open database;
- attach session;
- detach session;
- close database;
- reopen database;
- database route selection;
- database identity and catalog bootstrap;
- initial filespace behavior;
- recovery-required mode;
- read-only or restricted open where implemented;
- refusal diagnostics when open is unsafe.

## Required Proof Shape

The expanded chapter should include a repeatable smoke test:

1. create or open a disposable database;
2. create schema and table;
3. insert rows;
4. commit;
5. detach and close;
6. reopen;
7. verify committed rows;
8. test one controlled refusal.

## Related Pages

- [Filespaces And Storage](filespaces_and_storage.md)
- [Backup, Restore, And Data Movement](backup_restore_and_data_movement.md)
- [Language Reference: Database](../Language_Reference/syntax_reference/database.md)
