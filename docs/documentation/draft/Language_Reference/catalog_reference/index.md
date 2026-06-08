# Catalog Reference Index

This section documents public SBsql catalog surfaces. A catalog page explains
how to read an authorized metadata surface; it is not a direct mutation API.
Catalog rows are engine-owned, UUID-identified, transactionally visible, and
redacted by security policy.

## Catalog Authority

Catalog data records durable engine metadata: type descriptors, domain
descriptors, operation descriptors, protected-material metadata, policy
bindings, and protected-material audit evidence. User-facing names are resolver
input. Durable catalog identity is UUID based.

The public rules are:

- base catalog rows are mutated only by engine-managed DDL, security,
  protected-material, or catalog lifecycle operations;
- `SHOW`, `DESCRIBE`, information-style views, and support diagnostics are
  authorized projections over catalog state;
- projections can redact or hide rows according to the caller's security
  context;
- catalog rows become visible only through MGA transaction finality;
- cached parser, plan, driver, UDR, support-bundle, and metadata state must
  revalidate when catalog, security, resolver, or resource epochs change.

## Reading A Catalog Page

Each table page follows the same public interpretation model.

| Section | Meaning |
| --- | --- |
| Role | Why the surface exists and which engine behavior depends on it. |
| Keys and columns | Public metadata fields and their descriptor families. |
| Column semantics | How important fields affect binding, validation, policy, or execution. |
| Visibility and mutation | Who can read the projection and which engine operation can change it. |
| Dependencies and invalidation | Which cached or compiled state must rebind when rows change. |
| Failure modes | Required diagnostics for missing, stale, hidden, invalid, or policy-blocked state. |
| Verification checklist | Proof expectations for the surface. |

## Catalog Surfaces

| Catalog Surface | File |
| --- | --- |
| `sys.catalog.type_descriptor` | [sys_catalog_type_descriptor.md](sys_catalog_type_descriptor.md) |
| `sys.catalog.domain_descriptor` | [sys_catalog_domain_descriptor.md](sys_catalog_domain_descriptor.md) |
| `sys.catalog.domain_element` | [sys_catalog_domain_element.md](sys_catalog_domain_element.md) |
| `sys.catalog.type_alias_mapping` | [sys_catalog_type_alias_mapping.md](sys_catalog_type_alias_mapping.md) |
| `sys.catalog.type_capability` | [sys_catalog_type_capability.md](sys_catalog_type_capability.md) |
| `sys.catalog.operation_descriptor` | [sys_catalog_operation_descriptor.md](sys_catalog_operation_descriptor.md) |
| `sys.catalog.protected_material` | [sys_catalog_protected_material.md](sys_catalog_protected_material.md) |
| `sys.catalog.protected_material_version` | [sys_catalog_protected_material_version.md](sys_catalog_protected_material_version.md) |
| `sys.catalog.protected_material_policy_binding` | [sys_catalog_protected_material_policy_binding.md](sys_catalog_protected_material_policy_binding.md) |
| `sys.security.catalog.protected_material_audit_event` | [sys_security_catalog_protected_material_audit_event.md](sys_security_catalog_protected_material_audit_event.md) |

## Common Query Pattern

Catalog examples use explicit column lists so public scripts do not depend on
hidden, redacted, version-specific, or future columns.

```sql
select descriptor_uuid,
       canonical_type,
       type_family
from sys.catalog.type_descriptor
order by canonical_type;
```

## Related Reference Pages

- [UUID Catalog Identity](../core_paradigms/uuid_catalog_identity.md)
- [Security And Sandboxing](../core_paradigms/security_and_sandboxing.md)
- [Parser To SBLR Pipeline](../core_paradigms/parser_to_sblr_pipeline.md)
- [Type System Overview](../data_types/type_system_overview.md)
- [Domains, Casts, And Coercion](../data_types/domains_casts_and_coercion.md)
- [Refusal Vectors](../syntax_reference/refusal_vectors.md)
