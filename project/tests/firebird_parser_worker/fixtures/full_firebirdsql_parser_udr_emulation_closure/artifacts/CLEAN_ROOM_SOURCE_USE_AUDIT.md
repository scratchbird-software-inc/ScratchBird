# Firebird Clean-Room Source Use Audit

Status: seeded
Search key: `FIREBIRD_CLEAN_ROOM_SOURCE_USE_AUDIT`

## Source Evidence Files Consulted

- `project/tests/reference_regression/reference_release_acquisition/firebird/5.0.4/source`
- `project/tests/reference_regression/reference_release_acquisition/firebird/5.0.4/regression`
- `project/tests/reference_regression/reference_catalog_seeds/firebird`

## Behavior Facts Extracted

- Firebird SQL, PSQL, BLR, SQLDA, DPB, TPB, SPB, BPB, service, utility, diagnostic, catalog, and version-profile surfaces are mapped to ScratchBird-owned implementation or emulation rows.
- Reference source and QA files are behavior evidence for matrices and CTest harnesses only.
- Reference tools are built and invoked only from reference-native CTest lanes.

## Generated Matrix Rows Created

- `FIREBIRD_GRAMMAR_API_BLR_SBLR_MATRIX.csv`
- `FIREBIRD_BLR_PARAMETER_BUFFER_MATRIX.csv`
- `FIREBIRD_REFERENCE_TOOL_BUILD_MANIFEST.csv`
- `FIREBIRD_QA_REFERENCE_REPLAY_MANIFEST.csv`
- `FIREBIRD_CTEST_REQUIRED_GATES.csv`

## ScratchBird Implementation Files Touched

- `project/src/parsers/compatibility/firebird`
- `project/src/udr/sbu_firebird_parser_support`
- `project/tests/firebird_parser_worker`

## Static-Copy Checks

- `firebird_clean_room_provenance_gate` scans Firebird implementation roots for reference license text, reference include paths, Firebird client-library tokens, and reference cryptography/math runtime tokens.
- The gate is paired with `firebird_package_boundary_gate` and `firebird_tool_runtime_isolation_gate`.

## Runtime Link Dependency Checks

- Runtime parser and UDR binaries are checked with `ldd`.
- Reference `libfbclient`, `libtommath`, and `libtomcrypt` are allowed only in the reference-native CTest toolchain under `build/reference`.

## Final Clean-Room Conclusion

The current Firebird parser, UDR, wire descriptor, and test harness seed implementation is ScratchBird-owned code derived from execution_plan matrices and behavior evidence. Reference code and reference libraries remain test-only inputs.
