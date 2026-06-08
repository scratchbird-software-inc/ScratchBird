# Deterministic Generation And No-Network Plan

Search key: `SBSQL-SURFACE-SBLR-DETERMINISTIC-GENERATION-NO-NETWORK`

## Purpose

Generated SBsql ledgers, parser tables, fixtures, per-row evidence, and release declarations must reproduce from repo-local inputs only. No implementation closure gate may require internet access.

## Required Inputs

- `public_input_snapshot*.csv`
- `public_contract_snapshot*.yaml`
- `project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/public_proof/artifacts/*.csv`
- `project/tools/sb_parser_gen/*`
- `project/tests/sbsql_parser_worker/generated/*`

## Required Outputs

- regenerated strict row coverage ledger
- regenerated per-row evidence manifest
- regenerated parser registry artifacts
- regenerated CTest fixture manifests
- regenerated release declaration CSV/JSON
- checksum report for generated artifacts

## Gate Rule

The CTest gate `sbsql_deterministic_generation_no_network_gate` must run with network unavailable or denied, regenerate the deterministic outputs into a temporary build artifact directory, compare checksums with tracked outputs, and fail on drift.
