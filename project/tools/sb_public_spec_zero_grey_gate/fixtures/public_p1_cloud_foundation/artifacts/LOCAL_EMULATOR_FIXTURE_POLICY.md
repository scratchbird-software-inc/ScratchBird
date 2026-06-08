# Local Emulator Fixture Policy

Search key: `PUBLIC_P1_CLOUD_FOUNDATION_LOCAL_EMULATOR_FIXTURE_POLICY`

CTest must not require live AWS, GCP, Azure, Kubernetes, CDN, or external KMS
accounts to prove these public targets.

Required emulator surfaces:

- local cloud provider capability profile
- local KMS/HSM reference emulator that returns deterministic protected
  reference metadata, never raw keys
- local workload identity/OIDC/managed identity verifier fixtures
- Kubernetes CRD schema validation and dry-run reconciler fixture
- edge/CDN invalidation sink that records signed/redacted event metadata
- negative fixtures for missing provider, unsupported mode, invalid CRD,
  unsafe redaction policy, and unavailable edge provider

The emulator is a test fixture. It must not become production authority and must
be clearly separated from live provider adapters.
