# COPY Streaming Import And Export

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_copy`

Related pages: [INSERT Statement](insert.md), [Transaction Control](transaction_control.md), [Backup, Restore, Replication, And Migration](backup_restore_replication_migration.md), [Table Lifecycle](table.md), [Type System Overview](../data_types/type_system_overview.md), [Security And Privileges](security_and_privilege_statements.md), and [Refusal Vectors](refusal_vectors.md).

## Purpose

`COPY` declares a bulk import or export route. For large insert streaming, use `COPY ... FROM STDIN` rather than constructing a very large `INSERT ... VALUES` statement. The `COPY` statement binds the target, column descriptors, stream profile, format, options, privileges, and SBLR import/export route. The actual row bytes are then moved as stream frames through the client/driver protocol or admitted bridge route.

The parser does not decode row bytes, store rows, authorize the stream, or decide transaction finality. It creates an import/export plan that the engine and streaming layer execute under ordinary authorization, descriptor, constraint, trigger, index, policy, and MGA rules.

## Basic Large Insert Streaming

The compact declaration for a large streamed insert is:

```sql
copy app.my_table (col1, col2, col3)
from stdin;
```

The client then sends row frames for `(col1, col2, col3)` until it sends the stream-end frame. Those row frames are not additional SQL statements.

CSV with a header row:

```sql
copy app.my_table (col1, col2, col3)
from stdin
with header;
```

JSON Lines:

```sql
copy app.my_table
from stdin jsonl;
```

The current public proof fixtures cover import planning for `COPY customer FROM STDIN`, `COPY customer FROM STDIN JSONL`, and `COPY customer FROM STDIN WITH HEADER`. Execution of the stream is an engine/driver route, not parser-side byte decoding.

## Syntax

```ebnf
copy_statement ::=
    COPY copy_target copy_direction copy_endpoint copy_format? copy_options? returning_clause? ;

copy_target ::=
      table_ref copy_column_list?
    | QUERY "(" query_dml_stmt ")" ;

copy_column_list ::=
    "(" identifier ("," identifier)* ")" ;

copy_direction ::=
      FROM
    | TO ;

copy_endpoint ::=
      STDIN
    | STDOUT
    | STREAM parameter_ref
    | LOCATION location_ref ;

copy_format ::=
      CSV
    | JSONL
    | BINARY
    | FORMAT identifier ;

copy_options ::=
    WITH copy_option ("," copy_option)* ;

copy_option ::=
      HEADER
    | NO HEADER
    | DELIMITER string_literal
    | NULL string_literal
    | QUOTE string_literal
    | ESCAPE string_literal
    | ENCODING identifier
    | BATCH SIZE integer_literal
    | REJECTS STREAM parameter_ref
    | MAX ERRORS integer_literal
    | ON ERROR STOP
    | ON ERROR QUARANTINE ;
```

Only admitted endpoint, format, and option combinations may execute. Unsupported or policy-denied combinations must fail before row data is accepted.

## Direction

| Direction | Meaning |
| --- | --- |
| `FROM` | Import stream rows into a target table or admitted writable rowset. |
| `TO` | Export a table or query rowset to an admitted output stream. |

Large insert streaming uses `FROM`.

```sql
copy app.event_stage (event_id, event_body, received_at)
from stdin jsonl;
```

Large export uses `TO`.

```sql
copy query (
  select event_id, event_body, received_at
  from app.event_stage
  where received_at >= :start_at
)
to stdout jsonl;
```

Export does not create row versions. Import creates row versions only through engine-owned execution after stream frames are validated and applied.

## Endpoint Model

| Endpoint | Contract |
| --- | --- |
| `STDIN` | Client-to-server stream attached to the current statement. |
| `STDOUT` | Server-to-client stream attached to the current statement. |
| `STREAM parameter` | A typed stream handle supplied as a parameter descriptor. |
| `LOCATION ref` | Policy-controlled server-side location. Denied unless location policy admits it. |

Portable client applications should prefer `STDIN`, `STDOUT`, or typed `STREAM` parameters. Server-local locations are administrative and policy-controlled.

## Formats

| Format | Typical use | Notes |
| --- | --- | --- |
| `CSV` | Delimited text import/export. | Default format for `COPY ... FROM STDIN` when no format is specified. |
| `JSONL` | One JSON value per line. | Each record maps through descriptor-bound field rules. |
| `BINARY` | Typed binary frames where admitted. | Requires a compatible descriptor and stream profile. |
| `FORMAT name` | Named format profile. | Must resolve to a policy-admitted format descriptor. |

Format parsing belongs to the admitted stream execution route, not to SQL text authority.

## Import Semantics

`COPY ... FROM` is a bulk insert route.

| Phase | Required behavior |
| --- | --- |
| Parse | Recognize the `COPY` statement shape. |
| Bind | Resolve target UUID, columns, descriptors, endpoint, format, options, and result shape. |
| Admit | Lower to the DML import planning route and verify required rights. |
| Open stream | Establish an authorized stream profile and frame limits. |
| Decode frames | Decode CSV, JSONL, binary, or named-format records through the stream execution route. |
| Validate rows | Apply type conversion, domains, defaults, generated values, constraints, policies, and triggers. |
| Maintain indexes | Maintain ordinary, document, vector, search, and other indexes attached to the target. |
| Finalize | Commit or roll back imported row versions through MGA. |

The statement can be used inside an explicit transaction:

```sql
begin transaction;

copy app.event_stage (event_id, event_body, received_at)
from stdin jsonl
with batch size 10000,
     on error quarantine;

commit;
```

In autocommit mode, the engine still opens and finalizes an engine-owned transaction around the admitted import work.

## Error Handling And Rejects

For large streams, reject handling must be explicit.

```sql
copy app.event_stage (event_id, event_body, received_at)
from stdin jsonl
with rejects stream :reject_stream,
     max errors 100,
     on error quarantine;
```

| Option | Behavior |
| --- | --- |
| `ON ERROR STOP` | Refuse the import when the first invalid row or stream error is encountered. |
| `ON ERROR QUARANTINE` | Put invalid rows into an admitted quarantine/reject route and continue until policy limits are reached. |
| `MAX ERRORS` | Stops the import when the error threshold is exceeded. |
| `REJECTS STREAM` | Emits rejected rows and diagnostics to a typed stream handle. |

If the transaction rolls back, successfully decoded row versions from the import are not visible to later snapshots.

## Authority Boundaries

| Concern | Authority |
| --- | --- |
| Target identity | Catalog UUID resolved during binding. |
| Stream bytes | Stream endpoint and frame profile. |
| Descriptor conversion | Engine descriptor/type system. |
| Authorization | Materialized security and policy. |
| Row visibility | MGA transaction inventory and row-version metadata. |
| Parser role | Syntax recognition and SBLR lowering only. |
| Commit/rollback | Engine-owned MGA finality. |

`COPY` must not embed SQL text, raw object-name authority, server-local path authority, or source handles into the import plan as durable authority.

## Diagnostics And Refusals

| Condition | Expected diagnostic class |
| --- | --- |
| Target not found or hidden by sandbox | Object resolution or sandbox denied. |
| Missing write/read privilege | Authorization denied. |
| Endpoint not admitted | Stream or location denied. |
| Format not supported | Unsupported format. |
| Option not admitted | Unsupported or policy-denied option. |
| Row frame invalid | Stream frame invalid. |
| Descriptor conversion fails | Type conversion refused. |
| Constraint, trigger, or policy fails | Row validation violation. |
| Too many reject rows | Import threshold exceeded. |
| Transaction outcome uncertain | Recovery-required or fail-closed diagnostic. |

Diagnostics should identify statement UUID, target, stream phase, row boundary, and refusal vector where disclosure policy permits it.

## Verification Checklist

| Check | Required outcome |
| --- | --- |
| Parse | `COPY` statement shape is recognized by SBsql. |
| Bind | Target, columns, endpoint, format, options, and result descriptors resolve. |
| Authorize | Effective user or agent UUID has required read/write and stream privileges. |
| Admit | SBLR route and result shape are accepted by the engine verifier. |
| Stream | Frame size, in-flight bytes, cancellation, timeout, and retry policy are enforced. |
| Validate | Rows are converted, constrained, triggered, indexed, and policy-checked by engine-owned routes. |
| Finalize | Imported rows become visible only through MGA commit finality. |
| Render | Progress, rejects, summary, and diagnostics expose only authorized information. |
