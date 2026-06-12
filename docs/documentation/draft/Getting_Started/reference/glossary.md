# Glossary

## Purpose

This glossary defines terms used in the draft Getting Started Guide and related SBsql Language Reference pages. The definitions are written for end users, evaluators, and operators. They are intentionally concise and cautious: a term appearing here does not mean the related feature is complete, enabled, or available in every build.

## ScratchBird Product Names

| Term | Meaning |
| --- | --- |
| ScratchBird | The project and product line described by this documentation. |
| ScratchBird Convergent Data Engine | The full product concept: engine, parsers, tools, resources, and operational surfaces. |
| SB | Short brand form used in names and examples. |
| SBcore | ScratchBird Engine. The embedded engine library that owns durable catalog identity, transactions, storage, security admission, recovery decisions, and engine diagnostics. |
| SBsql | ScratchBird SQL. The native ScratchBird command language and script runner surface. |
| SBParser | ScratchBird Core Parser. The native SBsql parser package that lowers SBsql requests to SBLR. |
| SBsrv | ScratchBird IPC Server. A local multi-user server process for same-machine clients. |
| SBgate | ScratchBird Listener. The listener and parser-facing entry point used for network-facing client traffic. |
| SBmgr | ScratchBird Single Node Manager. A single-node front door that can proxy authenticated connections to internal listener routes in managed deployments. |
| SBadm | ScratchBird Administrator. Administrative utility name for configuration, time zone, character set, collation, and policy management where present. |
| SBbak | ScratchBird Backup Manager. Utility name for backup and backup-set operations where present. |
| SBsec | ScratchBird Security. Utility name for security provider, user, role, group, and policy management where present. |
| SBdoc | ScratchBird Doctor. Utility name for analysis, diagnosis, and repair-oriented workflows where present and admitted. |
| SBcop | ScratchBird Conformance Officer. Utility name for conformance and comparison checks where present. |

## Architecture Terms

| Term | Meaning |
| --- | --- |
| Convergent Data Engine | An engine design that attempts to bring multiple data shapes, parser surfaces, transaction rules, security rules, and diagnostics under one shared engine authority model. |
| CDE | Abbreviation for Convergent Data Engine. |
| Engine authority | The rule that durable behavior belongs to SBcore: object identity, descriptors, transactions, security admission, storage, recovery, and diagnostics. |
| Parser boundary | The separation between a client language or wire protocol and engine execution authority. |
| Parser package | A component that accepts a specific language or protocol surface and lowers accepted work to ScratchBird execution requests. |
| Compatibility parser | A standalone parser package for one reference-system client family. It should not silently accept unrelated dialects. |
| SBsql language profile | A parser resource profile that can change user-facing SBsql spellings, phrase order, diagnostics, completion hints, and source rendering without changing SBLR, UUID identity, descriptors, security, storage, or MGA transaction authority. |
| Canonical element stream | The normalized parser output created before UUID binding. It records canonical token and surface identities rather than treating localized words as engine authority. |
| Standard SBsql fallback | A policy-controlled input fallback that lets a non-English session accept canonical English SBsql when the preferred language profile does not parse the statement. |
| Parser route | The configured path that determines which parser handles a client request. |
| SBLR | ScratchBird's bound engine-facing request representation. Parsers emit SBLR after parsing and binding accepted work. |
| Bound request | A structured request whose names, values, parameters, and types have been resolved enough to submit toward engine authority. |
| Raw text | The command text received from a client before parsing. Raw text is not durable engine authority. |
| Catalog projection | A view or metadata surface that presents engine catalog information in a particular shape for a parser, tool, or user. |
| Workarea | A schema-root area presented to a parser or user as its operating root. |
| Compatibility surface | The subset of behavior a parser or tool is designed and proven to accept, execute, or refuse clearly. |
| Refusal | A controlled response that says a request is unsupported, denied, unavailable, unsafe, or otherwise not admitted. |

## Database And Catalog Terms

| Term | Meaning |
| --- | --- |
| Database | A managed durable store of data, metadata, identity, transactions, security rules, diagnostics, and recovery behavior. |
| Metadata | Information that describes data, such as schemas, tables, columns, types, constraints, indexes, views, routines, grants, policies, and catalog rows. |
| Catalog | Engine-owned metadata that describes durable database objects and their relationships. |
| Catalog identity | The durable identity of a catalog object, separate from the user-facing name used to spell it. |
| UUID identity | Durable object identity based on UUIDs rather than only text names. |
| Object descriptor | Engine metadata describing an object shape, type, storage behavior, dependency, or operational capability. |
| Type descriptor | Metadata describing a datatype, its value behavior, binary representation, capabilities, and related rules. |
| Domain | A reusable constrained type definition. |
| Constraint | A rule attached to a table, column, domain, or related object. |
| Index | A search structure maintained for faster lookup, ordering, constraint enforcement, or query planning where implemented. |
| View | A named query projection. |
| Materialized view | A stored projection whose refresh and dependency behavior must be defined by the implementation. |
| Procedure | A stored routine that can perform controlled work and may return output parameters or result sets where supported. |
| Function | A routine that returns a value or result. |
| Package | A named grouping of routine definitions where supported. |
| Trigger | Routine behavior tied to table, database, transaction, or event-style actions where implemented. |
| Sequence | A database object that generates ordered values according to its definition. |
| Comment | Descriptive metadata attached to an object. Comments do not grant authority and should not contain secrets. |

## Schema And Name Terms

| Term | Meaning |
| --- | --- |
| Schema | A namespace branch that can contain objects and, where supported, child schemas. |
| Recursive schema tree | A schema model where schemas can contain child schemas, creating a tree rather than one flat namespace. |
| Database root | The top of the durable database tree. Not every session can see it directly. |
| Parser-visible root | The root of the namespace presented to the selected parser route. |
| Home schema | The schema associated with a user, identity, or configured workarea. |
| Current schema | The default schema used for unqualified names in a session. |
| Search path | An ordered lookup path used by commands that allow path-based name resolution. |
| Qualified name | A name that includes schema or path information, such as `app.notes`. |
| Unqualified name | A name without schema qualification, such as `notes`. |
| Name resolution | The process of turning a user-visible name into engine object identity. |
| Sandbox | The visible boundary that limits what a session or parser route can name, inspect, or access. |
| Schema branch | A subtree of the database namespace. |
| Object lifecycle | The create, alter, rename, comment, describe, use, refresh, validate, or drop actions that apply to an object type. |

## Transaction And Recovery Terms

| Term | Meaning |
| --- | --- |
| Transaction | A boundary around work that can commit, roll back, and participate in visibility rules. |
| Commit | Make a transaction's admitted changes final according to engine visibility rules. |
| Rollback | Discard uncommitted transaction changes. |
| Savepoint | A named point inside a transaction that can be rolled back without ending the whole transaction where supported. |
| Autocommit | A mode where each statement may be committed automatically according to session and parser rules. |
| MGA | ScratchBird's transaction and visibility authority model. In this documentation, the key rule is that transaction finality belongs to the engine. |
| Visibility | The rule that determines which transaction versions a session can see. |
| Cleanup | Engine-controlled work that reclaims or resolves old transaction state when it is safe. |
| Recovery | The process of reopening or refusing a database after shutdown, interruption, or uncertain durable state. |
| Recovery-required state | A state where the engine requires recovery handling before normal writes can proceed. |
| Fail closed | Refuse work when the safe outcome is uncertain instead of silently accepting it. |
| Reopen proof | A test that closes and reopens a database to verify committed state is still present. |

## Security Terms

| Term | Meaning |
| --- | --- |
| Identity | The authenticated user, service, or agent identity attached to a session or operation. |
| Principal | A user, role, group, service, or other authority-bearing identity. |
| Authentication | Establishing who the session or agent is. |
| Authorization | Deciding what an authenticated identity is allowed to do. |
| Grant | A permission given to a principal or object. |
| Revoke | Removal of a previously granted permission. |
| Role | A named set of privileges that can be granted and activated according to policy. |
| Policy | A rule that controls access, masking, row visibility, external access, operational admission, or protected material use. |
| Row-level security | Policy behavior that limits which rows a session can see or change. |
| Mask | A policy-controlled transformation that hides or changes protected values in query output. |
| Protected material | Secrets or sensitive values that require controlled storage, reference, redaction, and use. |
| Secret reference | A reference to protected material without placing the raw secret in a parser packet, script, or diagnostic. |
| Materialized authorization | Authorization information loaded into an engine-admissible form before work is executed. |
| Denied | A refusal because the authenticated identity, policy, or sandbox does not admit the operation. |

## Data And Type Terms

| Term | Meaning |
| --- | --- |
| Datatype | A named value category with storage, comparison, conversion, and validation behavior. |
| Scalar value | A single value such as an integer, timestamp, boolean, UUID, or text value. |
| Numeric type | A datatype for integer, unsigned integer, decimal, fixed-point, or floating-point values. |
| Text type | A datatype for character data governed by character set and collation rules. |
| Character set | The encoding rules for text values. |
| Collation | The comparison and ordering rules for text values. |
| Temporal type | A datatype for dates, times, timestamps, intervals, or time-zone-aware values. |
| UUID | A fixed-size identifier value commonly used for durable identity. |
| Binary value | A sequence of bytes with type-specific interpretation. |
| Protected value | A value governed by protected-material policy. |
| Document value | A structured value such as JSON-like data where implemented. |
| Graph value | A relationship-oriented value or model surface where implemented. |
| Vector value | A numeric vector used for similarity or embedding-style operations where implemented. |
| Time-series value | A value or record organized around time-oriented measurement behavior where implemented. |
| Coercion | An implicit or explicit conversion between compatible types. |
| Cast | An explicit type conversion requested by the user. |
| Null | A marker for absence of a value, distinct from zero, empty string, or false. |

## Query Terms

| Term | Meaning |
| --- | --- |
| DDL | Data definition language: commands that create, alter, describe, comment on, rename, or drop database objects. |
| DML | Data manipulation language: commands that read or change rows and values. |
| Query | A request that reads data and returns a result set or scalar result. |
| Result set | Rows and columns returned by a query or routine. |
| Projection | The selected output columns or expressions of a query. |
| Predicate | A condition used to filter rows or control logic. |
| Join | A query operation that combines rows from more than one source. |
| Grouping | A query operation that forms groups of rows for aggregate calculations. |
| Aggregate | A calculation over multiple rows, such as count or sum where implemented. |
| Window function | A calculation over a window of rows related to the current row where implemented. |
| CTE | Common table expression. A named temporary query expression inside a statement. |
| Recursive CTE | A CTE that refers to itself according to the rules of the language surface. |
| Ordering | The explicit sort order requested for result rows. |
| Limit | A request to return only a bounded number of rows. |
| Offset | A request to skip a number of rows before returning results. |
| Upsert | Insert-or-update behavior according to a conflict rule where supported. |
| Merge | A statement that conditionally inserts, updates, or deletes based on a source relation where supported. |
| Copy | A large or streaming data input or output surface where implemented and admitted. |

## Procedural Terms

| Term | Meaning |
| --- | --- |
| Procedural SQL | Stored routine language constructs such as blocks, variables, control flow, cursors, exceptions, and triggers where implemented. |
| Block | A procedural unit containing declarations and executable statements. |
| Variable | A named procedural value local to a routine or block. |
| Cursor | A controlled handle over a result set. |
| Result-set cursor | A cursor passed or returned as a routine-controlled result where supported. |
| Exception handler | Procedural logic that handles a diagnostic or error condition. |
| Event trigger | Trigger-style behavior tied to database, transaction, or event actions where implemented. |
| UDR | User-defined routine or parser-support routine package, depending on context. In parser documentation, it commonly means the package that supports bridge or extension behavior for that parser. |
| Bridge | A controlled connection or interface used by a parser-support routine to reach another database surface where configured and admitted. |

## Operations And Data Movement Terms

| Term | Meaning |
| --- | --- |
| Configuration | Settings that control startup, resource locations, parser registration, security providers, policy defaults, and runtime behavior. |
| Resource file | A staged file needed by the product, such as character set, collation, time zone, policy, or configuration data. |
| Health check | A diagnostic check that reports whether a component appears alive and able to answer. |
| Readiness check | A diagnostic check that reports whether a component is ready to accept intended work. |
| Liveness check | A diagnostic check that reports whether a component is still running. |
| Support bundle | A redacted package of diagnostic evidence for review or support. |
| Redaction | Removing or masking protected material before diagnostics are shown or bundled. |
| Message vector | Structured diagnostic output used for errors, refusals, and operational status. |
| Logical stream | Data movement represented as statements, rows, records, or events rather than physical page files. |
| Logical backup | A backup stream that represents database content as logical metadata and data operations. |
| Logical restore | Replaying a logical stream as admitted database operations. |
| Physical backup | A page-copy or file-copy backup shape. Compatibility parser routes should not treat physical page-copy formats as normal logical restore input. |
| Import | Bring external logical data into a database through an admitted parser or tool route. |
| Export | Write logical data from a database to an external stream or file according to policy. |
| CDC | Change data capture. A stream or record of changes suitable for replication, ETL, or integration where implemented. |
| Replication | Copying changes between systems according to an ordering and identity model where implemented. |
| ETL | Extract, transform, load. A data movement workflow that reads from one source, transforms, and writes to another target. |
| Migration | Moving schema, data, routines, security, or operational behavior from one database shape to another. |
| Quarantine | Holding questionable incoming records or events aside for review instead of applying them silently. |
| Cutover | The controlled switch from one active source or route to another. |
| Idempotency key | A value used to detect repeated events or operations so replay can be handled safely. |

## Build And Release Terms

| Term | Meaning |
| --- | --- |
| Build output | The generated binaries, libraries, parser packages, resources, and configuration artifacts for a target platform. |
| Output tree | The staged directory layout intended for testing or release packaging. |
| Target platform | The operating system and architecture being built or tested. |
| Proof gate | A test or validation step intended to prove that a behavior remains implemented and has not regressed. |
| CTest | The test runner integration used by many project tests. |
| Conformance test | A test that compares behavior against a declared specification, parser expectation, or compatibility target. |
| Smoke test | A small test proving that a basic workflow starts, runs, and stops. |
| Regression test | A test intended to prevent a previously handled behavior from breaking again. |
| Draft documentation | Documentation under active review. Draft status means users should verify commands and claims against the current build and tests. |

## See Also

- [Document Map](document_map.md)
- [What Is A Database?](../core_concepts/what_is_a_database.md)
- [How ScratchBird Implements A CDE](../core_concepts/how_scratchbird_implements_a_cde.md)
- [Schemas, Objects, And Names](../using_scratchbird/schemas_objects_and_names.md)
- [Language Reference](../../Language_Reference/README.md)
