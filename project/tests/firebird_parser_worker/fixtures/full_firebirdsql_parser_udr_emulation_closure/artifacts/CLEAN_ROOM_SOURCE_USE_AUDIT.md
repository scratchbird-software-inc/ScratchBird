# Firebird Clean-Room Source Use Audit

Status: seeded
Search key: `FIREBIRD_CLEAN_ROOM_SOURCE_USE_AUDIT`

## Source Evidence Files Consulted

- `project/tests/donor_regression/donor_release_acquisition/firebird/5.0.4/source`
- `project/tests/donor_regression/donor_release_acquisition/firebird/5.0.4/regression`
- `project/tests/donor_regression/donor_catalog_seeds/firebird`

## Behavior Facts Extracted

- Firebird SQL, PSQL, BLR, SQLDA, DPB, TPB, SPB, BPB, service, utility, diagnostic, catalog, and version-profile surfaces are mapped to ScratchBird-owned implementation or emulation rows.
- Donor source and QA files are behavior evidence for matrices and CTest harnesses only.
- Donor tools are built and invoked only from donor-native CTest lanes.

## Generated Matrix Rows Created

- `FIREBIRD_GRAMMAR_API_BLR_SBLR_MATRIX.csv`
- `FIREBIRD_BLR_PARAMETER_BUFFER_MATRIX.csv`
- `FIREBIRD_DONOR_TOOL_BUILD_MANIFEST.csv`
- `FIREBIRD_QA_DONOR_REPLAY_MANIFEST.csv`
- `FIREBIRD_CTEST_REQUIRED_GATES.csv`

## ScratchBird Implementation Files Touched

- `project/src/parsers/donor/firebird`
- `project/src/udr/sbu_firebird_parser_support`
- `project/tests/firebird_parser_worker`

## Static-Copy Checks

- `firebird_clean_room_provenance_gate` scans Firebird implementation roots for donor license text, donor include paths, Firebird client-library tokens, and donor cryptography/math runtime tokens.
- The gate is paired with `firebird_package_boundary_gate` and `firebird_tool_runtime_isolation_gate`.

## Runtime Link Dependency Checks

- Runtime parser and UDR binaries are checked with `ldd`.
- Donor `libfbclient`, `libtommath`, and `libtomcrypt` are allowed only in the donor-native CTest toolchain under `build/donor`.

## Final Clean-Room Conclusion

The current Firebird parser, UDR, wire descriptor, and test harness seed implementation is ScratchBird-owned code derived from execution_plan matrices and behavior evidence. Donor code and donor libraries remain test-only inputs.
