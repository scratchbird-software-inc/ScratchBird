# P4 Security Auth Audit Closure Evidence

Search key: `PUBLIC_SINGLE_NODE_P4_SECURITY_AUTH_AUDIT_CLOSURE_EVIDENCE`

## Scope Closed

P4 closes `SB-PUBLIC-GAP-0061` through `SB-PUBLIC-GAP-0068` for public
single-node security, authentication, authorization, audit, protected material,
security policy, auth plugin manifest, and TLS regression behavior.

Implemented evidence:

- Engine-owned authentication validates local password verifier hashes and
  rejects parser-side authority, stale policy epochs, wrong verifiers, and
  reusable plaintext credential persistence.
- Auth provider manifest and policy APIs validate provider family, capability,
  dependency, trust, rollout, policy epoch, and admin authority before admission.
- Authorization tests cover principal, role, group, GRANT, REVOKE, MGA
  visibility, default deny, stale policy cache refusal, row security, and
  definer-rights cache invalidation.
- Audit tests persist security and lifecycle audit evidence before visible
  success and validate redaction/cache-invalidation linkage.
- Protected-material and encryption tests cover key admission, cache redaction,
  encrypted filespace open, rotation, expiry, purge, shutdown purge, scope
  mismatch refusal, parser authority refusal, and plaintext key refusal.
- TLS regression labels remain attached to the listener and SBSQL full-route
  TLS/auth tests.

## Validation

CTest command:

```sh
ctest --test-dir build -L "security_auth_gate|authorization_privilege_gate|audit_event_gate|encryption_at_rest_gate|security_policy_gate|auth_plugin_manifest_gate|tls_regression_gate" --output-on-failure
```

Result: passed, `9/9` tests.

Covered tests:

- `database_lifecycle_security_auth_audit_p4_conformance`
- `database_lifecycle_security_principal_conformance`
- `database_lifecycle_encryption_key_conformance`
- `database_lifecycle_attach_auth_conformance`
- `database_lifecycle_config_policy_security_provider_conformance`
- `sb_listener_tls_required_fail_closed_smoke`
- `sb_listener_sbp_sbsql_sbwp_tls_engine_auth_route_smoke`
- `database_lifecycle_full_route_conformance`
- `sbsql_tls_transport_policy_conformance`

## Authority Notes

- The engine remains the only authentication and authorization authority.
- Listeners, parsers, drivers, and donor surfaces may transport credentials or
  proof, but do not make final acceptance decisions.
- Protected material is never returned or persisted as plaintext in diagnostics,
  result rows, audit evidence, or cache inspection surfaces.
- Security-sensitive paths preserve MGA visibility and fail closed on stale
  policy, stale cache, missing authority, or parser/donor bypass attempts.
