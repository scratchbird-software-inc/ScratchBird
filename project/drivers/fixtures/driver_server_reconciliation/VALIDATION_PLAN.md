# Validation Plan

Search key: `DRIVER-SERVER-RECONCILIATION-VALIDATION-PLAN`.

## Required Gates

1. Parse `public_contract_snapshot`.
2. Verify every row in `artifacts/TARGET_CHECKLIST_ROWS.csv` has one lane or server status record.
3. Reject any required row with `not_started`, `implemented_without_evidence`, `server_unspecified`, or `undocumented_implementation`.
4. Reject conditional rows unless they are `implemented_and_proven` or `not_applicable_with_citation`.
5. Verify every implementation-ahead item is classified and either specified, guarded, or refused.
6. Run security regression proving PEER/ident fail closed without verified OS peer-credential evidence.
7. Run CTest labels for driver/adaptor/tool gates and the full SBSQL route.
8. Run server-verification packets for every claimed driver lane.
9. Run benchmark only through the declared production route, not direct engine shortcuts.
10. Synchronize `public_audit_summary` and public zero-grey registries.
11. Validate protocol version skew and feature negotiation against old/new client/server compatibility cases.
12. Validate deterministic refusal behavior for every guarded, unsupported, conditional, or N/A row.
13. Run fuzz and fault-injection cases for wire, auth, driver, stream, and lifecycle boundaries.
14. Validate package metadata, install smoke tests, signing/SBOM status, and build-artifact isolation for each claimed lane.
15. Enforce performance budgets for the real client-to-engine route.
16. Execute documentation sample applications for every claimed driver, adapter, and tool path.
17. Run donor compatibility tests through the admitted ScratchBird route.
18. Generate the machine-readable release declaration and compare it to tracker, inventory, and zero-grey registry state.

## Execution_Plan Creation Gate

Before implementation agents are launched, run:

```bash
python3 project/tools/sb_public_spec_zero_grey_gate/public_spec_zero_grey_gate.py hardening \
  --execution_plan-root project/drivers/fixtures/driver_server_reconciliation/public_proof
```

The CTest equivalent is:

```bash
ctest --test-dir build --output-on-failure -R driver_server_reconciliation_hardening_gate
```

## Initial Local Test

The first fast regression after P0 edits is:

```bash
cmake --build build --target security_auth_audit_p4_conformance
ctest --test-dir build --output-on-failure -R security_auth_audit_p4_conformance
```

If the target name differs in the local build, use:

```bash
ctest --test-dir build -N | grep -i security
```

## Full Closure Test Shape

The final closure run must include:

- Driver/adaptor/tool common conformance gate.
- Per-lane status YAML gate.
- Server-verification packet gate.
- Native SBWP/TLS listener/parser/server/engine route tests.
- MGA transaction stress tests for autocommit, explicit transactions, savepoints,
  cancel, reconnect, reset, pool return, prepared transaction, and dormant
  reattach.
- Auth provider matrix tests for every admitted method and every fail-closed
  unsupported method.
- Type round-trip matrix for every D9 row.
- Metadata matrix for every D13 row.
- Adapter E2E sample app tests for every claimed adapter.
- Protocol skew tests for old client/new server and new client/old server cases.
- Deterministic refusal tests for unsupported, guarded, conditional, and N/A rows.
- Wire/auth/driver fuzz tests and state-race fault injection.
- Packaging and install smoke tests for every distributable lane.
- Real-route performance budget checks with explicit thresholds.
- Donor compatibility route tests that do not use parser-only shortcuts.
- Machine-readable release declaration generation and validation.
