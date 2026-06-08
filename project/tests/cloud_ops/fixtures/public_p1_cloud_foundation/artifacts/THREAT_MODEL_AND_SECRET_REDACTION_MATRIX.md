# Threat Model and Secret Redaction Matrix

Search key: `PUBLIC_P1_CLOUD_FOUNDATION_THREAT_MODEL_AND_SECRET_REDACTION_MATRIX`

## Threat Surfaces

| Surface | Required control |
| --- | --- |
| Catalog records | Store UUIDs, protected references, envelopes, hashes, policies, and redacted metadata only. |
| Diagnostic vectors | Never include plaintext secret, token, key, KMS material, credential, signed payload body, or provider response body. |
| Audit events | Include actor/session/transaction/policy/result metadata and redacted protected-material UUIDs only. |
| Metrics | Use bounded labels and redacted provider/profile identifiers; no user secret values or bearer material. |
| Logs/traces | Redact connect strings, provider credentials, KMS references beyond UUID/class, and invalidation payload bodies. |
| Operator status | Status records are evidence only and must not contain secrets, provider tokens, or key references beyond redacted UUIDs. |
| Edge/CDN invalidation | Payloads carry cache tag IDs, class, dependency UUIDs where policy permits, finality label, and signature metadata only. |

## Required Tests

- Secret-like strings injected into every target route must be absent from logs,
  diagnostics, audit payloads, metric labels, operator status, and edge events.
- Denied release/purge/KMS/auth/invalidation paths must emit a redacted
  diagnostic and audit event.
- Successful paths must not return raw protected material.
