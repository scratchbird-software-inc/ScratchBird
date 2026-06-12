# Security Auth Audit Closure Model

Search key: `PUBLIC_SINGLE_NODE_SECURITY_AUTH_AUDIT_CLOSURE_MODEL`

The engine is the only authentication and authorization authority. Listeners,
parsers, drivers, and reference packages may transport credentials or proof but may
not decide final acceptance.

Required behavior:

- Local authentication providers compare protected hash/encryption results, not
  plaintext passwords.
- Authorization covers privileges, roles, groups, schema/object scope, and
  deny-message vectors.
- Audit events are persisted before externally visible success when the spec
  requires evidence-before-success.
- Encryption-at-rest and protected material surfaces fail closed when keys or
  policy are unavailable.
- Auth plugin manifests are policy-controlled and versioned.
- TLS row `0066` remains closed through regression gates.
