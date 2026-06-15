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
[How Compatibility Behaviors Are Emulated](../behavior_emulation.md).

## Scope and boundaries

Like every reference parser, this one is compatibility-only and untrusted by the
engine. File and storage management, low-level or physical utilities, host
operating-system access, and any path that would bypass the engine are
deliberately blocked and refused with a diagnostic; unsupported or unsafe surface
is refused, not silently emulated. See
[Compatibility Scope And Boundaries](../compatibility_scope_and_boundaries.md).

## See also

- [How Reference Parsers Work](../how_reference_parsers_work.md)
- [Conformance And Compatibility Targets](../conformance_and_status.md)
- [Reference Parser List](../README.md)
- [Client And Driver Guide](../../Client_Driver_Guide/README.md)
