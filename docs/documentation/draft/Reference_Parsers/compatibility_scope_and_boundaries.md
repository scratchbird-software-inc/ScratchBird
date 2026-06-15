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
  [Operations And Administration](../Operations_Administration/README.md) guide.)
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

- [How Reference Parsers Work](how_reference_parsers_work.md)
- [How Compatibility Behaviors Are Emulated](behavior_emulation.md)
- [Conformance And Compatibility Targets](conformance_and_status.md)
- [Backup, Restore, And Data Movement](../Operations_Administration/backup_restore_and_data_movement.md)
