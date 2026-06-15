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
  [How Compatibility Behaviors Are Emulated](behavior_emulation.md)),
- resolves names to the engine's UUID catalog identity, and
- refuses, with a clear diagnostic, anything it cannot honor as genuine
  compatibility or that falls in a blocked category (see
  [Compatibility Scope And Boundaries](compatibility_scope_and_boundaries.md)).

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

- [Reference Parser List](README.md)
- [How Reference Parsers Work](how_reference_parsers_work.md)
- [How Compatibility Behaviors Are Emulated](behavior_emulation.md)
- [Compatibility Scope And Boundaries](compatibility_scope_and_boundaries.md)
