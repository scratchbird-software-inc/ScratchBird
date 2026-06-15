# Convergent Multi-Model

## Purpose

This page explains how ScratchBird supports many data model families within a
single engine, and what structural guarantee makes that convergence correct
rather than merely convenient. The central concept is the
*candidate-evidence/MGA-recheck invariant*: results from any specialized
data provider or index are treated as candidates that the engine always
rechecks against its own transaction visibility and security authority before
returning them to a caller.

For the data type reference, see
[../Language_Reference/data_types/](../Language_Reference/data_types/).
For multi-model statement syntax, see
[../Language_Reference/syntax_reference/multimodel_statements.md](../Language_Reference/syntax_reference/multimodel_statements.md).

**This is a draft.** No claim here is a completeness guarantee for any
particular model family or build configuration.

---

## The Type System Foundation

ScratchBird's type system is organized around two parallel enumerations
verified in `src/core/datatypes/datatype_descriptor.hpp`:

- **28 TypeFamily values** (including `null_type` and `unknown`) that classify
  the structural nature of a value: `boolean`, `signed_integer`, `unsigned_integer`,
  `real`, `decimal`, `uuid`, `character`, `binary`, `bit_string`, `temporal`,
  `blob`, `network`, `document`, `search`, `structured`, `range`, `spatial`,
  `vector`, `graph`, `time_series`, `columnar`, `aggregate_state`, `sketch`,
  `locator`, `opaque`, `result_set`, and `unknown`.

- **87 CanonicalTypeId values** (not counting `unknown`) that identify specific
  concrete types within those families — for example, within the `document`
  family: `document`, `json_document`, `binary_json_document`, `bson_document`,
  `xml_document`, `hstore_document`, `object_document`, `flattened_object_document`;
  within the `vector` family: `vector`, `dense_vector`, `sparse_vector`,
  `binary_vector`, `quantized_vector`; within `graph`: `graph_node`, `graph_edge`,
  `graph_path`; within `search`: `token_stream`, `search_query`,
  `search_rank_feature`, `search_completion`, `search_percolator`; and so on.

These types are first-class. A table can have columns of mixed families. A single
transaction can write relational rows, document fields, graph edges, and vector
embeddings. The type system does not treat any family as a special case.

### The DatatypeDescriptor

Every type is described by a `DatatypeDescriptor` struct that carries the
canonical type id, family, width class, stable name, precision/scale
defaults, nullability, and two important flags:

- `descriptor_authoritative` — the descriptor, not any external system, is
  the canonical definition of the type.
- `reference_name_is_alias_only` — any name by which a type is known in a
  compatibility context is a label; the `CanonicalTypeId` and descriptor UUID
  are the authority.

This mirrors the broader identity philosophy of ScratchBird: names are labels,
structured identity is the authority.

---

## Provider Families For Specialized Execution

Supporting many data model families requires that some operations be executed
by specialized internal providers rather than a single generic evaluation path.
The engine defines eight provider families in
`src/engine/internal_api/nosql/nosql_physical_provider_contract.hpp`:

| Provider Family | Structural domain |
|-----------------|-------------------|
| `kKeyValue` | Key-value storage and retrieval |
| `kDocument` | Document and semi-structured storage |
| `kSearch` | Full-text and ranked search indexing |
| `kVector` | Dense, sparse, and quantized vector similarity |
| `kGraph` | Node and edge traversal |
| `kTimeSeries` | Time-ordered measurement storage |
| `kSpatial` | Geometric and geographic indexing |
| `kColumnar` | Column-oriented storage and aggregation |

Each family is an independent physical implementation within the engine. A
query that touches multiple model families may invoke multiple providers
within a single transaction.

---

## The Candidate-Evidence / MGA-Recheck Invariant

The structural guarantee that makes multi-model convergence correct is expressed
in the `EngineNoSqlMgaRecheckProof` struct:

```
struct EngineNoSqlMgaRecheckProof {
  bool proof_present = false;
  bool row_mga_recheck_required = true;      // default true
  bool row_security_recheck_required = true; // default true
  bool provider_claims_transaction_finality_authority = false;
  bool provider_claims_visibility_authority = false;
  bool index_claims_transaction_finality_authority = false;
  bool delta_overlay_claims_transaction_finality_authority = false;
  bool parser_claims_transaction_finality_authority = false;
  bool write_ahead_log_claims_transaction_finality_authority = false;
  std::string authority_source = "engine_transaction_inventory";
};
```

The defaults are telling: `row_mga_recheck_required = true` and
`row_security_recheck_required = true`. Every row returned by any specialized
provider is treated as *candidate evidence* that must be rechecked by the
engine itself before being delivered to the caller. No provider, no index, no
delta overlay, no parser, and no write-ahead mechanism can assert transaction
finality or visibility authority. The `authority_source` is always
`engine_transaction_inventory`.

This invariant is enforced structurally, not by convention. A provider that
attempts to claim visibility authority produces a diagnostic refusal
(`SB_NOSQL_PROVIDER_VISIBILITY_AUTHORITY_REFUSED`), not a silent override.

### What This Means In Practice

When you query a graph traversal, a vector similarity search, a full-text
ranked search, or a document lookup — and the result set overlaps with
rows that were written by a concurrent transaction, or rows that are outside
your current snapshot, or rows your security policy does not allow you to see
— the engine's MGA visibility recheck and security recheck will exclude those
rows from your result. The specialized provider cannot see your transaction
snapshot or your security context directly; the engine applies both
post-hoc on every candidate row.

This is the mechanism that makes it safe for a single query to join data
across model families: the transaction boundary and the security boundary
hold uniformly regardless of how each row was physically retrieved.

---

## Convergence Within A Single Catalog

All model families share one catalog. There is no separate catalog for the
document store, a separate one for the graph provider, and another for vectors.
A schema node in the recursive schema tree can parent objects of any model
family. A security policy bound to a schema applies to all objects within it,
regardless of their type family.

Backup, restore, and diagnostics operate on the unified catalog and unified
storage — there is no need for per-model-family backup tools.

---

## Build-Dependency Caveats

Some type families and provider families require mandatory library support that
may not be present in all build configurations. The `DatatypeDescriptor` carries
a `requires_mandatory_library` flag and `required_capability_key` to express
this. A build that does not include the required library will produce a
diagnostic on first use of the affected type, not a crash or silent degradation.

Feature availability should always be verified against the current build,
configuration, and release notes before relying on a specific model family
in a deployment.
