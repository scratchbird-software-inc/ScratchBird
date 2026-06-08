# Early Beta Known Gaps

Date: 2026-02-18  
Scope: Initial early beta release

## 1. Functional Gaps

- Parser/compiler integration path is bridge-backed and still relies on best-effort compile probing for some scenarios.
- Native metadata introspection behavior requires deeper runtime validation under live workloads.
- Native compile/execute contract behavior should be hardened for additional edge cases.

## 2. Governance and Security Gaps

- Mutation approval token validation is scaffold-level and not yet tied to durable approval evidence.
- Fine-grained authorization and tenant boundary policy controls are not finalized.
- Cost attribution, quotas, and rate-limiting controls are planned but not yet active.

## 3. Operational Gaps

- No production runbook/SLO dashboard package is published in this repository.
- Retry/circuit-breaker strategies for external dependencies are not standardized.
- Version compatibility checks with parser-layer release trains are not enforced fail-closed.

## 4. Documentation Gaps

- Public release contracts still need continuing promotion as implementation hardens.
- Additional operator troubleshooting playbooks are needed for live bridge failures.
- Native certification checklist and test evidence should be published.

## 5. Exit Criteria Toward Next Milestone

1. Finalize and enforce parser/compiler compatibility contract across repos.
2. Implement durable mutation-approval evidence and policy audit trail completeness checks.
3. Certify native workflow coverage with repeatable live integration tests.
4. Publish production-facing runbook and operational SLO/error budget guidance.
