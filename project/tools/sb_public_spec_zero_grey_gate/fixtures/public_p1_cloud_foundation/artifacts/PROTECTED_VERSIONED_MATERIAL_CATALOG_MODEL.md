# Protected Versioned Material Catalog Model

Search key: `PUBLIC_P1_PROTECTED_VERSIONED_MATERIAL_CATALOG_MODEL`

The implementation must add a generic catalog family for sensitive,
policy-governed, versioned material without embedding credential or key
semantics into the catalog layer.

Required records:

- `protected_material_identity`: material UUID, object class, owner scope,
  storage classification, active version UUID, lifecycle state, created/updated
  transaction IDs, and audit lineage.
- `protected_material_version_identity`: version UUID, material UUID, version
  number, protected reference/envelope/hash metadata, validity interval,
  rotation state, retention policy UUID, release policy UUID, purge policy UUID,
  access policy UUID, and audit policy UUID.
- `protected_material_policy_binding`: links material/version UUIDs to security
  policy objects and diagnostic policy states.
- `protected_material_audit_event`: records create, rotate, resolve, release,
  deny, purge, and policy-change events.

Required behavior:

- Create, rotate, release, resolve, and purge are engine internal API operations.
- Raw secret values are never catalog values.
- Rollback removes uncommitted material/version state.
- Commit publishes new versions under MGA visibility.
- Purge must preserve audit evidence while removing protected reference
  reachability according to policy.
- `sys.information` projections may show redacted metadata; `sys.catalog`
  stores UUID-first low-level facts only.
