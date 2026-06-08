# Firebird Donor Tool Sandbox Policy

Status: draft
Search key: `FIREBIRD_DONOR_TOOL_SANDBOX_POLICY`

## Rule

Firebird donor-native tools are test-only artifacts. They must run inside isolated CTest-owned directories and must never install system files, open external network connections, or become ScratchBird runtime dependencies.

## Required Controls

- Build tools in a CTest/external-project scratch directory.
- Run tools with loopback-only endpoints.
- Disable external network access in the harness.
- Use per-test temporary database and output directories.
- Use strict wall-clock timeouts.
- Capture stdout, stderr, exit status, status vectors, normalized output, and raw output.
- Delete temporary sockets, files, and processes after each test unless evidence retention is enabled.
- Fail if a donor tool attempts system install, global config mutation, uncontrolled filesystem writes, or external network access.

## Required Gate

`firebird_donor_tool_sandbox_gate` proves these controls before donor-native replay can run.
