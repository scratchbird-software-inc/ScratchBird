# ScratchBird Documentation Library

Start here. This page is the front door to the ScratchBird documentation set. It
lists every book, who each is for, and a suggested reading order across books.

ScratchBird is a **Convergent Data Engine (CDE)**. The documentation is
organized into **volumes**, each published as its own downloadable book (PDF).
The very large SBsql reference is split into three companion volumes.

> This is **draft** documentation. A topic appearing here does not guarantee a
> feature is complete, enabled, performant, certified, or available in any
> particular build. Always confirm against the current build, configuration, and
> release notes. The ScratchBird engine is open source under MPL-2.0.

## The books

| Volume | Title | Read it for | Parts (source manuals) |
| --- | --- | --- | --- |
| 1 | Concepts and Getting Started | A first understanding; create a database and run a session | Getting Started, CDE Concepts, Functionality Matrix |
| 2a | SBsql Language Reference — Foundations, Data Types, and Catalog | The SBsql type system, core paradigms, catalog, and localized language support | Core Paradigms, Data Types, Catalog Reference, Language Support |
| 2b | SBsql Language Reference — Syntax | Statement and query syntax, procedural SQL, and the EBNF grammar | Syntax Reference |
| 2c | SBsql Language Reference — Functions | Built-in functions, operators, aggregates, and windows (delivered as one file per namespace group) | Functional Reference |
| 3 | Operations, Security, and Autonomy | Installing, running, securing, and self-managing a deployment | Operations and Administration, Security Guide, Agent Runtime Guide, Acceleration Guide |
| 4 | Application Development and Integration | Embedding the engine, using drivers, and AI / MCP integration | Embedding and API Reference, Client and Driver Guide, AI Integration Guide |
| 5 | Compatibility and Reference Parsers | Migrating from, or interoperating with, other databases | Reference Parsers |

A shared [Glossary](GLOSSARY.md) is included at the back of every volume.

## Suggested reading paths

**Evaluating ScratchBird**
1. Volume 1 — Concepts and Getting Started.
2. Volume 5 — Compatibility and Reference Parsers (if migrating).
3. Volume 3 — Operations, Security, and Autonomy (to understand running it).

**Writing SBsql**
1. Volume 1 — Concepts and Getting Started.
2. Volume 2a — Foundations, Data Types, and Catalog.
3. Volume 2b — Syntax, and Volume 2c — Functions (as references while you work).

**Building an application**
1. Volume 1 — Concepts and Getting Started.
2. Volume 4 — Application Development and Integration.
3. Volume 2b / 2c — Syntax and Functions (as references).

**Operating a deployment**
1. Volume 1 — Concepts and Getting Started.
2. Volume 3 — Operations, Security, and Autonomy.
3. Volume 5 — Compatibility and Reference Parsers (if hosting compatibility routes).

## How these books are built

Each volume is assembled from the source manuals in this directory by
`book_assembly/build.py`, which concatenates a volume's parts in reading order
with generated front matter and a shared glossary, and produces a print-ready
HTML for PDF rendering. The build is described in
[book_assembly/README.md](book_assembly/README.md).
