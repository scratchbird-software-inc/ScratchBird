# Schema Tree And Name Resolution

This page documents the SBsql schema tree, schema session variables, name-resolution order, recursive schema behavior, base schema branches, and SBsql-parser sandbox rules.

Related pages: [schema.md](schema.md), [script_tokens_and_identifiers.md](script_tokens_and_identifiers.md), [database.md](database.md), [../core_paradigms/uuid_catalog_identity.md](../core_paradigms/uuid_catalog_identity.md), [../core_paradigms/security_and_sandboxing.md](../core_paradigms/security_and_sandboxing.md), [../functional_reference/sb_core.md](../functional_reference/sb_core.md), and [../catalog_reference/index.md](../catalog_reference/index.md).

## Purpose

A schema is both a catalog object and a resolver scope. A schema name such as `app` is not durable authority. The durable object is the schema UUID, and every object created inside that schema is bound to a parent schema UUID.

The schema tree is recursive. A schema may have child schemas, and each child has its own UUID, names, grants, policy, comments, and child-object namespace. This lets ScratchBird model native SBsql workspaces, parser-attached database roots, remote bridge namespaces, system catalog branches, user home branches, and compatibility catalog projections without flattening every object into a single global namespace.

## Schema Session Variables

| Variable Or Surface | Meaning | Authority |
| --- | --- | --- |
| Current schema | Default schema UUID used for unqualified object creation and ordinary unqualified lookup. Exposed through `current_schema()` and the `CURRENT_SCHEMA` context variable. | Session execution context. |
| Default schema | Database or session default used when a new session has no more specific current-schema setting. `ALTER DATABASE ... SET DEFAULT SCHEMA ...` changes database-level default metadata. | Database catalog metadata plus session attach policy. |
| Home schema | Default user-owned schema created or selected by identity policy, normally under the user-home root. It is a convenience root for the user's own objects, not a privilege bypass. | Identity policy and schema catalog row. |
| Search path | Ordered list of schema UUIDs considered after the current schema for unqualified lookup. Exposed through `search_path()` and `SHOW SEARCH PATH` where admitted. | Session descriptor and SBsql session policy. |
| Default root | The root UUID used as a final resolver scope for the session, SBsql sandbox, or system/admin context. | Attach policy, security context, and SBsql session policy. |
| Sandbox root | The highest schema root visible to a SBsql-parser session or restricted session. Names outside this root are invisible unless a granted catalog projection renders them. | Materialized authorization and SBsql-session sandbox policy. |
| Temporary schema | Session-private schema for temporary objects where the SBsql supports it. | Session descriptor and SBsql-session temporary-object policy. |

These variables are UUID-bearing session descriptors. Display names are rendered through the name registry after authorization and language/profile selection.

## Context Functions

```sql
select current_schema();
show search path;
```

`current_schema()` returns the current schema projection for the session. The generated functional reference records it as `sb.session.current_schema` with SBLR binding `sblr.expr.session_current_schema.v3`.

`search_path()` and `SHOW SEARCH PATH` expose the active search-path descriptor where the session and SBsql session policy admit that surface. SBsql policies may render equivalent behavior as `SET search_path`, `SHOW search_path`, `USE database`, or no search path at all.

## Base Schema Tree

Database creation assigns UUIDv7 schema object IDs for bootstrap paths. The names below are labels over those UUIDs; implementations must not treat the text path as object identity.

```text
<database uuid>
|
+-- sys
|   |
|   +-- catalog              engine-owned catalog authority tables
|   +-- catalog_readable     authorized catalog projections for users and SBsql views
|   +-- metrics              local metrics and retention metadata
|   +-- agents               agent registration, leases, and local agent metadata
|   +-- security             security providers, users, roles, groups, policy metadata
|   +-- configuration        configuration descriptors and validation surfaces
|   +-- management           administrative command and management-state surfaces
|   +-- fn                   built-in function and function-package metadata
|   +-- udr                  UDR package, bridge, parser-support, and trusted binding metadata
|   +-- parser               SBsql session policy, dialect, route, and metadata
|   +-- storage              filespace, page, TOAST, index, and storage diagnostics
|   +-- mga                  transaction inventory, horizons, cleanup, and recovery metadata
|   +-- audit                audit and event records visible through policy
|   +-- compatibility        SBsql and driver compatibility projections
|   +-- information          ScratchBird-native information schema
|   +-- information_schema   standards-compatible information-schema projection
|   +-- diagnostics          canonical diagnostics and message-vector metadata
|
+-- users
|   |
|   +-- public               default shared user schema
|   +-- <home schema>        user-owned home branch created or selected by identity policy
|
+-- remote                  bridge/federation namespace roots
|
+-- workarea                parser-attached database/workarea roots
|
+-- local.user              bootstrap local-user root used by the initial catalog manifest
```

The minimum bootstrap manifest requires `sys.catalog`, `sys.metrics`, and `local.user`. The broader logical tree gives stable placement for public system branches, user home schemas, remote bridge roots, and parser-attached workarea roots.

## Branch Purpose

| Branch | Purpose | Ordinary User Mutability |
| --- | --- | --- |
| `sys.catalog` | Durable catalog authority records. | No. Engine-owned. |
| `sys.catalog_readable` | Authorized projections over catalog authority tables. | No direct mutation. Query visibility is policy controlled. |
| `sys.metrics` | Local metrics, retention policy, and observability state. | No direct mutation except admitted administrative policy. |
| `sys.security` | Security provider, identity, role, group, and policy metadata. | Only through security DCL/admin surfaces. |
| `sys.fn` | Built-in function/package registry. | No ordinary user mutation. |
| `sys.udr` | UDR and parser-support package metadata. | Only through trusted UDR registration/admin surfaces. |
| `sys.parser` | Parser profiles, metadata rendering descriptors, and parser route metadata. | Only through parser administration policy. |
| `sys.storage` | Storage diagnostics and storage object metadata. | Engine/admin controlled. |
| `sys.mga` | Transaction, horizon, cleanup, and recovery metadata. | Engine controlled. |
| `sys.information` | ScratchBird-native information views. | Read through grants and policy. |
| `sys.information_schema` | Standards-style information schema projection. | Read through grants and policy. |
| `users.public` | Shared ordinary user namespace. | Yes, subject to grants and policy. |
| `users.<home schema>` | Per-user home namespace. | Yes for the owning identity, subject to grants and policy. |
| `remote` | Namespaces for remote bridge connections and external relation descriptors. | Controlled by bridge and external-access policy. |
| `workarea` | Parser-attached workarea roots. | Mutated through the connected parser route and granted SBsql administration. |
| `local.user` | Initial local-user bootstrap root. | User mutable in the bootstrap manifest. |

Cluster-specific roots are not public standalone authority. Cluster surfaces must pass the compile-time cluster gate and, in public builds, route to the cluster stub or return a canonical unsupported/unlicensed message vector.

## Recursive Schema Rules

| Rule | Contract |
| --- | --- |
| Parent identity | Every non-root schema stores its parent schema UUID. |
| Path labels | A path such as `users.alice.app` is a sequence of resolver labels over UUIDs. |
| Child namespace | Child schemas and objects are scoped under the parent schema UUID. |
| No text authority | Changing a schema name updates resolver metadata; it does not change object UUIDs or child object identity. |
| No ambiguous visible path | If the same localized path resolves to more than one visible object UUID, binding fails with an ambiguity diagnostic. |
| No sandbox escape | A restricted or SBsql session cannot resolve outside its sandbox root by spelling a parent path. |
| Transaction visibility | Schema creation, rename, and drop become visible only through MGA transaction finality. |
| Cache invalidation | Resolver caches, prepared statements, parser metadata, and support-bundle projections are invalidated when schema resolver state changes. |

## Name Resolution Order

The resolver works on UUIDs and descriptors. A parser-recognized name must still bind through the engine resolver before execution.

1. If the reference is a UUID reference, validate the UUID, expected object class, transaction visibility, and authorization. UUID references do not use the search path.
2. Split a qualified name into identifier atoms. Preserve quoted/exact-match flags and apply the active identifier profile to unquoted atoms.
3. For a qualified name, resolve the parent path directly and do not use the search path.
4. For an unqualified name, search candidate scopes in order:
   - explicit target schema supplied by the surrounding command;
   - current schema UUID;
   - search-path schema UUIDs;
   - default root UUID;
   - final global fallback where the engine admits it.
5. Match only entries with the requested object class or an admitted compatible class. For example, a relation request may match a table, view, materialized view, external table, or foreign table.
6. Apply language, localized-name, primary-name, alias, SBsql, and identifier-profile rules.
7. Apply materialized authorization and sandbox visibility.
8. Return a bound object UUID, resolved object type, schema UUID, catalog generation, security epoch, resource epoch, and name-resolution epoch.

If no authorized match exists, the resolver returns the same user-facing result for "not found" and "not visible" where policy requires metadata hiding.

## Identifier Profile Rules

Identifier folding is profile-aware:

| Profile Family | Unquoted Identifier Behavior |
| --- | --- |
| SBsql default profile | Fold toward upper-case lookup unless exact matching is required. |
| SBsql lower-case profile | Fold toward lower-case lookup. |
| SBsql case-insensitive profile | Use case-insensitive lookup behavior for unquoted identifiers. |
| Quoted identifiers | Require exact-match lookup under the active quoted-identifier profile. |

Names can have localized labels and comments. The default language is `en`, the session language is preferred when present, and `und` can be used for language-independent names.

## SBsql Examples

```sql
create schema app;
create table app.orders (
  order_id uuid primary key,
  order_total decimal(18,2)
);

comment on schema app is 'Application schema';
show schema app;
describe schema app;
```

Qualified references do not depend on the search path:

```sql
select order_id, order_total
from app.orders
where order_total > 100;
```

Unqualified references bind through the current schema and search path:

```sql
select current_schema();
show search path;

select order_id
from orders;
```

Administrative code that needs exact identity should use UUID references or bind names and then record the resolved UUID:

```sql
describe schema uuid '019d0000-0000-7000-8000-000000000001';
```

The UUID literal above is illustrative. Real database and schema UUIDs are assigned by the engine.

## SBsql Parser Sandboxes

SBsql parser sessions see their connected SBsql database or workarea as their schema root.

| SBsql Family | Schema/Database Behavior |
| --- | --- |
| SBsql | Single attachment schema context. There is no SBsql-style search path. Resolution is relative to the connected SBsql schema root. |
| SBsql/SBsql/SBsql/SBsql/Dolt-style profiles | `USE database` changes the current database/schema descriptor. There is no separate multi-schema search path. |
| SBsql-family profiles | `search_path` is represented as a UUID-resolved descriptor list. Temporary schema behavior is session-private and may shadow permanent objects where the SBsql policy requires it. |

A parser route cannot escape to SBsql global namespaces by spelling paths outside its root. Metadata rendering catalog views can expose projected metadata from outside the root only when those views have their own grants and policy. The user's select privilege on a projection is not the same thing as parser authority to traverse the underlying schema tree.

## Catalog And Resolver Evidence

Resolver output must carry enough evidence for safe execution and cache invalidation:

| Evidence | Use |
| --- | --- |
| Object UUID | Durable object identity. |
| Schema UUID | Parent schema authority and dependency scope. |
| Resolved object type | Confirms that a relation/table/view/domain/function/etc. match is class-correct. |
| Catalog generation | Detects stale catalog bindings. |
| Security epoch | Detects stale authorization bindings. |
| Resource epoch | Detects stale resource/dependency bindings. |
| Name-resolution epoch | Detects stale name-registry or search-path bindings. |
| Search-path hash | Separates prepared statements whose text is identical but whose resolver context differs. |

## Failure Modes

| Condition | Required Behavior |
| --- | --- |
| Missing schema variable | Fail closed with a context diagnostic. |
| Duplicate visible path | Ambiguity diagnostic; no arbitrary winner. |
| Hidden object | Return not-found/not-visible according to metadata-hiding policy. |
| Quoted identifier mismatch | Exact-match failure. |
| Search-path stale after DDL | Invalidate cached plan or refuse stale execution. |
| SBsql root escape | Sandbox-denied diagnostic. |
| System branch mutation by ordinary user | Denied diagnostic. |
| Cluster branch without cluster authority | Unsupported or unlicensed cluster message vector. |

## Verification Checklist

| Check | Required Outcome |
| --- | --- |
| Bootstrap | Required schema roots exist and have UUIDv7 schema identities. |
| Recursive tree | Child schema creation stores parent schema UUID and cannot create ambiguous visible paths. |
| Session variables | Current schema, default root, and search path are UUID descriptors. |
| Resolution | Qualified names bypass search path; unqualified names use current schema and search-path order. |
| Identifier profiles | SBsql, SBsql, SBsql, and SBsql folding rules produce expected bindings. |
| Sandboxing | SBsql parser sessions cannot resolve outside their connected root. |
| Catalog projections | Authorized catalog views can render projected metadata without granting resolver traversal. |
| Invalidation | Schema DDL changes invalidate resolver, plan, parser, and support-bundle projections. |
