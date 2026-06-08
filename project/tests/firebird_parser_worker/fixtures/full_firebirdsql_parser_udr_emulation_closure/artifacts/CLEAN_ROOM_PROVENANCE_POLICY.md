# Firebird Clean-Room Provenance Policy

Status: seeded
Search key: `FIREBIRD_CLEAN_ROOM_PROVENANCE_POLICY`

## Rule

Firebird source code under `docs/reference/` is read-only behavior evidence. ScratchBird Firebird implementation files must be original ScratchBird code.

## Allowed Use

- Read Firebird source to identify grammar, API, BLR, parameter-buffer, status-vector, tool, and regression behavior.
- Record behavior facts in execution_plan artifacts and generated matrices.
- Write ScratchBird implementation code from the generated matrices and canonical contracts.

## Forbidden Use

- Copy Firebird implementation code into `project/src/parsers`, `project/src/udr`, `project/src/server`, or `project/src/engine`.
- Port Firebird parser functions, runtime functions, storage code, utility code, or generated parser actions directly.
- Link ScratchBird runtime products against Firebird libraries or donor-native tools.
- Treat Firebird source comments as product documentation in ScratchBird implementation files.

## Required Gate

`firebird_clean_room_provenance_gate` must inspect implementation paths, generated artifacts, and build targets. It fails on copied donor implementation code, runtime link dependency on Firebird libraries, or missing provenance for generated matrices.
