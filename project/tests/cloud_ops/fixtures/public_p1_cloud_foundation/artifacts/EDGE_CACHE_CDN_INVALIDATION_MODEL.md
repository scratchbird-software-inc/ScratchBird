# Edge Cache and CDN Invalidation Model

Search key: `PUBLIC_P1_EDGE_CACHE_CDN_INVALIDATION_MODEL`

The implementation must add an edge cache/CDN invalidation surface that is
redaction-safe and transaction-authority preserving.

Required records:

- cache tag descriptor UUID
- tag class
- dependency scope UUIDs
- redaction policy UUID
- finality mode
- TTL
- purge flag
- provider profile UUID
- signed invalidation stream metadata

Required behavior:

- Cache tags are catalog/policy objects.
- Invalidation events are emitted only after MGA commit visibility.
- Rollback emits no invalidation.
- Payloads use redacted object identifiers and policy-approved labels.
- Missing provider, unsupported cache class, invalid signature configuration, or
  unsafe redaction policy is refused deterministically.
- CDN/edge event delivery is not durability or transaction finality.
