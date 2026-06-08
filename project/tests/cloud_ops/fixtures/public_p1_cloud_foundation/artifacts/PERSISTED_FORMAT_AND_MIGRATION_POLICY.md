# Persisted Format and Migration Policy

Search key: `PUBLIC_P1_CLOUD_FOUNDATION_PERSISTED_FORMAT_AND_MIGRATION_POLICY`

New catalog families and cloud/operator/edge policy records must have explicit
format behavior.

Required behavior:

- Catalog/table record formats have version identifiers.
- Database create seeds new structures in transaction 1.
- Opening an older database runs deterministic bootstrap migration or enters
  restricted-open with diagnostics.
- Downgrade that would lose protected material, provider, KMS, operator, edge,
  audit, or outbox state is refused.
- Repair/diagnostic mode can inspect records without releasing protected
  material.
- Migration is idempotent and auditable.
- No migration relies on WAL, external provider logs, Kubernetes status, or CDN
  delivery records as recovery truth.

Required tests:

- create fresh database
- open database lacking the new structures and migrate
- fail downgrade with exact diagnostic
- restricted-open when protected provider/KMS dependencies are unavailable
- repair/diagnostic read without secret exposure
