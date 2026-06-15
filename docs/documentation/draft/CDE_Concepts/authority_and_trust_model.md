# Authority And Trust Model

## Purpose

This page describes the single authority line that governs all operations in
ScratchBird, why SQL text is not authority, why outer layers are untreated as
untrusted toward data, and how the fail-closed principle and the cluster-boundary
constraint reinforce this model. Understanding this model is prerequisite to
understanding how the CDE design maintains correctness across many client
dialects and many data model families.

For the full security architecture, see
[../Security_Guide/trust_and_separation_architecture.md](../Security_Guide/trust_and_separation_architecture.md).
For the cluster boundary and provider-gated behavior, see
[../Language_Reference/core_paradigms/bridge_and_cluster_boundaries.md](../Language_Reference/core_paradigms/bridge_and_cluster_boundaries.md).
For the parser-engine separation that underpins this model, see
[dialect_plurality_and_parser_separation.md](dialect_plurality_and_parser_separation.md).

**This is a draft.** Nothing here is a security certification or audit statement.

---

## The Single Authority Line

Every operation that modifies data, reads data, or affects catalog objects in
ScratchBird is evaluated against four authority sources that form a single
chain:

1. **UUID / descriptor authority** — what object is being operated on.
   The `object_uuid` (from `CatalogObjectIdentity`) is the durable anchor.
   Names are resolved to UUIDs before any operation proceeds. An operation
   that cannot resolve its target to a valid UUID is refused.

2. **MGA visibility** — what versions of the data are visible to the
   current transaction snapshot. The engine's transaction inventory is the
   authority. No provider, index, parser, or external system can override
   this. Verified in `src/engine/internal_api/nosql/nosql_physical_provider_contract.hpp`:
   `authority_source = "engine_transaction_inventory"`.

3. **Materialized security policy** — what the current principal is
   permitted to do with the resolved object. Security policy is evaluated
   by the engine against materialized policy state; the result of a query
   through any client dialect is subject to the same security evaluation.

4. **SBLR internal representation** — execution authority flows through the
   engine's internal bound representation. The engine evaluates SBLR,
   not client text.

These four form the authority chain. Nothing outside this chain can override
any link in it.

---

## SQL Text Is Not Authority

This is one of the most important structural properties of the system: raw SQL
text — whether SBsql or any compatibility dialect — is never runtime authority.

Client text arrives at a parser. The parser's job is to translate that text into
SBLR. Once the SBLR is produced, the text is set aside. What the engine evaluates
is the SBLR, not the text. The original text is retained only as diagnostic
metadata for error messages and query identification.

This is encoded in the `SblrSourceArtifactMap` struct
(`src/engine/sblr/sblr_engine_envelope.hpp`):

```
bool raw_sql_text_authoritative = false;
bool contains_sql_text = false;
```

The fields default to `false`. SQL text carried in an SBLR envelope is explicitly
marked as non-authoritative. The engine does not re-parse or re-evaluate text
at runtime to determine what an operation means.

The practical consequence: an attacker who can inject text into a query channel
cannot use that text to claim engine authority. Authority comes from the UUID
resolution, MGA state, and security policy evaluation — all of which are
engine-side, not parser-side.

---

## Outer Layers Are Untrusted Toward Data

The engine treats all outer layers — client drivers, network listeners, parser
packages, manager processes — as untrusted toward data. This is not a statement
about whether those layers are well-written; it is a structural design choice
that means the engine does not rely on outer layers to enforce correctness or
security.

Specifically:

- **Parsers cannot claim transaction finality.** The `EngineNoSqlMgaRecheckProof`
  field `parser_claims_transaction_finality_authority` defaults to `false`.
  The enforcement function `RefuseParserAuthorityAttempt`
  (`src/engine/sblr/sblr_parser_authority_guards.hpp`) rejects any attempt
  to claim such authority. This applies to the native SBsql parser as much
  as to any compatibility parser.

- **Providers cannot claim visibility authority.** Fields
  `provider_claims_visibility_authority` and
  `provider_claims_transaction_finality_authority` in
  `EngineNoSqlProviderGenerationProof` default to `false`. A provider that
  asserts visibility authority produces `SB_NOSQL_PROVIDER_VISIBILITY_AUTHORITY_REFUSED`.

- **Indexes are candidates, not authorities.** An index result is a candidate
  set of rows. The engine rechecks each row for MGA visibility and security
  before delivering it.

- **Write-ahead log is not transaction authority.** The
  `write_ahead_log_claims_transaction_finality_authority` field defaults to
  `false` (marked `wal-not-authority` in the source).

- **The network listener and connection layer** cannot bypass security policy.
  Security is evaluated engine-side after SBLR arrives, not connection-side.

---

## Fail-Closed Behavior

When the engine encounters a situation where authority cannot be established —
a missing descriptor, an unresolvable UUID, a missing security proof, a
stale index generation, an unsupported provider family — the default behavior
is to refuse with a diagnostic, not to proceed with an approximate or
potentially incorrect result.

The `EngineNoSqlPhysicalProviderSelection` struct carries `fail_closed = true`
as its default, reflecting this policy at the provider selection level.

Diagnostic codes for failure cases are explicit and stable — for example:
`SB_NOSQL_PROVIDER_SECURITY_PROOF_MISSING`,
`SB_NOSQL_PROVIDER_DESCRIPTOR_NOT_VISIBLE_TO_SNAPSHOT`,
`SB_NOSQL_PROVIDER_INDEX_GENERATION_STALE`. These are refusals, not silent
degradations. An operator or caller can diagnose exactly why an operation
was refused.

---

## Time Is Not Transaction Authority

The time subsystem (`src/core/time/`) provides standardized time with explicit
uncertainty handling: wall clock time, monotonic time, and cluster-standardized
time are all available. Time is used for identity generation (UUIDv7 time
prefix), lease management, TTL expiry, and metrics — things that are useful
to coordinate but not semantically load-bearing for transaction correctness.

Time cannot prove commit finality. Whether a transaction committed is determined
by the engine's transaction inventory, not by any wall clock or logical clock
external to that inventory. A row whose commit timestamp is "in the past" is
not necessarily visible to a snapshot that predates the commit; the snapshot's
relationship to the transaction inventory is what determines visibility.

This matters for the CDE design because time-based reasoning about data
correctness is a common source of subtle bugs in distributed systems. ScratchBird
isolates this: time is a useful utility, not a correctness oracle.

---

## Provider-Gated Cluster Boundary

Cluster-scope behavior — distributed queries, cluster-scope agents, distributed
storage, cluster-aware admission control — routes through an external cluster
provider. In a non-cluster build, or in a build where the cluster provider is
absent, cluster operations fail closed with a diagnostic. They do not silently
degrade to local-only behavior; they are explicitly refused.

This is the provider-gated cluster boundary described in
[../Language_Reference/core_paradigms/bridge_and_cluster_boundaries.md](../Language_Reference/core_paradigms/bridge_and_cluster_boundaries.md).
The relevant diagnostic codes include
`SB_NOSQL_PROVIDER_CLUSTER_SCOPE_REFUSED_LOCAL_ONLY` and
`SB_NOSQL_PROVIDER_DISTRIBUTED_SCOPE_REFUSED_LOCAL_ONLY`.

The fail-closed principle applies here too: an operation that requires cluster
scope is refused, not silently executed as a local operation with potentially
different semantics.

---

## The Trust Model In The CDE Context

In a system that accepts many client dialects and supports many data model
families, a trust model that relies on any one of those layers to enforce
correctness would be fragile. A new compatibility parser, a new data provider,
or a new acceleration layer could each introduce a path that bypasses the
correctness model.

ScratchBird's response is to make the trust model *structural*: it is enforced
at the engine boundary by code (the MGA recheck, the parser authority guard,
the provider selection fail-closed logic), not by convention or by trusting
that each outer layer behaves correctly. The engine checks, and fails closed
if the check fails.

This is what makes it possible to add compatibility parsers and provider
families without systematically weakening the correctness guarantees that the
rest of the system depends on.
