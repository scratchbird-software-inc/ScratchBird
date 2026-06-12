# Reference Regression Policy

Search key: `PUBLIC_SINGLE_NODE_REFERENCE_REGRESSION_POLICY`

Reference engines and reference tools are compatibility inputs and regression oracles.
They are never ScratchBird storage, transaction, recovery, or security
authority.

Required behavior:

- Use local reference source/tool references when available.
- Compile the reference tools needed to run original reference regression tests.
- Route reference regression inputs through the ScratchBird parser/UDR path and
  engine route, not through a hidden reference backend.
- Missing reference source/toolchain is an environment-missing result and blocks
  final closure for that reference row unless the contract marks the row as a
  capability-reference refusal surface.
- SQL Server, Oracle, and DB2 rows are capability-reference rows: closure is
  exact capability matrix, diagnostics, and refusal/bridge policy, not hidden
  commercial engine emulation.
