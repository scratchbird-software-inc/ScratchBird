# Identity And Recursive Schema

## Purpose

This page explains how ScratchBird assigns durable identity to every engine
object, how that identity is organized in a recursive schema tree, and why
this design enables rename, move, and multi-dialect compatibility projection
without breaking references. Identity is foundational to the CDE design: it
is what allows many client dialects, many data model families, and many
operational tools to refer to the same objects through different name
conventions without diverging on what object is actually meant.

For the full UUID identity reference, see
[../Language_Reference/core_paradigms/uuid_catalog_identity.md](../Language_Reference/core_paradigms/uuid_catalog_identity.md).
For the recursive schema tree detail, see
[../Getting_Started/architecture/recursive_schema_tree.md](../Getting_Started/architecture/recursive_schema_tree.md).

**This is a draft.** Nothing here is a compatibility guarantee.

---

## The Core Principle: Names Are Labels, UUIDs Are Authority

In most conventional database engines, an object's name is its identity.
If you rename a table, existing references break. If you move a schema,
paths change. If you want to present the same table under a different name
to a compatibility client, you typically have to create a view or synonym
that tracks the original.

ScratchBird separates name from identity at the structural level.

Every catalog object carries two distinct UUIDs:

- **`catalog_row_uuid`** — identifies this specific row in the catalog. It
  is the identity of the catalog entry itself.
- **`object_uuid`** — identifies the logical engine object (the table, view,
  schema, index, function, etc.) that this row describes. This is the durable
  anchor.

Names are stored as `LocalizedObjectName` entries associated with an `object_uuid`.
A single object can have multiple names, in multiple language tags, with
`LocalizedNameClass` values of `default_name`, `alias`, `compatibility_name`,
or `system_path`. When a compatibility client resolves a name, it resolves through
a localized name registry to the `object_uuid`. The engine then operates on
the object by its UUID, not by any name.

Verified in `src/catalog/bootstrap/catalog_identity.hpp`.

### What This Enables

- **Rename without reference breakage.** Renaming an object adds a new default
  name and retires the old one. Any reference that was resolved to an
  `object_uuid` before the rename still refers to the same object. Compiled
  stored procedures, policies, and security grants follow the object, not
  the name.

- **Compatibility projection.** A compatibility client that expects to find
  objects at a specific path (for example, the path layout of a different
  relational dialect) can be given a localized `compatibility_name` that maps
  to the same `object_uuid`. The engine serves the same data through both names.

- **Stable external references.** UUIDs are versioned as UUIDv7
  (`GenerateDurableEngineIdentityV7` in `src/core/uuid/uuid.hpp`), which
  embeds a timestamp prefix for natural time ordering. External tools that
  store a UUID reference remain valid across catalog evolution.

---

## The Recursive Schema Tree

The schema tree is the structural spine that organizes all objects in a database.
Its defining property is that it is recursive: every schema node can parent any
number of child schema nodes, forming a tree of arbitrary depth.

Each node in the tree is an `EngineSchemaTreeRecord`:

```
struct EngineSchemaTreeRecord {
  uint64_t creator_tx;
  uint64_t event_sequence;
  std::string schema_uuid;         // UUID of this schema node
  std::string parent_schema_uuid;  // UUID of the parent (empty at root)
  std::string default_name;        // migration/display cache only
  std::vector<EngineLocalizedName> localized_names;
  std::string state = "active";
};
```

The `default_name` is explicitly marked as a migration and display cache — it is
not the authority. Name authority is the SBNAME1 name registry. The schema tree
itself carries only UUID-to-UUID parent relationships.

Cycle detection is built in: `SchemaTreeWouldCreateCycle` is checked before
any schema re-parenting operation.

Verified in `src/engine/internal_api/catalog/schema_tree_api.hpp`.

### Bootstrap Schema Roots

When a database is created, the engine bootstraps a fixed set of well-known
schema paths under the root. These paths are defined in
`src/core/catalog/bootstrap_schema_roots.hpp`:

| Bootstrap path | Purpose |
|----------------|---------|
| `sys` | Engine-owned system namespace |
| `sys.catalog` | Catalog tables |
| `sys.metrics` | Metrics surfaces |
| `sys.agents` | Agent runtime catalog |
| `sys.security` | Security catalog |
| `sys.configuration` | Configuration namespace |
| `sys.management` | Management namespace |
| `sys.fn` | System functions |
| `sys.udr` | User-defined routines |
| `sys.parser` | Parser namespace |
| `sys.storage` | Storage catalog |
| `sys.mga` | MGA-specific catalog |
| `sys.audit` | Audit catalog |
| `sys.compatibility` | Compatibility projection namespace |
| `sys.information` | Information schema (authoritative) |
| `sys.information_schema` | Information schema (compatibility path) |
| `sys.diagnostics` | Diagnostic views |
| `users` | User data root |
| `users.public` | Public schema |
| `remote` | Remote/cluster namespace |
| `emulated` | Emulated compatibility namespace |

These are assigned fresh UUIDv7 identifiers on database creation. The paths
are the labels; the UUIDs are the identity. Note `sys.compatibility` and
`emulated` — these namespaces exist specifically to support compatibility
projection for client dialects that expect different name structures.

---

## Workareas

Workareas are a runtime concept that provides session-scoped or
connection-scoped name resolution context, allowing a session to work
within a specific subtree of the schema tree without fully qualifying every
name. The workarea is a name-resolution scope, not a security boundary; security
is always evaluated by the engine regardless of the active workarea.

For full detail on workareas and name resolution, see
[../Language_Reference/core_paradigms/uuid_catalog_identity.md](../Language_Reference/core_paradigms/uuid_catalog_identity.md).

---

## Why Durable Identity Matters For A CDE

The recursive schema tree and UUID-anchored identity are enabling conditions
for the convergent design:

- **Many dialects, one object.** A relational-dialect client and a
  document-dialect client can refer to the same underlying data through
  different name conventions, because both names resolve to the same
  `object_uuid`. The engine applies the same security policy, the same
  transaction visibility, and the same type system to both.

- **Rename-safe references.** Security grants and policies reference objects
  by UUID. When an object is renamed, its grants remain valid.

- **Catalog consistency across model families.** Whether an object stores
  relational rows, graph edges, or vector embeddings, it has a catalog entry
  with a UUID, a parent schema UUID, and a localized name set. The catalog
  is the same structure regardless of the model family.
