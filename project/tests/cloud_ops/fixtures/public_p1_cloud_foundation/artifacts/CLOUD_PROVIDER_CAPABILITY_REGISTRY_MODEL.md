# Cloud Provider Capability Registry Model

Search key: `PUBLIC_P1_CLOUD_PROVIDER_CAPABILITY_REGISTRY_MODEL`

The implementation must add provider capability profiles as first-class catalog
and policy objects used by server/database configuration.

Required profile fields:

- `provider_profile_uuid`
- provider name and profile class
- supported identity modes
- supported KMS/HSM/envelope modes
- supported storage classes and snapshot semantics
- supported route/network/load-balancing capabilities
- supported operator lifecycle capabilities
- supported edge-cache/CDN invalidation capabilities
- observability bridge capabilities
- public single-node support state
- private cluster-only capability flags with fail-closed diagnostics

Required behavior:

- Deployment profiles select a provider profile by UUID.
- Requests for unsupported capabilities are refused before side effects.
- Profiles are versioned and auditable.
- Provider behavior is configuration/policy; it is not storage, transaction, or
  security authority.
