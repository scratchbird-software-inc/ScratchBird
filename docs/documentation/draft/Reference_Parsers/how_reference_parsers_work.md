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
[Conformance And Status](conformance_and_status.md).

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
[Security Guide](../Security_Guide/trust_and_separation_architecture.md).

## Where to go next

- [Compatibility Scope And Boundaries](compatibility_scope_and_boundaries.md) —
  what compatibility does and does not include, and what is deliberately blocked.
- [How Compatibility Behaviors Are Emulated](behavior_emulation.md) — how index,
  storage, and transaction behavior are mapped onto the engine.
- [Conformance And Compatibility Targets](conformance_and_status.md) — what each
  parser is built to match.
- [Reference Parser List](README.md) — the full set of parsers and their status.
- [Client And Driver Guide](../Client_Driver_Guide/README.md) — connection,
  wire, and authentication specifics.
