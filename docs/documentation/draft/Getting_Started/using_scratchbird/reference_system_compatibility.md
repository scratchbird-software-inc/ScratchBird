# Reference-System Compatibility

## Purpose

ScratchBird can serve clients that speak the language and protocol of another database system, through a compatibility parser package. This matters when you have existing applications, tools, or workflows built for another system and you want them to connect to a ScratchBird database without being rewritten.

A compatibility parser is a standalone adapter for one client family. It lets a client speak that parser's language and protocol shape while ScratchBird keeps storage, transactions, identity, security, and recovery inside the ScratchBird engine. The key word is "scoped": the presence of a parser package means there is a route for that client family; it does not mean every command, tool behavior, catalog row, wire-protocol edge case, or administrative feature from that source ecosystem is complete in the current build.

This page should be read after [Schemas, Objects, And Names](schemas_objects_and_names.md), because compatibility parsers surface a workarea as the client-visible root — which only makes sense once you understand the recursive schema tree.

## The Compatibility Model

A compatibility parser sits between a client and SBcore (the core database engine). The parser is responsible for accepting the client surface; the engine remains responsible for durable authority.

![diagram](./reference_system_compatibility-1.svg)

## What The Parser Owns

A compatibility parser can own client-facing behavior. Understanding this boundary helps you know what to test per-parser and what remains consistent across all parsers.

Parser-owned behavior includes:

- protocol negotiation for its client family;
- syntax accepted by that parser;
- parser-specific object defaults;
- parser-specific type spelling and literal handling;
- catalog projections that make ScratchBird metadata visible in the expected shape;
- diagnostic rendering for that client family;
- logical backup, restore, replication, or import/export streams where those surfaces are implemented and admitted by policy;
- mapping accepted work into SBLR (the internal representation passed to the engine).

The parser does not own final transaction authority, storage recovery, durable catalog identity, authorization finality, object UUID assignment, cleanup and garbage collection decisions, page format interpretation outside the engine, or low-level repair or verification authority.

## Parser Isolation

Each compatibility parser is isolated from the others. This separation is intentional — it keeps compatibility behavior explicit and prevents a client from accidentally entering a different dialect.

| Rule | Meaning |
| --- | --- |
| One parser, one client family | A parser should accept only the syntax and protocol shape it is built for. |
| No cross-dialect fallback | A parser should not silently accept unrelated language forms because another parser supports them. |
| No implicit dependency | Installing one compatibility parser must not imply that any other parser is installed. |
| Parser-local defaults | Object defaults, name folding, null handling, index defaults, and diagnostics are parser-specific where implemented. |
| Engine authority remains shared | Once accepted and lowered, engine execution still uses ScratchBird catalog, security, transaction, and storage rules. |

## Compatibility Areas

Compatibility is not a single thing — a parser can be strong in one area and incomplete in another. Read compatibility claims by specific surface rather than treating "has a parser package" as blanket compatibility.

| Area | What To Check |
| --- | --- |
| Connection behavior | Authentication route, session state, database selection, client options, and refusal behavior. |
| SQL syntax | Accepted statements, expressions, functions, procedural blocks, comments, identifiers, and script tokens. |
| Type system | Native type spelling, value ranges, coercions, binary encodings, domain behavior, and null handling. |
| DDL | Create, alter, drop, recreate, rename, comment, show, describe, and object-specific lifecycle behavior. |
| DML and query behavior | Insert, update, delete, merge, upsert, select, joins, grouping, ordering, windowing, recursive common table expressions, and result ordering. |
| Transactions | Autocommit, begin, commit, rollback, savepoints, retain or chain behavior where applicable, and prepared transactions where supported. |
| Procedural SQL | Stored procedures, functions, triggers, packages, events, cursors, result sets, and routine metadata. |
| Catalog projection | System tables, views, metadata functions, dependency rows, privileges, indexes, constraints, and generated objects. |
| Backup and restore | Logical stream support, denied physical copy behavior, server-local file policy, and diagnostics. |
| CDC, replication, and ETL | Direction, ordering, transaction grouping, record identity, quarantine, cutover, and idempotency behavior where implemented. |
| Diagnostics | Error codes, message text, refusal classes, unsupported behavior, denied behavior, and policy failures. |

## Sandboxed Workareas

A compatibility client normally connects to a compatibility workarea (the namespace root visible to the client). That workarea is the root of the namespace the client can see. Objects outside the workarea are not directly accessible to the client, even if a catalog projection displays selected metadata about them.

![diagram](./reference_system_compatibility-2.svg)

Some catalog projection objects may display selected metadata from outside the sandbox when the projection object itself has been granted that authority. That does not give the user unrestricted direct access outside the workarea.

## Logical Streams And File Access

Compatibility commands that move data require careful handling. The key distinction is between logical remote streams and server-local or physical storage operations.

Allowed by design when implemented and policy admits it:

- a client sends a logical restore stream;
- a client receives a logical backup stream;
- a tool imports rows or records through an admitted route;
- a tool exports rows or records through an admitted route;
- a parser-support routine streams logical CDC or ETL records.

Denied or restricted by design:

- server-local file open or manipulation by a compatibility parser unless policy explicitly admits a safe surface;
- physical page-copy backup or restore formats;
- low-level repair, verification, or page maintenance through compatibility parser routes;
- operations targeting objects outside the connected workarea without explicit engine authority.

A logical stream can be interpreted as client work. A physical page copy or repair command attempts to bypass engine authority.

## Refusals Are Compatibility Behavior

A compatibility parser that fails clearly on unsupported behavior is doing its job correctly. A parser that silently accepts unsupported commands and returns success without doing the work is a defect, not a feature.

Common refusal classes:

- syntax not accepted by the selected parser;
- feature unsupported by the parser;
- feature unsupported by the engine build;
- operation denied by sandbox policy;
- operation denied because it requires server-local file access;
- operation denied because it is physical backup, restore, repair, or verification;
- unavailable parser UDR or bridge package;
- missing capability for CDC, replication, import, export, or migration;
- ambiguous ordering or transaction grouping in a stream.

## Reading Compatibility Claims

When evaluating a parser, look for proof of the exact surface you need rather than inferences from directory names or package manifests.

1. The parser package exists in the build output.
2. The parser can be configured and selected.
3. Connection and authentication tests pass.
4. The SQL or protocol surface you need is implemented.
5. Type, index, transaction, and catalog behavior match the parser's documented compatibility target.
6. The behavior is covered by reference-system regression tests or parser proof gates.
7. Unsupported cases return the documented message vector (a structured diagnostic record).

Do not infer readiness from file names, directory names, or generated manifests alone.

For procedures to configure and test compatibility parsers, see the full [Operations Administration](../../Operations_Administration/operating_modes_runbook.md) chapters.

## Where To Go Next

- [First Database](first_database.md)
- [Schemas, Objects, And Names](schemas_objects_and_names.md)
- [Engine Parser Boundary](../architecture/engine_parser_boundary.md)
- [SBsql And SBLR](../architecture/sbsql_and_sblr.md)
- [Language Reference](../../Language_Reference/README.md)
