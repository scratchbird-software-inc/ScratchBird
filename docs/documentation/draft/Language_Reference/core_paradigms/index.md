# Core Paradigms Index

The core paradigms chapter explains the architectural rules that underpin every
statement in the SBsql language. Before reading the syntax or functional
reference, it is worth knowing how SBsql text becomes engine work, what MGA
transaction authority means, how catalog identity is stored, and where security
decisions are made. The pages in this chapter answer those questions in
conceptual terms so that the rest of the manual makes sense on first read.

These pages do not document statement syntax. They document the execution model
that syntax must satisfy.

## Pages In This Chapter

| Page | File | Use it for |
| --- | --- | --- |
| Intro And MGA Authority | [intro_and_mga.md](intro_and_mga.md) | Understanding what SBsql is, why the engine does not run SQL text directly, and what MGA controls. |
| Parser To SBLR Pipeline | [parser_to_sblr_pipeline.md](parser_to_sblr_pipeline.md) | Following the path from SBsql source text through tokenization, binding, lowering, and server admission to execution. |
| SBsql Language Profiles | [sbsql_language_profiles.md](sbsql_language_profiles.md) | Understanding how localized keyword spellings and locale-specific tools map onto the same canonical engine work. |
| UUID Catalog Identity | [uuid_catalog_identity.md](uuid_catalog_identity.md) | Understanding why catalog objects are identified by UUID rather than by name, and what that means for rename, migration, and dependency tracking. |
| Transactions And Recovery | [transactions_and_recovery.md](transactions_and_recovery.md) | Understanding MGA transaction lifecycle, snapshot rules, commit and rollback finality, and recovery classification. |
| Security And Sandboxing | [security_and_sandboxing.md](security_and_sandboxing.md) | Understanding how identity, roles, grants, sandbox roots, policy, masking, and fail-closed refusal fit together. |
| Bridge And Cluster Boundaries | [bridge_and_cluster_boundaries.md](bridge_and_cluster_boundaries.md) | Understanding the difference between local operations, bridge operations, and cluster-classified operations. |

## Suggested Reading Order

For a first reading, work through the pages in the order shown in the table
above. `intro_and_mga.md` introduces the vocabulary the other pages depend on.
`parser_to_sblr_pipeline.md` makes the flow concrete. The remaining pages can
be read in any order once those two are clear.

Readers who come to this chapter from an operational context (asking "why did
my statement get refused?") may want to go directly to `security_and_sandboxing.md`
or `transactions_and_recovery.md`.

## Back To The Chapter List

[Language Reference](../README.md)
