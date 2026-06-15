# Refusal Vectors

This page is part of the SBsql Language Reference Manual. It explains how SBsql reports commands that are syntactically recognized but cannot be executed in the current security, policy, build, license, provider, recovery, or stream context.

Generation task: `syntax_reference_refusal_vectors`

Related pages: [Security And Privileges](security_and_privilege_statements.md), [Policy, Mask, And RLS Lifecycle](policy_mask_and_rls.md), [Cluster-Gated Statements](cluster_gated_statements.md), [Management And Operations](management_and_operations.md), [Backup, Restore, Replication, And Migration](backup_restore_replication_migration.md), [COPY Streaming Import And Export](copy.md), [Transaction Control](transaction_control.md), [Schema Tree And Name Resolution](schema_tree_and_name_resolution.md), and [Bridge Boundary Model](../core_paradigms/bridge_and_cluster_boundaries.md).

## Purpose

A refusal vector is the structured diagnostic returned when SBsql understands the requested surface but must not execute it. Refusal vectors are part of the language contract. They let clients distinguish a spelling error from a recognized but unavailable command, a policy denial, an unlicensed capability, a recovery fence, or a stream validation failure.

Refusal is deliberately separate from ordinary parse failure:

| Outcome | Meaning | Typical phase |
| --- | --- | --- |
| Parse error | The text cannot be reduced to an SBsql statement or expression. | Parse |
| Bind error | Names, descriptors, parameters, or result shapes cannot be resolved. | Bind |
| Refusal vector | The request is recognized, bound enough to classify, and rejected by authority, build, policy, license, provider, stream, recovery, or safety rules. | Admission or execution gate |
| Runtime diagnostic | The operation was admitted and then failed during execution, such as constraint failure, arithmetic error, or transaction conflict. | Execution |

The parser never grants authority by accepting text. Durable catalog identity is UUID based, descriptors define type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

## High-Level Classes

Every refusal vector belongs to one of three public classes.

| Class | Meaning | Retry expectation |
| --- | --- | --- |
| `unsupported` | The surface, option, route, shape, profile, build flag, or provider operation is not available in this build or for this target. | Retry only after changing the statement, build profile, provider, or feature set. |
| `denied` | The request is blocked by authorization, sandboxing, policy, safety, recovery state, resource admission, descriptor rules, stream rules, or data-protection rules. | Retry only after the blocking authority condition changes. |
| `unlicensed` | The surface and route are recognized, but the running product profile or admitted provider reports that the capability is not licensed. | Retry only with a product profile or provider that licenses the capability. |

These classes are intentionally coarse at the top level. Client tools should display the class first, then use the canonical reason code and message fields for detail.

## Syntax Productions

```ebnf
refusal_statement ::=
      unsupported_statement
    | refusal_stmt
    | unlicensed_statement ;
```

```ebnf
unsupported_statement ::= unsupported_token_sequence ;
refusal_stmt          ::= denied_token_sequence ;
unlicensed_statement  ::= unlicensed_token_sequence ;
```

The EBNF names above describe the diagnostic surface. They are not commands that users issue directly. A user issues an ordinary SBsql statement; if the statement reaches a refusal gate, the result is rendered as a refusal vector.

## Admission Stages

SBsql admission is layered. A later stage may still refuse a request even when every earlier stage succeeded.

![diagram](./refusal_vectors-1.svg)

The exact stage is reported in the refusal payload so tools can separate a missing feature from a security decision or a recovery fence.

## Canonical Message Vector

SBsql clients should treat the message vector as the authoritative diagnostic payload. Text rendering is for humans and may be localized or redacted.

| Field | Contract |
| --- | --- |
| `class` | One of `unsupported`, `denied`, or `unlicensed`. |
| `code` | Stable diagnostic code for programmatic handling. |
| `severity` | Error, warning, notice, or informational diagnostic. Refusals that block execution are errors. |
| `statement_family` | Bound statement family, such as DML, DDL, transaction, management, stream, bridge, backup, security, or cluster-gated. |
| `operation_id` | Stable operation identifier after SBsql/SBLR classification. |
| `surface_id` | Grammar or route surface that produced the refusal. |
| `stage` | Parse, bind, admission, authorization, stream, provider, recovery, resource, or execution gate. |
| `principal_uuid` | Effective user or agent UUID when disclosure policy admits it. |
| `object_uuid` | Target object UUID when disclosure policy admits it. |
| `scope` | Database, schema, object, stream, session, bridge, provider, or system scope. |
| `retryable` | Boolean or policy code describing whether retry can succeed without changing authority or configuration. |
| `operator_action` | Optional safe next action, such as request privilege, enable a provider, change endpoint, reopen in recovery mode, or inspect support diagnostics. |
| `redaction` | Indicates whether names, paths, identifiers, policy names, or object details were hidden. |
| `evidence_ref` | Optional internal evidence handle or support-bundle reference. It must not expose secrets or protected material. |
| `message` | Human-readable summary. It must not reveal hidden object existence unless policy permits disclosure. |

SQLSTATE-style codes may be rendered by a driver when that driver has a mapping. The canonical SBsql contract is still the message vector.

## Unsupported

`unsupported` means the request is recognized but unavailable. It does not imply the user lacks privilege.

Common unsupported cases:

| Case | Example cause | Expected result |
| --- | --- | --- |
| Statement family absent from build | A build omits an optional runtime route. | `unsupported` before execution. |
| Option not admitted | A clause uses an option the route does not implement. | `unsupported` at bind or admission. |
| Target does not support the operation | A stream format, index attribute, or descriptor rule is unavailable for the target object. | `unsupported` with target class when disclosure permits it. |
| Provider operation missing | A provider is present but reports no support for the requested operation. | `unsupported` from provider admission. |
| Cluster route disabled | A cluster-gated statement is recognized but cluster routing is not admitted in the build. | `unsupported` with no provider call. |

Example:

```sql
select *
from app.orders
fetch first 10 rows with ties;
```

If the active query profile does not admit `WITH TIES`, the statement is refused as unsupported before row execution.

Example:

```sql
copy app.events
from stdin
with format compressed_binary;
```

If `compressed_binary` does not resolve to an admitted stream format descriptor, the stream is refused before accepting row frames.

## Denied

`denied` means the request is known but must not proceed under current authority, sandbox, policy, safety, recovery, stream, or resource rules. Denial is fail-closed. A denial should not reveal whether a hidden object exists unless the security policy allows that disclosure.

Common denied cases:

| Case | Example cause | Expected result |
| --- | --- | --- |
| Authentication or active role failure | The session lacks an admitted identity or active role. | `denied` with security stage. |
| Privilege failure | The effective principal lacks the required privilege on a target UUID. | `denied`; hidden target details may be redacted. |
| Sandbox boundary | Name resolution attempts to leave the session sandbox root. | `denied` with sandbox stage. |
| Policy, mask, or RLS failure | A policy blocks the row, column, expression, or operation. | `denied` with policy stage. |
| Protected material release | A statement tries to display, export, cast, or log protected material without release authority. | `denied` with redaction. |
| Server-local file access | A bulk, backup, restore, or diagnostic statement tries to open a server-local path without policy admission. | `denied` before file access. |
| Physical backup or restore route | A request attempts low-level page-copy backup or restore through a non-admitted surface. | `denied` before file or page access. |
| Repair or verification route | A low-level repair, verification, or file-structure operation is attempted outside an admitted administrative route. | `denied` with management or diagnostic stage. |
| Recovery fence | The database requires recovery, read-only recovery mode, or operator action before writes are admitted. | `denied` with recovery stage. |
| Resource admission | Memory, file descriptor, stream bytes, queue depth, or timeout policy blocks the request. | `denied` or runtime diagnostic according to the stage. |

Sandbox example:

```sql
select *
from admin.security_audit;
```

If the session root is `tenant_a` and `admin.security_audit` is outside that root, the resolver returns a sandbox denial. The rendered message should not reveal hidden names outside the sandbox.

Server-local file example:

```sql
copy app.orders
from location '/var/import/orders.csv'
with header;
```

`LOCATION` is a policy-controlled server-side endpoint. If the active policy does not admit that location, the engine refuses the statement before opening the path. Client-side streaming should use `STDIN` instead:

```sql
copy app.orders
from stdin
with header;
```

Recovery example:

```sql
insert into app.orders (order_id, status)
values (:order_id, 'pending');
```

If the database is fenced for recovery-required mode, the write is denied before creating durable row versions.

## Unlicensed

`unlicensed` means the parser, SBLR route, ABI boundary, or provider handshake recognized the command, but the current product profile does not license execution. This is not a privilege failure and should not be fixed by granting object privileges.

Common unlicensed cases:

| Case | Example cause | Expected result |
| --- | --- | --- |
| Public compile/link stub | A gated provider boundary exists only to prove routing and diagnostics. | `unlicensed` or fail-closed provider refusal. |
| Product profile excludes route | The binary can classify the operation, but the profile does not license it. | `unlicensed` at admission. |
| Provider capability present but not licensed | A provider reports the operation as known but unavailable for this installation. | `unlicensed` from provider admission. |

Example:

```sql
show cluster provider;
```

In a public build that admits the compile/link stub, this reaches the stub boundary and returns provider metadata plus an unlicensed or fail-closed diagnostic. In a build that does not admit cluster routing, the same statement returns `unsupported` before a provider call.

## Bridge And Stream Refusals

Bridge, stream, backup, restore, replication, migration, and `COPY` statements have additional validation because they can move data across connection boundaries.

| Condition | Refusal class | Rule |
| --- | --- | --- |
| Missing bridge package or provider boundary | `unsupported` | The route is not available. |
| Bridge authentication failure | `denied` | The remote session cannot be established with admitted identity and secret references. |
| Missing capability | `unsupported` | The target route does not report the requested capability. |
| External network blocked by policy | `denied` | Policy does not admit the outbound or inbound route. |
| Stream format invalid | `denied` or runtime diagnostic | Bind-time format mismatch is denied; per-row data errors follow the stream error policy. |
| Ordering ambiguous | `denied` | CDC, replay, or migration route lacks the required ordering token or transaction grouping. |
| Idempotency key missing | `denied` | An apply or replay route requires idempotency and the request did not provide it. |
| Cutover validation failure | `denied` | The route cannot prove that the destination is ready for cutover. |

Logical streams are admitted only through explicit stream surfaces and policies. Page-copy backup, low-level database repair, server-local file manipulation, and unbounded diagnostic extraction must fail closed unless an administrative SBsql route explicitly admits the operation.

## Rendering Rules

Refusal rendering must be useful without leaking protected information.

- A message may name an object only when the session is allowed to know that object exists.
- A sandbox denial should report the sandbox boundary, not hidden outside branches.
- Secret references may be named by safe identifier; raw secrets must never be displayed.
- Server-local paths should be redacted unless policy admits diagnostic path disclosure.
- Support-bundle references should identify evidence handles, not protected content.
- Provider diagnostics should be normalized into the SBsql message vector before being displayed.
- Repeated failures should be stable enough for automated tests to compare class, code, stage, and operation identity.

Example display:

```text
class: denied
code: UDR.BRIDGE.SANDBOX_DENIED
stage: authorization
operation: query.relational.select
message: object is outside the session schema root
redaction: target_name_hidden
retryable: false
```

Example display for an unavailable route:

```text
class: unsupported
code: UDR.BRIDGE.UNSUPPORTED
stage: admission
operation: bulk.import
message: stream format is not admitted for this target
retryable: false
```

## Retryability

Refusal vectors should not encourage blind retries.

| Refusal | Retry without change? | Typical fix |
| --- | --- | --- |
| Unsupported statement family | No | Use an admitted surface or enable a build/provider that supplies the route. |
| Unsupported option | No | Remove or replace the option. |
| Denied privilege | No | Grant the required privilege or change active role where policy admits it. |
| Denied sandbox | No | Connect to the correct schema root or use an authorized catalog projection. |
| Denied recovery fence | No | Complete recovery or open the database in an admitted recovery mode. |
| Denied resource policy | Sometimes | Reduce batch size, stream frame size, timeout, or concurrency. |
| Unlicensed route | No | Use a product profile or provider that licenses the route. |
| Provider unavailable | Sometimes | Wait for provider health recovery or choose another admitted provider. |

## Transaction Semantics

A refused statement does not commit partial work. If refusal happens before execution, no row version, catalog mutation, index change, filespace change, stream apply, or transaction finality change is created.

If a statement streams multiple records and the refusal occurs after the statement has been admitted, the stream error policy controls whether the transaction aborts, quarantines invalid records, or stops at the first failing frame. MGA still owns transaction finality. A parser, stream reader, provider, or bridge route does not decide commit, rollback, prepare, or recovery state.

## Proof Expectations

Refusal behavior is part of the testable public contract. Proof suites should verify:

- recognized but unavailable surfaces return `unsupported`, not parse errors;
- security and sandbox failures return `denied` without leaking hidden names;
- unlicensed provider routes reach the admitted stub only when the build profile allows routing;
- disabled gated routes return `unsupported` before provider calls;
- server-local file and physical page-copy attempts fail before opening files;
- logical stream declarations refuse invalid formats before accepting data frames;
- protected values are redacted in diagnostics, logs, and support bundles;
- recovery-required mode fences writes and reports the recovery stage;
- message vectors include stable class, code, stage, operation, retryability, and redaction fields;
- repeated full rebuilds regenerate the same proof outcomes.

## Related Surface Rows

| Surface | Kind | Family | Lowering | Result Shape |
| --- | --- | --- | --- | --- |
| `refusal_statement` | grammar production | diagnostic | yes | `sblr.diagnostic.refusal.v3` |
| `unsupported_statement` | grammar production | diagnostic | yes | `sblr.diagnostic.refusal.v3` |
| `refusal_stmt` | grammar production | diagnostic | yes | `sblr.diagnostic.refusal.v3` |
| `unlicensed_statement` | grammar production | diagnostic | yes | `sblr.diagnostic.refusal.v3` |
| `cluster_gated_statement` | statement family | cluster-gated | yes | `sblr.diagnostic.refusal.v3` when refused |
| `copy_statement` | statement family | stream | yes | `sblr.bulk.import.v3` or `sblr.bulk.export.v3` when admitted |
| `backup_stmt` | statement family | archive | yes | `sblr.diagnostic.refusal.v3` when refused |
| `restore_stmt` | statement family | archive | yes | `sblr.diagnostic.refusal.v3` when refused |
| `grant_stmt` | statement family | security | yes | `sblr.catalog.mutation.v3` when admitted |
| `policy_stmt` | statement family | security | yes | `sblr.catalog.mutation.v3` when admitted |
