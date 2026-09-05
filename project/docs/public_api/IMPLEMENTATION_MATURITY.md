# Public Capability Implementation Maturity

ScratchBird reports the current maturity of each public engine/API capability
in `project/src/engine/internal_api/SBLR_API_OPERATION_MATRIX.yaml`. The
`implementation_maturity` field is the authority for maturity. The matching
field in `ENGINE_API_SURFACE_REGISTRY.yaml` is checked for equality. Older
`current_implementation_status`, `implementation_status`, and
`executor_readiness_status` fields describe compatibility, provider, or routing
details; they are not maturity claims.

Each capability has exactly one of these current levels:

1. `codec_contract` — request, descriptor, or result encoding, decoding, and
   validation exist. No dispatch or execution is claimed.
2. `routing` — admission, rejection, dispatch, or transfer to another authority
   is observed. No engine-owned state change is claimed.
3. `logical_implementation` — the engine produces the documented semantic
   result or in-process state transition. Physical storage and restart survival
   are not claimed.
4. `physical_implementation` — tests observe an authoritative local catalog,
   row-storage, MGA, lifecycle, or provider effect in a running instance.
   Restart survival is not claimed.
5. `durability_proven` — the same authoritative effect is observed after an
   explicit close/reopen, process restart, or kill/recovery boundary. The
   matrix row must cite the restart-boundary test in `maturity_evidence`.
6. `production_qualified` — durability evidence is supplemented by the
   supported-platform, sanitizer, fuzz, soak, resource, recovery, packaging,
   and operational evidence required by the current release policy, followed
   by an explicit release qualification decision.

Levels are ordered and cumulative. A component advances only after observed
behavior satisfies the next level. Source-file presence, a registered opcode,
a generated test, a passing codec round trip, or a legacy status label cannot
promote it. A higher-level claim must never be inferred for callers or wrappers
from the maturity of a lower-level component.

## SBLR runtime filenames

The `runtime` suffix under `project/src/engine/sblr/` is a historical module
name. It identifies code used at runtime, not a completed executor. These files
may contain only request/descriptor/result carriers and codecs. Review the
operation matrix and follow the dispatch and engine API paths before making an
execution claim.

For example, `sblr_ddl_create_table_runtime.cpp` implements CREATE TABLE wire
encoding and decoding. Physical CREATE TABLE behavior is not established by
that file. The public operation is routed by `sblr_dispatch.cpp` to
`internal_api::EngineCreateTable`, whose implementation is in
`engine/internal_api/ddl/create_api.cpp` and its included implementation units.
The `ddl.create_table` matrix row records that execution path explicitly.

By contrast, the current `engine.op.ddl_create_fdw` row stops at
`codec_contract`: its registered implementation is the SBLR request,
descriptor, and result codec, and no deeper executor path is claimed. SBLR
descriptor coordinators stop at `routing` unless separate engine-owned semantic
behavior is observed.

## Current claim boundary

The matrix currently makes no `durability_proven` or `production_qualified`
claim. Existing restart, recovery, soak, and release tests remain useful
evidence, but a capability is promoted only when its row cites the applicable
evidence and the maturity policy gate accepts it. This deliberately prevents a
general project-level test result from becoming an unsupported per-capability
durability or production claim.

Run the policy check from the repository root with:

```sh
python3 project/tools/release/implementation_maturity_gate.py
```
