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
  [Compatibility Scope And Boundaries](compatibility_scope_and_boundaries.md)).

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

- [How Reference Parsers Work](how_reference_parsers_work.md)
- [Compatibility Scope And Boundaries](compatibility_scope_and_boundaries.md)
- [Conformance And Compatibility Targets](conformance_and_status.md)
- [Reference Parser List](README.md)
