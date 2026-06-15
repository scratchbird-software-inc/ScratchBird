---
title: "ScratchBird — Compatibility and Reference Parsers"
---

# ScratchBird — Compatibility and Reference Parsers

*ScratchBird documentation — draft*

## Who this book is for

Teams migrating from, or interoperating with, other databases.

## About this book

This volume explains, in general terms, how ScratchBird's reference parsers provide compatibility with other database dialects and wire protocols, how behavior is emulated, and where the boundaries are.

## Parts in this volume

- **Reference Parsers**

> This is a **draft**. See *About This Documentation* at the end of this book for
> status and license. Confirm any specific behavior against the current build.

\newpage



# Reference Parsers




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/README.md -->

<a id="ch-reference-parsers-readme-md"></a>

# Reference Parsers (Compatibility Parsers)

## Purpose

This guide explains, in general terms, the **why** and **how** of ScratchBird's reference parsers — the compatibility surfaces that let clients built for other databases connect to ScratchBird using their own dialect and wire protocol. It is deliberately high-level: it describes the model, the behavior emulation, and the boundaries, not any source system's internals or a parser's statement-by-statement coverage.

Each reference parser is responsible for **one source dialect and one wire protocol**, exists for **compatibility only** (it adds no functionality and blends no other dialect), and is **untrusted** by the engine — accepted work is lowered to the engine's internal SBLR representation and rechecked there. The engine keeps all identity, transaction, storage, and security authority.

This is a **draft**; availability of any specific behavior still depends on the build, configuration, policy, and release notes.

## Read first

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md) — the model and the request flow.
- [How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md) — index, storage, and transaction/autocommit emulation over the engine.
- [Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md) — what compatibility covers and what is deliberately blocked.
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md) — what each parser is built to match.

## Reference parsers

Each parser presents one system's dialect, wire protocol, and behavior as compatibility, over a single ScratchBird engine.

| Parser | Category |
| --- | --- |
| [DuckDB](#ch-reference-parsers-parsers-duckdb-md) | Relational |
| [Firebird](#ch-reference-parsers-parsers-firebird-md) | Relational |
| [MariaDB](#ch-reference-parsers-parsers-mariadb-md) | Relational |
| [MySQL](#ch-reference-parsers-parsers-mysql-md) | Relational |
| [PostgreSQL](#ch-reference-parsers-parsers-postgresql-md) | Relational |
| [SQLite](#ch-reference-parsers-parsers-sqlite-md) | Relational |
| [ClickHouse](#ch-reference-parsers-parsers-clickhouse-md) | Analytical |
| [InfluxDB](#ch-reference-parsers-parsers-influxdb-md) | Analytical |
| [Milvus](#ch-reference-parsers-parsers-milvus-md) | Analytical |
| [OpenSearch](#ch-reference-parsers-parsers-opensearch-md) | Analytical |
| [OpenSearch SQL/PPL](#ch-reference-parsers-parsers-opensearch-sql-ppl-md) | Analytical |
| [Cassandra](#ch-reference-parsers-parsers-cassandra-md) | NoSQL |
| [MongoDB](#ch-reference-parsers-parsers-mongodb-md) | NoSQL |
| [Neo4j](#ch-reference-parsers-parsers-neo4j-md) | NoSQL |
| [Redis](#ch-reference-parsers-parsers-redis-md) | NoSQL |
| [XTDB](#ch-reference-parsers-parsers-xtdb-md) | NoSQL |
| [Apache Ignite](#ch-reference-parsers-parsers-apache-ignite-md) | Distributed |
| [CockroachDB](#ch-reference-parsers-parsers-cockroachdb-md) | Distributed |
| [Dolt](#ch-reference-parsers-parsers-dolt-md) | Distributed |
| [FoundationDB](#ch-reference-parsers-parsers-foundationdb-md) | Distributed |
| [immudb](#ch-reference-parsers-parsers-immudb-md) | Distributed |
| [TiDB](#ch-reference-parsers-parsers-tidb-md) | Distributed |
| [TiKV](#ch-reference-parsers-parsers-tikv-md) | Distributed |
| [Vitess](#ch-reference-parsers-parsers-vitess-md) | Distributed |
| [YugabyteDB](#ch-reference-parsers-parsers-yugabytedb-md) | Distributed |

## Related manuals

- CDE Concepts: Dialect Plurality And Parser Separation (ScratchBird — Concepts and Getting Started, page XXX)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX) — connection, wire, and authentication specifics.
- Security Guide: Trust And Separation Architecture (ScratchBird — Operations, Security, and Autonomy, page XXX) — why parsers are untrusted.
- Operations And Administration: Parser Registration And Routes (ScratchBird — Operations, Security, and Autonomy, page XXX)





===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/how_reference_parsers_work.md -->

<a id="ch-reference-parsers-how-reference-parsers-work-md"></a>

# How Reference Parsers Work

## Purpose

This page explains, in general terms, what a reference parser is and how it
works inside ScratchBird. It does not describe any source system's internals or
any parser's statement-by-statement coverage. The goal is to make the model
clear: why these parsers exist, what each one is responsible for, and where the
boundaries are.

## What a reference parser is

A **reference parser** (also called a compatibility parser) lets a client
written for another database connect to ScratchBird and speak that database's
own language. Each parser presents one source system's **client wire protocol**
and one source system's **SQL or query dialect**, accepts requests in that
form, and hands the accepted work to the ScratchBird engine.

The term "reference" means the parser is measured against a published
specification for the system it targets. One parser — the Firebird parser — is
the worked reference implementation that the others are built to match. See
[Conformance And Status](#ch-reference-parsers-conformance-and-status-md).

## One parser, one dialect, one wire protocol

The single most important rule of this design is narrowness:

- **One parser is responsible for exactly one source dialect and one wire
  protocol.** It does not try to be a general gateway.
- **Parsers do not borrow from each other.** A parser never blends another
  system's syntax, types, or behavior into its own surface. Each parser's
  cross-dialect dependency is none.
- **No parser adds new functionality.** A parser exists to translate, not to
  invent. It exposes only what its target dialect already offers; it does not
  extend, improve, or combine capabilities.

Compatibility is the only goal. If a client expects a particular dialect, the
matching parser is the one and only place that dialect is understood.

## How a request flows

At a high level, every reference parser does the same job in the same order:

1. **Negotiate** the source system's client wire protocol or API session.
2. **Authenticate** through the ScratchBird server path (the parser does not
   own authentication; it relays it).
3. **Parse** the request in the source dialect.
4. **Resolve** the names the client used to ScratchBird's durable UUID catalog
   identity, through the public catalog resolution surface.
5. **Lower** accepted work into SBLR — the engine's internal, bound request
   representation — and build the associated data packets.
6. **Return** engine results and diagnostics rendered back into the shape the
   client expects.

```text
client (source dialect + wire protocol)
        |
   reference parser   negotiate -> parse -> resolve names -> lower to SBLR
        |
   ScratchBird server   rechecks and admits (or refuses)
        |
   ScratchBird engine   owns identity, transactions (MGA), storage, security
```

## The parser is untrusted

A reference parser is **not** trusted by the engine. Whatever a parser
produces is rechecked by the server and the engine before anything happens:

- The engine's execution authority is its internal SBLR/MGA path only. A
  parser's output is treated as a request, never as a command that must be
  obeyed.
- A parser holds no storage authority, no transaction-finality authority, and
  no recovery authority. The engine owns all of those.
- Identity is the engine's UUID catalog. A parser maps visible names to UUIDs
  through the public resolution surface; it never becomes the source of
  identity.

Because the parser is untrusted and replaceable, a compatibility surface can be
added, corrected, or refused without weakening the engine. This is the same
trust-separation principle described in the
Security Guide (ScratchBird — Operations, Security, and Autonomy, page XXX).

## Where to go next

- [Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md) —
  what compatibility does and does not include, and what is deliberately blocked.
- [How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md) — how index,
  storage, and transaction behavior are mapped onto the engine.
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md) — what each
  parser is built to match.
- [Reference Parser List](#ch-reference-parsers-readme-md) — the full set of parsers and their status.
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX) — connection,
  wire, and authentication specifics.




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/compatibility_scope_and_boundaries.md -->

<a id="ch-reference-parsers-compatibility-scope-and-boundaries-md"></a>

# Compatibility Scope And Boundaries

## Purpose

This page describes, in general terms, what a reference parser's compatibility
covers and — just as importantly — what it deliberately does not. It is about
the shape of the boundary, not a list of individual statements.

## Compatibility is the only goal

A reference parser exists so that an existing client can talk to ScratchBird
using the dialect and wire protocol it already speaks. That is the whole
purpose. It follows that:

- A parser exposes its target dialect's behavior **as compatibility**, not as a
  new product surface.
- A parser does **not** add features, options, or syntax beyond what its target
  dialect defines.
- A parser does **not** mix in another dialect's behavior. Each parser stays
  inside the single dialect it is responsible for.

When a client uses a feature the target dialect supports and ScratchBird can
honor through its engine, the parser lowers that work to the engine. When a
client uses something the parser cannot honor as genuine compatibility, the
parser **refuses with a clear diagnostic** rather than silently pretending to
succeed or quietly substituting different behavior.

## The engine keeps authority

No matter which dialect a client speaks, durable behavior is owned by the
ScratchBird engine, not by the parser:

- Transactions, visibility, and finality are owned by the engine's
  Multi-Generational Architecture.
- Identity is the engine's UUID catalog.
- The parser's accepted output (SBLR) is rechecked and admitted by the engine
  before anything becomes durable.

A parser presents a familiar surface; it does not change who is in charge
underneath it.

## What is deliberately blocked

Some categories of a source system's surface are intentionally **not** offered
through a compatibility parser, because they fall outside the engine's
authority model or would breach its security and storage boundaries. These are
refused with diagnostics rather than emulated. In general terms, the blocked
categories include:

- **File and storage management** — operations that would read, write, place,
  or manipulate database files, pages, or on-disk structures directly.
- **Low-level and physical utilities** — physical backup/repair/validation
  tooling and other low-level maintenance surfaces of the source system. (The
  engine provides its own logical backup, restore, and maintenance through its
  own authorized surfaces; see the
  Operations And Administration (ScratchBird — Operations, Security, and Autonomy, page XXX) guide.)
- **Host operating-system access** — anything that would run programs, reach the
  host filesystem, or otherwise act outside the database boundary.
- **Direct storage or page access** — any path that would bypass the engine and
  touch storage directly.
- **Authority bypass** — any request that asserts it should own execution,
  transaction finality, recovery, or identity. Those belong to the engine.

The principle is consistent: a compatibility parser may present the *shape* of a
source dialect, but it cannot become a back door around the engine's
transaction, security, or storage authority. Where a source system exposes such
a back door, the compatibility parser closes it.

## Unsupported is refused, not faked

A surface that is not yet implemented, not safe to honor, or not permitted by
policy is refused with a diagnostic identifying the reason. ScratchBird does not
silently approximate a behavior it cannot faithfully provide. This keeps the
compatibility promise honest: a request either runs as genuine compatibility or
it returns a clear refusal.

## Where to go next

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- Backup, Restore, And Data Movement (ScratchBird — Operations, Security, and Autonomy, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/behavior_emulation.md -->

<a id="ch-reference-parsers-behavior-emulation-md"></a>

# How Compatibility Behaviors Are Emulated

## Purpose

A reference parser does more than translate syntax. Clients expect their source
database to *behave* in particular ways — how an index treats keys, how a
statement commits, how a type rounds. ScratchBird has one engine with one set of
rules, so the parser's job is to make that one engine present the **behavior** a
client expects. This page explains, in general terms, how that emulation works.

The key idea: ScratchBird does not run a different engine for each dialect. It
runs its own engine and **maps the emulated system's behavioral assumptions onto
it**. The same data, in the same logical structure, can be made to behave the
way each emulated system's clients expect.

## Behavior is emulated; authority is not

Emulation changes how things *look and behave* to the client. It never changes
who is in charge underneath:

- Identity is always ScratchBird's UUID catalog.
- Transactions, visibility, and finality are always owned by the engine's
  Multi-Generational Architecture (MGA).
- The blocked categories still apply (see
  [Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md)).

Within those limits, the parser shapes behavior to match the emulated system.

## Storage and index behavior

Two systems can offer "the same" index type — a B-tree, for example — and still
disagree about what that index means. They make different assumptions about
things such as:

- how null keys are ordered and whether they are indexed at all,
- how text keys compare (collation, case sensitivity, accent sensitivity),
- how uniqueness treats nulls and duplicate keys,
- key ordering and how the index is filled and maintained.

Because of these differences, an index created over the same data, using the
same logical index type, may legitimately be **stored on disk differently**
depending on which system is being emulated. To handle this, index creation
carries **behavioral settings** that tell the engine which assumptions to apply.
When a client of an emulated system creates, say, a B-tree, the parser supplies
the settings that make ScratchBird's index behave — and store itself — the way
that system's B-tree behaves.

The result: clients of different emulated systems can each create "a normal
index" and each get the behavior their ecosystem assumes, from one engine, over
one copy of the data.

## Transactions and autocommit

ScratchBird is **always inside a transaction**. There is no way to do anything
outside of a transaction — every statement runs within transaction context owned
by MGA. Emulation has to present each source system's transaction model on top
of that always-transactional foundation:

- **Systems with autocommit.** Many systems default to autocommit, where each
  statement is its own committed unit. ScratchBird emulates this by **committing
  and starting a new transaction around every statement**. This faithfully
  reproduces the per-statement commit behavior a client expects, but it is
  **slow** compared with grouping work into an explicit transaction — every
  statement pays for a commit. Clients that care about throughput should use
  explicit transactions where the source dialect allows it.
- **Systems with explicit transactions.** Where the source dialect manages
  transactions explicitly, the parser maps those begin/commit/rollback requests
  onto the engine's transaction control directly.
- **Systems with no transactions at all.** Some emulated systems have no concept
  of transactions. ScratchBird cannot run "outside" a transaction, so such work
  still executes inside the engine's always-on transaction model; the parser
  emulates the source's non-transactional expectations on top of it (typically
  by committing per operation, as with autocommit). The client sees the
  non-transactional behavior it expects; the engine still gets transactional
  safety underneath.

The trade-off is worth stating plainly: emulating per-statement commit behavior
is correct but costs performance. The closer a client can stay to explicit
transactions, the better it will perform.

## Other behavioral differences

The same approach applies to other places where systems disagree — for example
default values and implicit conversions, how types round or range-check, sort
ordering, and the shape and wording of diagnostics. In each case the parser
maps the emulated system's expected behavior onto the engine's own behavior, so
the client sees results consistent with its source system, while the engine
applies its own durable, authoritative rules underneath.

## Where to go next

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/conformance_and_status.md -->

<a id="ch-reference-parsers-conformance-and-status-md"></a>

# Conformance And Compatibility Targets

## Purpose

This page explains, in general terms, what each reference parser conforms to and
what "compatibility" guarantees for a client. It is about the target a parser
matches, not a maturity ranking.

## Each parser targets a published baseline

Every reference parser is built to conform to a published baseline of the system
it emulates — its dialect, its wire protocol, and its behavior. Conformance is
defined against that baseline: a conformant parser presents that system's
behavior faithfully and refuses, with a diagnostic, anything it cannot honor as
genuine compatibility.

Targeting a defined baseline keeps compatibility precise.

## What conformance covers

Conformance is about faithful behavior, not just accepted syntax. A conformant
reference parser:

- accepts its target dialect and speaks its target wire protocol,
- maps the target system's **behavior** onto the engine — including index and
  storage behavior and transaction/autocommit semantics (see
  [How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md)),
- resolves names to the engine's UUID catalog identity, and
- refuses, with a clear diagnostic, anything it cannot honor as genuine
  compatibility or that falls in a blocked category (see
  [Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md)).

## What conformance does not promise

Conformance describes what a parser is built to match. It does not, by itself,
guarantee that a particular behavior is enabled in a particular deployment.
Actual availability always depends on:

- the build and platform,
- configuration and which parser routes are enabled,
- security policy, and
- the current release notes.

Always confirm against the current build before relying on a specific
compatibility behavior.

## Where to go next

- [Reference Parser List](#ch-reference-parsers-readme-md)
- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md)
- [Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/apache_ignite.md -->

<a id="ch-reference-parsers-parsers-apache-ignite-md"></a>

# Apache Ignite Compatibility Parser

## Purpose

The Apache Ignite compatibility parser lets a client built for Apache Ignite — a distributed in-memory data platform — connect
to ScratchBird using the Apache Ignite dialect and wire protocol. It is a
**compatibility surface only**: it presents Apache Ignite's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | Apache Ignite |
| Category | Distributed |
| Client surface | Native Apache Ignite client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the Apache Ignite dialect |

## What it does

In general terms, this parser negotiates the Apache Ignite client session, parses the
Apache Ignite dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a Apache Ignite
client expects. It adds no functionality beyond Apache Ignite compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps Apache Ignite's expected behavior onto the engine — including index and
storage behavior and Apache Ignite's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a Apache Ignite client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/cassandra.md -->

<a id="ch-reference-parsers-parsers-cassandra-md"></a>

# Cassandra Compatibility Parser

## Purpose

The Cassandra compatibility parser lets a client built for Cassandra — a wide-column NoSQL database — connect
to ScratchBird using the Cassandra dialect and wire protocol. It is a
**compatibility surface only**: it presents Cassandra's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | Cassandra |
| Category | NoSQL |
| Client surface | Native Cassandra client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the Cassandra dialect |

## What it does

In general terms, this parser negotiates the Cassandra client session, parses the
Cassandra dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a Cassandra
client expects. It adds no functionality beyond Cassandra compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps Cassandra's expected behavior onto the engine — including index and
storage behavior and Cassandra's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a Cassandra client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/clickhouse.md -->

<a id="ch-reference-parsers-parsers-clickhouse-md"></a>

# ClickHouse Compatibility Parser

## Purpose

The ClickHouse compatibility parser lets a client built for ClickHouse — an analytical columnar SQL database — connect
to ScratchBird using the ClickHouse dialect and wire protocol. It is a
**compatibility surface only**: it presents ClickHouse's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | ClickHouse |
| Category | Analytical |
| Client surface | Native ClickHouse client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the ClickHouse dialect |

## What it does

In general terms, this parser negotiates the ClickHouse client session, parses the
ClickHouse dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a ClickHouse
client expects. It adds no functionality beyond ClickHouse compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps ClickHouse's expected behavior onto the engine — including index and
storage behavior and ClickHouse's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a ClickHouse client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/cockroachdb.md -->

<a id="ch-reference-parsers-parsers-cockroachdb-md"></a>

# CockroachDB Compatibility Parser

## Purpose

The CockroachDB compatibility parser lets a client built for CockroachDB — a distributed SQL database — connect
to ScratchBird using the CockroachDB dialect and wire protocol. It is a
**compatibility surface only**: it presents CockroachDB's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | CockroachDB |
| Category | Distributed |
| Client surface | Native CockroachDB client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the CockroachDB dialect |

## What it does

In general terms, this parser negotiates the CockroachDB client session, parses the
CockroachDB dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a CockroachDB
client expects. It adds no functionality beyond CockroachDB compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps CockroachDB's expected behavior onto the engine — including index and
storage behavior and CockroachDB's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a CockroachDB client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/dolt.md -->

<a id="ch-reference-parsers-parsers-dolt-md"></a>

# Dolt Compatibility Parser

## Purpose

The Dolt compatibility parser lets a client built for Dolt — a version-controlled SQL database — connect
to ScratchBird using the Dolt dialect and wire protocol. It is a
**compatibility surface only**: it presents Dolt's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | Dolt |
| Category | Distributed |
| Client surface | Native Dolt client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the Dolt dialect |

## What it does

In general terms, this parser negotiates the Dolt client session, parses the
Dolt dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a Dolt
client expects. It adds no functionality beyond Dolt compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps Dolt's expected behavior onto the engine — including index and
storage behavior and Dolt's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a Dolt client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/duckdb.md -->

<a id="ch-reference-parsers-parsers-duckdb-md"></a>

# DuckDB Compatibility Parser

## Purpose

The DuckDB compatibility parser lets a client built for DuckDB — an embedded analytical (columnar) SQL database — connect
to ScratchBird using the DuckDB dialect and wire protocol. It is a
**compatibility surface only**: it presents DuckDB's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | DuckDB |
| Category | Relational |
| Client surface | Native DuckDB client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the DuckDB dialect |

## What it does

In general terms, this parser negotiates the DuckDB client session, parses the
DuckDB dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a DuckDB
client expects. It adds no functionality beyond DuckDB compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps DuckDB's expected behavior onto the engine — including index and
storage behavior and DuckDB's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a DuckDB client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/firebird.md -->

<a id="ch-reference-parsers-parsers-firebird-md"></a>

# Firebird Compatibility Parser

## Purpose

The Firebird compatibility parser lets a client built for Firebird — a relational SQL database — connect
to ScratchBird using the Firebird dialect and wire protocol. It is a
**compatibility surface only**: it presents Firebird's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | Firebird |
| Category | Relational |
| Client surface | Native Firebird client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the Firebird dialect |

## What it does

In general terms, this parser negotiates the Firebird client session, parses the
Firebird dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a Firebird
client expects. It adds no functionality beyond Firebird compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps Firebird's expected behavior onto the engine — including index and
storage behavior and Firebird's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a Firebird client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/foundationdb.md -->

<a id="ch-reference-parsers-parsers-foundationdb-md"></a>

# FoundationDB Compatibility Parser

## Purpose

The FoundationDB compatibility parser lets a client built for FoundationDB — a distributed transactional key-value store — connect
to ScratchBird using the FoundationDB dialect and wire protocol. It is a
**compatibility surface only**: it presents FoundationDB's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | FoundationDB |
| Category | Distributed |
| Client surface | Native FoundationDB client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the FoundationDB dialect |

## What it does

In general terms, this parser negotiates the FoundationDB client session, parses the
FoundationDB dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a FoundationDB
client expects. It adds no functionality beyond FoundationDB compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps FoundationDB's expected behavior onto the engine — including index and
storage behavior and FoundationDB's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a FoundationDB client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/immudb.md -->

<a id="ch-reference-parsers-parsers-immudb-md"></a>

# immudb Compatibility Parser

## Purpose

The immudb compatibility parser lets a client built for immudb — an immutable, append-only database — connect
to ScratchBird using the immudb dialect and wire protocol. It is a
**compatibility surface only**: it presents immudb's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | immudb |
| Category | Distributed |
| Client surface | Native immudb client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the immudb dialect |

## What it does

In general terms, this parser negotiates the immudb client session, parses the
immudb dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a immudb
client expects. It adds no functionality beyond immudb compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps immudb's expected behavior onto the engine — including index and
storage behavior and immudb's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a immudb client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/influxdb.md -->

<a id="ch-reference-parsers-parsers-influxdb-md"></a>

# InfluxDB Compatibility Parser

## Purpose

The InfluxDB compatibility parser lets a client built for InfluxDB — a time-series database — connect
to ScratchBird using the InfluxDB dialect and wire protocol. It is a
**compatibility surface only**: it presents InfluxDB's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | InfluxDB |
| Category | Analytical |
| Client surface | Native InfluxDB client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the InfluxDB dialect |

## What it does

In general terms, this parser negotiates the InfluxDB client session, parses the
InfluxDB dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a InfluxDB
client expects. It adds no functionality beyond InfluxDB compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps InfluxDB's expected behavior onto the engine — including index and
storage behavior and InfluxDB's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a InfluxDB client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/mariadb.md -->

<a id="ch-reference-parsers-parsers-mariadb-md"></a>

# MariaDB Compatibility Parser

## Purpose

The MariaDB compatibility parser lets a client built for MariaDB — a relational SQL database — connect
to ScratchBird using the MariaDB dialect and wire protocol. It is a
**compatibility surface only**: it presents MariaDB's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | MariaDB |
| Category | Relational |
| Client surface | Native MariaDB client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the MariaDB dialect |

## What it does

In general terms, this parser negotiates the MariaDB client session, parses the
MariaDB dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a MariaDB
client expects. It adds no functionality beyond MariaDB compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps MariaDB's expected behavior onto the engine — including index and
storage behavior and MariaDB's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a MariaDB client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/milvus.md -->

<a id="ch-reference-parsers-parsers-milvus-md"></a>

# Milvus Compatibility Parser

## Purpose

The Milvus compatibility parser lets a client built for Milvus — a vector database — connect
to ScratchBird using the Milvus dialect and wire protocol. It is a
**compatibility surface only**: it presents Milvus's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | Milvus |
| Category | Analytical |
| Client surface | Native Milvus client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the Milvus dialect |

## What it does

In general terms, this parser negotiates the Milvus client session, parses the
Milvus dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a Milvus
client expects. It adds no functionality beyond Milvus compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps Milvus's expected behavior onto the engine — including index and
storage behavior and Milvus's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a Milvus client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/mongodb.md -->

<a id="ch-reference-parsers-parsers-mongodb-md"></a>

# MongoDB Compatibility Parser

## Purpose

The MongoDB compatibility parser lets a client built for MongoDB — a document NoSQL database — connect
to ScratchBird using the MongoDB dialect and wire protocol. It is a
**compatibility surface only**: it presents MongoDB's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | MongoDB |
| Category | NoSQL |
| Client surface | Native MongoDB client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the MongoDB dialect |

## What it does

In general terms, this parser negotiates the MongoDB client session, parses the
MongoDB dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a MongoDB
client expects. It adds no functionality beyond MongoDB compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps MongoDB's expected behavior onto the engine — including index and
storage behavior and MongoDB's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a MongoDB client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/mysql.md -->

<a id="ch-reference-parsers-parsers-mysql-md"></a>

# MySQL Compatibility Parser

## Purpose

The MySQL compatibility parser lets a client built for MySQL — a relational SQL database — connect
to ScratchBird using the MySQL dialect and wire protocol. It is a
**compatibility surface only**: it presents MySQL's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | MySQL |
| Category | Relational |
| Client surface | Native MySQL client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the MySQL dialect |

## What it does

In general terms, this parser negotiates the MySQL client session, parses the
MySQL dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a MySQL
client expects. It adds no functionality beyond MySQL compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps MySQL's expected behavior onto the engine — including index and
storage behavior and MySQL's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a MySQL client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/neo4j.md -->

<a id="ch-reference-parsers-parsers-neo4j-md"></a>

# Neo4j Compatibility Parser

## Purpose

The Neo4j compatibility parser lets a client built for Neo4j — a graph database — connect
to ScratchBird using the Neo4j dialect and wire protocol. It is a
**compatibility surface only**: it presents Neo4j's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | Neo4j |
| Category | NoSQL |
| Client surface | Native Neo4j client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the Neo4j dialect |

## What it does

In general terms, this parser negotiates the Neo4j client session, parses the
Neo4j dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a Neo4j
client expects. It adds no functionality beyond Neo4j compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps Neo4j's expected behavior onto the engine — including index and
storage behavior and Neo4j's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a Neo4j client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/opensearch.md -->

<a id="ch-reference-parsers-parsers-opensearch-md"></a>

# OpenSearch Compatibility Parser

## Purpose

The OpenSearch compatibility parser lets a client built for OpenSearch — a search and analytics engine — connect
to ScratchBird using the OpenSearch dialect and wire protocol. It is a
**compatibility surface only**: it presents OpenSearch's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | OpenSearch |
| Category | Analytical |
| Client surface | Native OpenSearch client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the OpenSearch dialect |

## What it does

In general terms, this parser negotiates the OpenSearch client session, parses the
OpenSearch dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a OpenSearch
client expects. It adds no functionality beyond OpenSearch compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps OpenSearch's expected behavior onto the engine — including index and
storage behavior and OpenSearch's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a OpenSearch client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/opensearch_sql_ppl.md -->

<a id="ch-reference-parsers-parsers-opensearch-sql-ppl-md"></a>

# OpenSearch SQL/PPL Compatibility Parser

## Purpose

The OpenSearch SQL/PPL compatibility parser lets a client built for OpenSearch SQL/PPL — the SQL and PPL query interface for a search and analytics engine — connect
to ScratchBird using the OpenSearch SQL/PPL dialect and wire protocol. It is a
**compatibility surface only**: it presents OpenSearch SQL/PPL's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | OpenSearch SQL/PPL |
| Category | Analytical |
| Client surface | Native OpenSearch SQL/PPL client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the OpenSearch SQL/PPL dialect |

## What it does

In general terms, this parser negotiates the OpenSearch SQL/PPL client session, parses the
OpenSearch SQL/PPL dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a OpenSearch SQL/PPL
client expects. It adds no functionality beyond OpenSearch SQL/PPL compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps OpenSearch SQL/PPL's expected behavior onto the engine — including index and
storage behavior and OpenSearch SQL/PPL's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a OpenSearch SQL/PPL client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/postgresql.md -->

<a id="ch-reference-parsers-parsers-postgresql-md"></a>

# PostgreSQL Compatibility Parser

## Purpose

The PostgreSQL compatibility parser lets a client built for PostgreSQL — a relational SQL database — connect
to ScratchBird using the PostgreSQL dialect and wire protocol. It is a
**compatibility surface only**: it presents PostgreSQL's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | PostgreSQL |
| Category | Relational |
| Client surface | Native PostgreSQL client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the PostgreSQL dialect |

## What it does

In general terms, this parser negotiates the PostgreSQL client session, parses the
PostgreSQL dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a PostgreSQL
client expects. It adds no functionality beyond PostgreSQL compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps PostgreSQL's expected behavior onto the engine — including index and
storage behavior and PostgreSQL's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a PostgreSQL client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/redis.md -->

<a id="ch-reference-parsers-parsers-redis-md"></a>

# Redis Compatibility Parser

## Purpose

The Redis compatibility parser lets a client built for Redis — a key-value store — connect
to ScratchBird using the Redis dialect and wire protocol. It is a
**compatibility surface only**: it presents Redis's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | Redis |
| Category | NoSQL |
| Client surface | Native Redis client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the Redis dialect |

## What it does

In general terms, this parser negotiates the Redis client session, parses the
Redis dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a Redis
client expects. It adds no functionality beyond Redis compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps Redis's expected behavior onto the engine — including index and
storage behavior and Redis's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a Redis client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/sqlite.md -->

<a id="ch-reference-parsers-parsers-sqlite-md"></a>

# SQLite Compatibility Parser

## Purpose

The SQLite compatibility parser lets a client built for SQLite — an embedded relational SQL database — connect
to ScratchBird using the SQLite dialect and wire protocol. It is a
**compatibility surface only**: it presents SQLite's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | SQLite |
| Category | Relational |
| Client surface | Native SQLite client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the SQLite dialect |

## What it does

In general terms, this parser negotiates the SQLite client session, parses the
SQLite dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a SQLite
client expects. It adds no functionality beyond SQLite compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps SQLite's expected behavior onto the engine — including index and
storage behavior and SQLite's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a SQLite client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/tidb.md -->

<a id="ch-reference-parsers-parsers-tidb-md"></a>

# TiDB Compatibility Parser

## Purpose

The TiDB compatibility parser lets a client built for TiDB — a distributed SQL database — connect
to ScratchBird using the TiDB dialect and wire protocol. It is a
**compatibility surface only**: it presents TiDB's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | TiDB |
| Category | Distributed |
| Client surface | Native TiDB client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the TiDB dialect |

## What it does

In general terms, this parser negotiates the TiDB client session, parses the
TiDB dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a TiDB
client expects. It adds no functionality beyond TiDB compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps TiDB's expected behavior onto the engine — including index and
storage behavior and TiDB's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a TiDB client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/tikv.md -->

<a id="ch-reference-parsers-parsers-tikv-md"></a>

# TiKV Compatibility Parser

## Purpose

The TiKV compatibility parser lets a client built for TiKV — a distributed transactional key-value store — connect
to ScratchBird using the TiKV dialect and wire protocol. It is a
**compatibility surface only**: it presents TiKV's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | TiKV |
| Category | Distributed |
| Client surface | Native TiKV client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the TiKV dialect |

## What it does

In general terms, this parser negotiates the TiKV client session, parses the
TiKV dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a TiKV
client expects. It adds no functionality beyond TiKV compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps TiKV's expected behavior onto the engine — including index and
storage behavior and TiKV's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a TiKV client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/vitess.md -->

<a id="ch-reference-parsers-parsers-vitess-md"></a>

# Vitess Compatibility Parser

## Purpose

The Vitess compatibility parser lets a client built for Vitess — a horizontally-sharded (scale-out) SQL system — connect
to ScratchBird using the Vitess dialect and wire protocol. It is a
**compatibility surface only**: it presents Vitess's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | Vitess |
| Category | Distributed |
| Client surface | Native Vitess client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the Vitess dialect |

## What it does

In general terms, this parser negotiates the Vitess client session, parses the
Vitess dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a Vitess
client expects. It adds no functionality beyond Vitess compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps Vitess's expected behavior onto the engine — including index and
storage behavior and Vitess's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a Vitess client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/xtdb.md -->

<a id="ch-reference-parsers-parsers-xtdb-md"></a>

# XTDB Compatibility Parser

## Purpose

The XTDB compatibility parser lets a client built for XTDB — a bitemporal document database — connect
to ScratchBird using the XTDB dialect and wire protocol. It is a
**compatibility surface only**: it presents XTDB's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | XTDB |
| Category | NoSQL |
| Client surface | Native XTDB client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the XTDB dialect |

## What it does

In general terms, this parser negotiates the XTDB client session, parses the
XTDB dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a XTDB
client expects. It adds no functionality beyond XTDB compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps XTDB's expected behavior onto the engine — including index and
storage behavior and XTDB's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a XTDB client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Reference_Parsers/parsers/yugabytedb.md -->

<a id="ch-reference-parsers-parsers-yugabytedb-md"></a>

# YugabyteDB Compatibility Parser

## Purpose

The YugabyteDB compatibility parser lets a client built for YugabyteDB — a distributed SQL database — connect
to ScratchBird using the YugabyteDB dialect and wire protocol. It is a
**compatibility surface only**: it presents YugabyteDB's own language and behavior to
existing clients and translates accepted work into the ScratchBird engine, which
keeps all authority.

| Property | Value |
| --- | --- |
| Target system | YugabyteDB |
| Category | Distributed |
| Client surface | Native YugabyteDB client wire protocol or API session |
| Authority | Engine SBLR/MGA only — the parser holds no execution, storage, transaction-finality, or recovery authority |
| Cross-dialect dependency | None — this parser implements only the YugabyteDB dialect |

## What it does

In general terms, this parser negotiates the YugabyteDB client session, parses the
YugabyteDB dialect, resolves the names a client uses to ScratchBird's UUID catalog
identity, and lowers accepted requests into the engine's internal SBLR
representation. Results and diagnostics are rendered back into the form a YugabyteDB
client expects. It adds no functionality beyond YugabyteDB compatibility and does
not blend in any other dialect.

## Behavior emulation

The parser maps YugabyteDB's expected behavior onto the engine — including index and
storage behavior and YugabyteDB's transaction and autocommit model over
ScratchBird's always-in-a-transaction foundation — so a YugabyteDB client sees
familiar behavior while the engine applies its own durable rules underneath. See
[How Compatibility Behaviors Are Emulated](#ch-reference-parsers-behavior-emulation-md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](#ch-reference-parsers-compatibility-scope-and-boundaries-md).

## See also

- [How Reference Parsers Work](#ch-reference-parsers-how-reference-parsers-work-md)
- [Conformance And Compatibility Targets](#ch-reference-parsers-conformance-and-status-md)
- [Reference Parser List](#ch-reference-parsers-readme-md)
- Client And Driver Guide (ScratchBird — Application Development and Integration, page XXX)




<a id="ch-glossary"></a>

# Glossary

## Purpose

This glossary defines terms used across the ScratchBird documentation set. The
definitions are written for end users, evaluators, operators, and developers.
They are intentionally concise and cautious: a term appearing here does not mean
the related feature is complete, enabled, or available in every build.

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



# About This Documentation

This book is part of the ScratchBird documentation set. ScratchBird is a
Convergent Data Engine (CDE).

**Draft status.** This is draft documentation. It describes the architecture and
intended behavior of the source tree. A topic appearing here does not by itself
guarantee that a feature is complete, enabled, performant, certified, or
available in any particular build. Always confirm against the current build,
configuration, tests, and release notes.

**License.** The ScratchBird engine is distributed under the Mozilla Public
License 2.0 (MPL-2.0). This documentation describes that open-source engine.

**No certification claim.** Nothing in this documentation constitutes a security
certification, performance benchmark, or compatibility guarantee.
