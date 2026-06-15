# What Is A Convergent Data Engine?

## Purpose

This page defines the Convergent Data Engine (CDE) product class at
architectural altitude — deeper than the Getting Started introduction it builds
on, but still conceptual rather than a syntax or API reference. It explains
what converges in a CDE, what does not, and what the category explicitly refuses
to promise.

For a plain-language introduction with diagrams, see
[../Getting_Started/core_concepts/what_is_a_convergent_data_engine.md](../Getting_Started/core_concepts/what_is_a_convergent_data_engine.md).

**This is a draft.** Nothing here is a production readiness claim, compatibility
guarantee, or performance assertion.

---

## The Core Idea

A conventional database engine is a single-model, single-dialect system. It
stores data in one structural form — rows in tables, or documents in
collections, or nodes and edges, or key-value pairs — and it speaks one primary
language. When an application needs more than one model, it deploys more than
one product, then moves data between them, reconciles security boundaries,
and maintains separate backup and diagnostic disciplines.

A CDE explores whether that explosion of separate products can be largely
replaced by one engine substrate that natively understands many structural
forms, many client languages, and many operational disciplines at the same time.

The key word is *one engine substrate*. A CDE is not a proxy that routes
traffic to separate back-end products. It is a single authority — one
catalog, one transaction system, one security model, one storage engine —
that can be presented to the outside world through many different client
interfaces and can store data in many different structural forms without
silently translating or losing correctness properties.

---

## What Converges

A CDE converges the following concerns into one engine authority:

### Data Models

Many structural forms — relational rows, document trees, graph nodes and edges,
vector embeddings, time-series measurements, key-value pairs, spatial geometries,
full-text search indexes, columnar segments, probabilistic sketches, and
aggregate states — are all first-class types within the same type system. A
single row can contain columns of multiple model families. A single transaction
can touch data of multiple structural forms.

See [convergent_multi_model.md](convergent_multi_model.md) for the type
families and the invariant that makes convergence safe.

### Client Dialects

Many client languages and wire protocols — described by capability and category
rather than product name — are accepted as input. Each compatibility interface
runs through a separate parser package that lowers statements to a single engine
representation (SBLR). The engine itself is the authority, not the dialect.
Native SBsql is the engine's own language.

See [dialect_plurality_and_parser_separation.md](dialect_plurality_and_parser_separation.md).

### Transaction Authority

Every operation, regardless of which client dialect submitted it, passes through
the same multi-generational transaction system. Commit finality, snapshot
visibility, and rollback are decided by the engine, not by any parser or
provider. A write from a document-style interface and a write from a
relational-style interface participate in the same transaction inventory.

See [multi_generational_foundation.md](multi_generational_foundation.md).

### Security

Security policy, authentication, privilege grants, column masking, protected
material, and audit are engine-side concerns. They apply regardless of which
client dialect submitted the request. A query arriving through a document
interface is subject to the same security evaluation as one arriving through a
relational interface. There is no path that bypasses the engine's security
evaluation.

See [authority_and_trust_model.md](authority_and_trust_model.md) and the
[Security Guide](../Security_Guide/README.md).

### Operations: Backup, Restore, Diagnostics, Monitoring

Because there is one storage layer and one catalog, there is one backup-and-restore
discipline, one diagnostic surface, one health model, and one monitoring path.
Operators do not need separate tools per data model. The autonomous agent runtime
manages engine self-care tasks — cleanup, capacity, backup drills, tuning
recommendations — through a governed authority model that applies uniformly.

See [autonomous_operation.md](autonomous_operation.md) and the
[Operations and Administration Guide](../Operations_Administration/README.md).

### Catalog and Identity

Every engine object — tables, views, schemas, indexes, procedures, sequences,
policies, domains — has a UUID-anchored identity that is stable across renames
and moves. Names are labels; the UUID is the durable anchor. The recursive schema
tree organizes all objects under this identity model uniformly, regardless of
what structural form the data takes.

See [identity_and_recursive_schema.md](identity_and_recursive_schema.md).

---

## What Does Not Converge

Understanding what a CDE does not promise is as important as understanding what
it does.

### Dialects Do Not Become The Same Language

Accepting multiple client languages does not mean they become a single unified
query language. Each compatibility surface is scoped to what the engine can
support for that interface, and the boundaries are explicit. Requests that fall
outside the scope receive a diagnostic refusal — never a silent substitution.
Native SBsql is the fullest interface to the engine. Compatibility surfaces
cover the subset that can be safely translated.

### Compatibility Is Not Automatic or Complete

Supporting a client dialect or wire protocol does not mean 100% feature parity
with any other engine that speaks a similar language. Compatibility is scoped.
Features that the engine cannot support for a given interface are explicitly
refused with a diagnostic.

### Not All Model Families Are Complete In All Builds

Build configuration controls which type families, provider families, and
compatibility parsers are compiled in. A feature is available only when the
current build, configuration, tests, and release notes confirm it. This manual
makes no general availability claim.

### No Silent Behavior Substitution

If a request cannot be executed correctly under ScratchBird's correctness
guarantees, the engine refuses with a diagnostic. It does not silently
approximate, substitute a different algorithm, or return a result that would
violate MGA visibility or security policy.

### Not A Production Or Certification Claim

The CDE category label is descriptive and architectural. It is not a production
readiness certificate, a compliance attestation, a performance benchmark, or a
security accreditation. See [what_a_cde_is_not.md](what_a_cde_is_not.md).

---

## The Architectural Shape Of Convergence

The CDE design has a distinctive shape. Outer layers — client drivers, listener
network layer, parser packages, manager processes — are all treated as
*untrusted toward data*. They present requests but cannot confer authority.
The engine evaluates every request against:

1. **UUID/descriptor authority** — the catalog row uuid and object uuid establish
   what object is being operated on; names are resolved to UUIDs before any
   operation proceeds.
2. **MGA visibility** — the transaction snapshot determines what versions are
   visible; no accelerator or index result can override this.
3. **Security policy** — materialized security policy is evaluated by the engine;
   the result from any external provider or index is candidate evidence that the
   engine rechecks.
4. **SBLR internal representation** — all execution authority flows through the
   engine's bound internal representation; SQL text is never runtime authority.

This architecture is described fully in
[authority_and_trust_model.md](authority_and_trust_model.md). The invariant that
ties it to multi-model operation is described in
[convergent_multi_model.md](convergent_multi_model.md).
