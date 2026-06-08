# Full Route Acceptance Fixture

Search key: `PUBLIC_P1_CLOUD_FOUNDATION_FULL_ROUTE_ACCEPTANCE_FIXTURE`

The final validation route is:

```text
client/tool
<-> SBWP/TLS or admitted local IPC
<-> listener / parser pool where SQL is involved
<-> sb_server
<-> engine
<-> authentication-policy and security-policy
<-> protected material / cloud provider / KMS / operator / edge APIs
<-> MGA commit or rollback authority
<-> response and diagnostics
```

Required positive flow:

1. Create an example database with system catalog and security roots.
2. Create a protected material record and version under a transaction.
3. Commit and verify `sys.information` redacted projection visibility.
4. Register a local emulator cloud provider capability profile.
5. Bind a secretless identity/KMS profile to the provider profile.
6. Validate an operator dry-run reconcile plan.
7. Create a cache tag and emit a post-commit invalidation event.
8. Verify audit events and metric counters.

Required negative flow:

1. Roll back a protected material version and verify no committed visibility.
2. Request unsupported provider/KMS/operator/edge capabilities and verify exact
   diagnostics.
3. Attempt static-secret mode without explicit policy and verify denial.
4. Attempt edge invalidation before commit and verify no event is emitted.
5. Confirm no path uses WAL, donor storage, parser finality, cloud provider
   status, Kubernetes status, or CDN event delivery as engine finality.
