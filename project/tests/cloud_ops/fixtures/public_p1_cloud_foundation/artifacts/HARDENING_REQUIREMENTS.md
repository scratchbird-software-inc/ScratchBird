# Hardening Requirements

Search key: `PUBLIC_P1_CLOUD_FOUNDATION_HARDENING_REQUIREMENTS`

## Required Controls

- No placeholder or stub behavior may close a target row.
- No plaintext secret material may be persisted or returned by the engine.
- Protected material must be UUID-identified, versioned, policy-linked, audited,
  and visible through MGA transaction rules only after commit.
- Purge/release/access decisions must be policy checked and audited before
  success is reported.
- Cloud provider capability records must fail closed when a requested provider,
  identity mode, KMS mode, storage class, operator feature, or edge-cache feature
  is unavailable or unsupported.
- Secretless authentication must refuse static secrets by default and must never
  treat external provider acceptance as engine authorization authority.
- Kubernetes operator reconciliation must be idempotent and must not activate
  cluster-only behavior in public single-node builds.
- Edge/CDN invalidation must be emitted only after engine commit visibility and
  cannot be a transaction finality source.
- All negative tests must assert exact diagnostic vectors, retryability, and
  transaction finality.
- Registry closure must preserve prior closed execution_plan rows and reduce
  `public_open_entries` from `29` to `24`.
