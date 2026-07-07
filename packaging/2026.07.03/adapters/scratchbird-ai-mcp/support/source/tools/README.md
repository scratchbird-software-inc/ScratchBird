# Tools

Local helper scripts for validation, evidence generation, and stack startup.

- `tools/validate_capability_matrix.py` validates `capability/capability-matrix.v0.json`.
- `tools/generate_ai_conformance_artifacts.py` regenerates `artifacts/ai_conformance/` for the current commit, including replay and attestation evidence.
- `tools/validate_evidence_gates.py` validates release evidence artifacts against `docs/releases/EARLY_BETA_CONFORMANCE_GATES.md`.
- `tools/validate_release_candidate.py` validates claimed interface profiles against the active evidence gates plus profile-specific certification artifacts.
- `tools/run_local_bridge.sh` starts the HTTP bridge (`python -m scratchbird_ai.http_bridge`).
- `tools/run_local_stack.sh` starts bridge + MCP server in one command for local adapter testing.
- `tools/smoke_http_contract.py` runs HTTP contract smoke tests (`--mode selftest` or `--mode live`).
- `tools/run_live_native_conformance.py` runs a fail-closed live-native certification harness, including service-internal explain/workload/audit probes plus runtime-mode metadata, and writes machine-readable artifacts under `artifacts/live_native_conformance/` by default.
