# Glossary

| Term | Meaning |
| --- | --- |
| CDE | Convergent Data Engine. A category term used by this guide for an engine design that brings multiple data, protocol, execution, and governance concerns into one engine substrate. |
| SBcore | ScratchBird embedded engine library. |
| SBsrv | ScratchBird local IPC server. |
| SBgate | ScratchBird listener and parser-facing entry point. |
| SBmgr | ScratchBird single-node manager. |
| SBsql | Native ScratchBird SQL language. |
| SBLR | ScratchBird engine-facing execution representation. |
| Parser package | A component that accepts a language or protocol and lowers admitted requests to ScratchBird execution requests. |
| Donor parser | A parser package for a specific donor database family or protocol surface. |
| Schema tree | Recursive namespace of schemas and child objects. |
| UUID identity | Durable engine identity for databases, schemas, objects, sessions, principals, and other authority-bearing objects. |
| MGA | ScratchBird transaction and visibility authority model. |
| Message vector | Structured diagnostic/refusal output. |
| Support bundle | Redacted diagnostic package for review or support. |
| Workarea | A schema-root area presented to a parser or user as its operating root. |
| Logical stream | Data movement represented as statements, rows, records, or events rather than page files. |
