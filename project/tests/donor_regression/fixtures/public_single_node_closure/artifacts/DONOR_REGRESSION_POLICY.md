# Donor Regression Policy

Search key: `PUBLIC_SINGLE_NODE_DONOR_REGRESSION_POLICY`

Donor engines and donor tools are compatibility inputs and regression oracles.
They are never ScratchBird storage, transaction, recovery, or security
authority.

Required behavior:

- Use local donor source/tool references when available.
- Compile the donor tools needed to run original donor regression tests.
- Route donor regression inputs through the ScratchBird parser/UDR path and
  engine route, not through a hidden donor backend.
- Missing donor source/toolchain is an environment-missing result and blocks
  final closure for that donor row unless the contract marks the row as a
  capability-reference refusal surface.
- SQL Server, Oracle, and DB2 rows are capability-reference rows: closure is
  exact capability matrix, diagnostics, and refusal/bridge policy, not hidden
  commercial engine emulation.
