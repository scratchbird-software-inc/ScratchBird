# sys.catalog.type_alias_mapping Catalog Reference

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `catalog_sys_catalog_type_alias_mapping`


## Role

`sys.catalog.type_alias_mapping` is a system catalog surface. It records durable metadata used by the binder, engine verifier, optimizer, security layer, support diagnostics, bridge rendering, or transaction model.

Catalog rows are not parser authority. They are visible through authorized catalog projections, SHOW/DESCRIBE surfaces, information-style views, or support tooling. Base catalog mutation must go through engine-managed catalog operations.

## Keys And Columns

| Column | Type Family | Requirement |
| --- | --- | --- |
| mapping_uuid | UUID | Mapping identity. |
| source_family | enum domain | SBsql engine family. |
| source_version_profile | text/domain | Version/profile. |
| source_type_name | text domain | SBsql spelling. |
| source_type_code | nullable text/domain | SBsql OID/code/protocol identifier. |
| representation_class | enum domain | native, domain, compound_domain, opaque_domain, udr_bridge, render_only, deferred, unsupported_by_policy. |
| descriptor_uuid | nullable UUID | Canonical type descriptor. |
| domain_uuid | nullable UUID | Domain descriptor. |
| udr_package_uuid | nullable UUID | Responsible C++ UDR package. |
| literal_policy_uuid | nullable UUID | Literal typing policy. |
| bind_policy_uuid | nullable UUID | Driver bind policy. |
| metadata_policy_uuid | nullable UUID | Metadata rendering policy. |
| compatibility_mode | enum domain | strict_scratchbird, alias_profile, bridge_only, degraded, render_only, unsupported_by_policy. |

## Full Definition Extract

### Catalog Table `sys.catalog.type_alias_mapping`

Primary key: `mapping_uuid`

Required columns:

| Column | Type family | Requirement |
| --- | --- | --- |
| `mapping_uuid` | UUID | Mapping identity. |
| `source_family` | enum domain | SBsql engine family. |
| `source_version_profile` | text/domain | Version/profile. |
| `source_type_name` | text domain | SBsql spelling. |
| `source_type_code` | nullable text/domain | SBsql OID/code/protocol identifier. |
| `representation_class` | enum domain | native, domain, compound_domain, opaque_domain, udr_bridge, render_only, deferred, unsupported_by_policy. |
| `descriptor_uuid` | nullable UUID | Canonical type descriptor. |
| `domain_uuid` | nullable UUID | Domain descriptor. |
| `udr_package_uuid` | nullable UUID | Responsible C++ UDR package. |
| `literal_policy_uuid` | nullable UUID | Literal typing policy. |
| `bind_policy_uuid` | nullable UUID | Driver bind policy. |
| `metadata_policy_uuid` | nullable UUID | Metadata rendering policy. |
| `compatibility_mode` | enum domain | strict_scratchbird, alias_profile, bridge_only, degraded, render_only, unsupported_by_policy. |

## Operational Boundaries

- Base rows require UUID identity and lifecycle metadata.
- Visibility is policy controlled and may use redaction.
- Derived views must preserve base-row authority and must not become engine identity.
- catalog projections are rendering surfaces only.

## Example Inspection

```sql
select *
from sys.catalog.type_alias_mapping
limit 20;
```
