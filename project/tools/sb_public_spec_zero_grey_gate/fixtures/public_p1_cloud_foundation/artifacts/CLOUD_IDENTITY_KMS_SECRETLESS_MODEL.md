# Cloud Identity, KMS, and Secretless Authentication Model

Search key: `PUBLIC_P1_CLOUD_IDENTITY_KMS_SECRETLESS_MODEL`

The implementation must connect cloud identity and KMS/HSM references to engine
security policy without storing plaintext secrets.

Required identity modes:

- workload identity
- OIDC federation
- managed identity
- IAM role
- service account token
- static-secret forbidden by default

Required KMS/HSM behavior:

- KMS and HSM references are protected material references, not raw keys.
- Envelope encryption records bind to protected material version UUIDs.
- Rotation creates new protected material versions under MGA visibility.
- Missing provider/KMS policy returns deterministic denial.
- External identity verification does not bypass engine authentication,
  authorization, session, or transaction creation.

Required negative tests:

- static secret without explicit policy is refused
- missing provider profile is refused
- unsupported identity mode is refused
- stale KMS version cannot authorize new release
- denied release emits audit event before failure return
