# First SBsql Session

## Purpose

Once your database is running (see [First Database](first_database.md)), the next step is learning how to work in a session: understanding where you are, running transactions deliberately, and reading what the engine tells you when something goes wrong.

SBsql is the native ScratchBird command language. A first SBsql session should prove that you can connect to the intended database, understand your schema context, run a small transaction, inspect results, and detach cleanly. This page walks through that arc step by step.

It does not replace the full Language Reference, and it should not be read as a complete list of supported statements.

## What A Session Is

A session is the authenticated conversation between a client and the database through a selected operating mode and parser route. Understanding what a session tracks helps you reason about why commands succeed or fail.

![diagram](./first_sbsql_session-1.svg)

Within one session, the engine tracks these things on your behalf:

- **identity**: who the engine thinks you are;
- **current schema**: where unqualified names resolve;
- **transaction state**: whether work is pending, committed, or rolled back;
- **parser route**: whether the request is going through native SBsql;
- **diagnostics**: how success and failure are reported.

## Session Checklist

Before running commands, know the answers to these questions — they explain most early session problems:

- which database you are connecting to;
- which operating mode is running;
- which identity is being used;
- whether autocommit is enabled by default for the selected tool;
- where diagnostics and logs can be reviewed;
- whether the database is disposable or persistent.

## Start With Context

Begin every new session by inspecting where you are. Exact output formatting can vary by build.

```sql
show schema path;
show search path;
select current_user;
show transaction;
```

These commands confirm that you are in the database and schema context you intended. If a command is not available in the current build, use the equivalent context-inspection command documented for that release. Starting without checking context is the most common cause of "objects appear missing" confusion.

## Create A Working Schema

Rather than placing test objects at the database root, create a schema to contain them. This mirrors real application practice and makes cleanup straightforward.

```sql
create schema app;

show schema path;
```

When the session's current schema is `app`, unqualified names such as `notes` can resolve relative to `app` when visible and unambiguous. The command for changing the current schema can vary by release, so the examples below use qualified names that do not depend on session schema state — that makes the examples more portable and clearer.

## Create A Table

The following table creation exercises several basic behaviors at once, which makes it a useful first test.

```sql
create table app.notes (
    note_id bigint not null,
    note_text text not null,
    created_at timestamptz not null,
    constraint pk_notes primary key (note_id)
);
```

This example demonstrates:

- table creation and column descriptor registration;
- scalar datatypes (`bigint`, `text`, `timestamptz`);
- a named primary key constraint;
- schema-qualified name resolution;
- catalog transaction behavior (the table is not visible until committed).

## Insert Rows

Insert more than one row so that ordering and row counts are easy to inspect in later steps.

```sql
insert into app.notes (note_id, note_text, created_at)
values
    (1, 'created from the first SBsql session', current_timestamp),
    (2, 'second row in the same statement', current_timestamp),
    (3, 'third row for ordering checks', current_timestamp);
```

Multi-row `values` input is useful for smoke tests because it proves that the parser and executor are not limited to one row per insert statement.

## Query Rows

Query the data using an explicit column projection and stable ordering. This example demonstrates a basic `select` with explicit output columns and an `order by` clause.

```sql
select note_id, note_text, created_at
from app.notes
order by note_id;
```

Avoid `select *` in documentation examples unless the point is to inspect all columns. Explicit projection makes examples clearer and avoids hiding column-order assumptions.

## Commit Or Roll Back Intentionally

End the transaction deliberately rather than relying on implicit behavior.

```sql
commit;
```

If you are experimenting and want to discard the work instead:

```sql
rollback;
```

For a first persistence test, commit the transaction, detach, reconnect, and query the table again. If the rows are not present after reconnect, check whether the commit ran and whether you reopened the same database.

## Test A Controlled Error

Run one statement that should fail. This teaches you what the engine's diagnostic output looks like before you encounter a real error under pressure.

```sql
select note_id
from notes_that_do_not_exist;
```

Expected behavior:

- the client receives a message vector (a structured diagnostic record) explaining the failure;
- the session remains controlled;
- protected details are not leaked;
- the next allowed command behaves according to transaction state.

Successful systems explain failures clearly. A controlled refusal is the right behavior here.

## Reconnect And Verify Persistence

After commit, verify that the data was actually written to durable storage.

1. Detach the SBsql client.
2. Stop and restart the selected runtime if appropriate.
3. Connect again.
4. Qualify names with the schema.
5. Query the committed rows.

```sql
select count(*) as note_count
from app.notes;

select note_id, note_text
from app.notes
order by note_id;
```

If the rows are not present, check whether the transaction was committed, whether the same database was reopened, and whether the current schema is the one you expect.

## Clean Up Test Objects

For a disposable first session, remove the test objects after verifying the workflow.

```sql
drop table app.notes;
drop schema app;
commit;
```

Only drop objects that you created for the test. Do not run cleanup examples against a database that contains real work.

## Reading Result Sets

A result set has column names, column order, datatypes, nullability behavior, and row order. For early tests, keep these habits:

- name the columns you want explicitly;
- include `order by` when row order matters;
- test null values intentionally;
- test type conversion intentionally;
- keep result sets small enough to inspect by eye.

## Reading Message Vectors

ScratchBird diagnostics communicate structured refusal or error information through message vectors. A user-facing rendering may include text, code, class, source component, object name, or policy information depending on the command and build.

For a first session, it is useful to categorize failures you see. Different categories require different fixes:

- syntax errors: the statement text needs correction;
- missing object errors: check the current schema and whether the object was committed;
- authorization denials: check grants and session identity;
- unsupported feature refusals: the feature may not be available in this build;
- configuration problems: check resource files, parser registration, or route configuration;
- runtime availability problems: check whether the required process is running.

## Common Session Mistakes

| Mistake | What Happens |
| --- | --- |
| Connecting to the wrong database path | Objects appear missing or changes appear to disappear. |
| Forgetting to commit | Reconnect tests may not show expected rows. |
| Using the wrong current schema | Unqualified names resolve somewhere else or fail. |
| Relying on implicit row order | Result rows may not appear in insertion order. |
| Mixing parser expectations | Native SBsql examples should be run through the SBsql parser route. |
| Ignoring diagnostics | A refused command may leave the session in a state that requires commit, rollback, or detach. |

## Where To Go Next

With a working session under your belt, [Schemas, Objects, And Names](schemas_objects_and_names.md) explains how ScratchBird stores durable object identity separately from the names you type — which matters as soon as you start renaming things, working with compatibility parsers, or writing migration scripts.

- [First Database](first_database.md)
- [Schemas, Objects, And Names](schemas_objects_and_names.md)
- [Schema Tree And Name Resolution](../../Language_Reference/syntax_reference/schema_tree_and_name_resolution.md)
- [Table Statements](../../Language_Reference/syntax_reference/table.md)
- [Insert](../../Language_Reference/syntax_reference/insert.md)
- [Select](../../Language_Reference/syntax_reference/select.md)
- [Transaction Control](../../Language_Reference/syntax_reference/transaction_control.md)
