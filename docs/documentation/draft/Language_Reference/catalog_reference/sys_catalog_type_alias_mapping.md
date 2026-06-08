# sys.catalog.type_alias_mapping Catalog Reference

This page documents the authorized catalog surface that maps SBsql-visible type
spellings, aliases, profiles, and rendering names to canonical descriptors or
domains.

Generation task: `catalog_sys_catalog_type_alias_mapping`

Related pages: [Type System Overview](../data_types/type_system_overview.md),
[sys.catalog.type_descriptor](sys_catalog_type_descriptor.md),
[sys.catalog.domain_descriptor](sys_catalog_domain_descriptor.md), and
[Script Tokens And Identifiers](../syntax_reference/script_tokens_and_identifiers.md).

## Role

`sys.catalog.type_alias_mapping` lets SBsql remain context sensitive while the
engine remains descriptor-driven. It records which public spelling is accepted
in which profile, what descriptor or domain it resolves to, and how metadata
should render that type back to an authorized user.

Aliases are not execution authority. After binding, the SBLR envelope carries
descriptor and domain UUIDs.

## Keys And Columns

Primary key: `mapping_uuid`

| Column | Type Family | Requirement |
| --- | --- | --- |
| `mapping_uuid` | UUID | Durable mapping identity. |
| `profile_family` | enum domain | SBsql profile or compatibility profile that owns the spelling. |
| `profile_version` | text/domain | Version or policy profile for the spelling. |
| `visible_type_name` | text domain | Public type spelling accepted or rendered by the profile. |
| `visible_type_code` | nullable text/domain | Optional profile-owned symbolic code for metadata rendering. |
| `representation_class` | enum domain | `native`, `domain`, `compound_domain`, `opaque_domain`, `udr_bridge`, `render_only`, or `unsupported_by_policy`. |
| `descriptor_uuid` | nullable UUID | Canonical type descriptor used when the alias maps to a carrier. |
| `domain_uuid` | nullable UUID | Domain descriptor used when the alias maps to a domain. |
| `udr_package_uuid` | nullable UUID | Trusted package responsible for an opaque or bridge-backed representation. |
| `literal_policy_uuid` | nullable UUID | Literal typing policy for this spelling. |
| `bind_policy_uuid` | nullable UUID | Parameter and assignment binding policy. |
| `metadata_policy_uuid` | nullable UUID | Rules for rendering the alias in metadata output. |
| `compatibility_mode` | enum domain | `strict_scratchbird`, `alias_profile`, `bridge_only`, `degraded`, `render_only`, or `unsupported_by_policy`. |

## Alias Classes

| Representation Class | Meaning |
| --- | --- |
| `native` | The visible name maps directly to a canonical ScratchBird descriptor. |
| `domain` | The visible name maps to a domain UUID. |
| `compound_domain` | The visible name maps to a structured domain with elements. |
| `opaque_domain` | The visible name maps to a descriptor whose behavior is exposed only through admitted operations. |
| `udr_bridge` | The visible name requires a trusted package boundary for rendering or binding. |
| `render_only` | The name can be shown in metadata but cannot be used as an executable type declaration. |
| `unsupported_by_policy` | The spelling is recognized only to return a stable unsupported diagnostic. |

## Binding Rules

When the parser sees a type spelling:

1. resolve the spelling under the active SBsql profile;
2. check `compatibility_mode`;
3. bind either `descriptor_uuid` or `domain_uuid`;
4. apply literal and bind policy;
5. attach descriptor/domain identity to the SBLR envelope;
6. render diagnostics using metadata policy.

If both descriptor and domain are null for an executable mapping, binding must
fail. If both are present, the mapping must define which identity is carrier
authority and which is domain policy.

## Metadata Rendering

Metadata rendering can choose a visible type name that differs from the
canonical descriptor name. Rendering is presentation only. It must not make the
visible name into engine authority.

Example:

```sql
select visible_type_name,
       representation_class,
       compatibility_mode
from sys.catalog.type_alias_mapping
where profile_family = 'sbsql'
order by visible_type_name;
```

## Visibility And Mutation

Base rows are engine-owned and are created or updated through type, domain,
profile, package, or catalog lifecycle operations. Users inspect mappings
through authorized catalog projections, type inspection, metadata views, or
support diagnostics.

Hidden or unsupported aliases should produce the same public result as an
unknown type when metadata-hiding policy requires it.

## Dependencies And Invalidation

Alias mapping changes can invalidate:

- prepared statements that used a type spelling;
- routine and trigger compilation;
- metadata caches;
- driver and parser metadata;
- support-bundle projections;
- UDR package bindings;
- cast and conversion decisions.

## Failure Modes

| Condition | Required Behavior |
| --- | --- |
| Visible name maps to no descriptor/domain | Bind diagnostic. |
| Mapping is `render_only` but used in DDL | Unsupported diagnostic. |
| Mapping is `unsupported_by_policy` | Stable unsupported message vector. |
| Trusted package missing for `udr_bridge` | Unavailable capability diagnostic. |
| Ambiguous visible names in one profile | Ambiguity diagnostic; no arbitrary winner. |
| Metadata policy hides mapping | Redacted or not-visible result. |
| Mapping epoch stale | Rebind or refuse cached statement. |

## Verification Checklist

Proof should demonstrate:

- accepted type spellings map to descriptor or domain UUIDs;
- unsupported spellings return explicit diagnostics;
- render-only aliases cannot create executable descriptors;
- profile-specific mappings do not leak outside their profile;
- metadata rendering does not become type authority;
- alias changes invalidate cached parser, plan, routine, and metadata state;
- hidden mappings do not leak through unauthorized projections.
