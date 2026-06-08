# Early Beta Known Gaps

Date: 2026-04-20
Scope: ScratchBird AI early beta (`0.1.0`)

## 1. Functional Gaps

- Explain/trace data exists at the helper and service level, but broader live
  bridge-backed explain validation is still limited.
- Native live-workload coverage is narrower than the in-process and
  fake-backend contract coverage beyond the refreshed direct-listener path.
- Engine-managed retrieval depth remains incomplete beyond the baseline
  live-certified lifecycle/search path.

## 2. Governance and Security Gaps

- Fine-grained authorization and tenant boundary policy are stronger than the February baseline, but still not production-complete.
- Third-party signing and externally hosted approval products are not yet finished; the shipped surface currently stops at durable local approval evidence plus HMAC or external-reference attestation issue/verify flows.

## 3. Operational Gaps

- Operator bundle generation, runtime diagnostics, and SLO summary generation are implemented, but the repository does not ship a pre-generated target-specific production dashboard/runbook package for every environment.
- Environment-specific live evidence for `manager_proxy`, `local_ipc`, and
  `embedded_local_only` still depends on the active test harness exposing those
  runtime modes.

## 4. Documentation and Release Gaps

- Release readiness now depends on [EARLY_BETA_CONFORMANCE_GATES.md](../releases/EARLY_BETA_CONFORMANCE_GATES.md); that contract should remain aligned with the actual code surface.
- Live bridge troubleshooting, governed-operation guidance, and operator bundle generation are documented; the remaining documentation work is now current completion scope rather than later cleanup.
- The repo-local implementation closeout is closed.

## 5. Exit Criteria Toward The Next Milestone

1. Certify additional runtime modes on environments that actually expose `manager_proxy`, `local_ipc`, or `embedded_local_only`.
2. Expand live workload, explain/trace, and deeper retrieval-scale certification beyond the bounded current claim surface.
3. Finish third-party signing and externally hosted approval productization beyond the shipped local attestation path.
4. Keep release/status materials synchronized with generated evidence.
5. Publish target-specific operator dashboard/runbook packages for supported environments.
