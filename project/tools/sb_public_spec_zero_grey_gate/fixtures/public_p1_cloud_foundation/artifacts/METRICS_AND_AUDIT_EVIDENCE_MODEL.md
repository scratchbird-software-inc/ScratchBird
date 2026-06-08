# Metrics and Audit Evidence Model

Search key: `PUBLIC_P1_CLOUD_FOUNDATION_METRICS_AND_AUDIT_EVIDENCE_MODEL`

The five target rows require observable, durable evidence.

Required audit events:

- protected material create, rotate, resolve, release, deny, purge, policy change
- provider profile create/update/deny
- KMS release, deny, rotate, stale-version refusal
- secretless identity accepted/denied by engine policy
- operator dry-run, reconcile, conflict, invalid CRD, cluster-field refusal
- edge cache tag create/update, invalidation emitted, invalidation refused

Required metric families:

- `sys.metrics.security.protected_material.*`
- `sys.metrics.cloud.provider.*`
- `sys.metrics.cloud.kms.*`
- `sys.metrics.cloud.operator.*`
- `sys.metrics.cloud.edge_cache.*`

Metrics must use bounded labels and redacted identifiers. Audit event success
must be recorded before success is returned for protected material release,
KMS release, operator lifecycle action, or edge invalidation emission.
