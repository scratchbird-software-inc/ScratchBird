# First SBsql Session

## Purpose

SBsql is the native ScratchBird SQL language. This page describes what a first session is trying to do without depending on a specific build command.

## Session Goals

A first SBsql session should prove that you can:

- connect to the intended database;
- identify the current schema context;
- create a schema;
- create a table;
- insert a row;
- query the row;
- commit or roll back intentionally;
- detach cleanly.

## Example Session

```sql
show current schema;
show search path;

create schema app;
set schema app;

create table notes (
  note_id uuid primary key,
  note_text varchar(200)
);

insert into notes (note_id, note_text)
values (uuid_v7(), 'hello from ScratchBird');

select note_id, note_text
from notes;

commit;
```

This example is illustrative. Function availability, exact command spelling, and output formatting should be checked against the current SBsql parser and language reference.

## Expected Diagnostics

A good first session should also include one intentional mistake, such as selecting from a missing table. The goal is to confirm that ScratchBird returns a controlled diagnostic or message vector instead of failing unclearly.

```sql
select *
from missing_table;
```

## Related Pages

- [../../Language_Reference/syntax_reference/select.md](../../Language_Reference/syntax_reference/select.md)
- [../../Language_Reference/syntax_reference/table.md](../../Language_Reference/syntax_reference/table.md)
- [../../Language_Reference/syntax_reference/transaction_control.md](../../Language_Reference/syntax_reference/transaction_control.md)
