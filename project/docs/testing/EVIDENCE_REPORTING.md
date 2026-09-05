# Test and Release Evidence Reporting

ScratchBird reports evidence in descending order of strength:

1. `runtime_observable_behavior` — the test executes the implementation and
   asserts externally observable results or engine-owned state transitions.
2. `durable_reopen_verification` — the test observes the claimed state after an
   explicit close/reopen, restart, or kill/recovery boundary.
3. `process_level_verification` — independently running processes exercise the
   public listener, server, tool, or packaging boundary.
4. `model_property_testing` — generated cases, state models, or properties are
   evaluated against an executing implementation.
5. `static_contract` — compilation, schema, ABI, configuration, or structural
   checks enforce a contract without demonstrating runtime behavior.
6. `source_token_check` — source text, filenames, hashes, or artifact counts
   guard a narrowly named regression. This is the lowest evidence tier.

The order is a reporting hierarchy, not a substitution rule. Passing a lower
tier cannot satisfy a missing higher tier.

## Source-token classifications

A source-token check must declare exactly one purpose:

- `repository_policy` protects repository layout, CI, licensing, public/private
  boundaries, or release-process policy.
- `source_contract` protects a specific static code or configuration contract.
- `evidence_inventory` records the expected coverage or artifact inventory but
  does not execute the referenced tests.

Every retained source-token check names the distinct regression it prevents.
It must also exclude claims of a behavioral pass, crash proof, fuzz execution,
sanitizer qualification, and durability proof. Source-token checks cannot
promote `implementation_maturity`.

The public release CTest labels for these checks include exactly one purpose
classification together with `source_token_check` and `non_behavioral`. Public
CI runs the retained inventories in `static-policy` and excludes their CTest
registrations from `unit-runtime-tests`.

## Current retained source-token inventories

| Check | Classification | Distinct protected regression |
| --- | --- | --- |
| `public_durable_codec_fuzz_coverage_inventory` | `evidence_inventory` | Durable-codec malformed-input and authority-refusal declarations disappear or stop naming their tests. |
| `public_crash_fault_source_contract_matrix` | `source_contract` | Required crash/fault refusal and MGA-authority contract cases disappear from the public source surface. |
| `public_release_soak_coverage_inventory` | `evidence_inventory` | Bounded soak declarations lose required workloads, budgets, iteration limits, or diagnostic artifacts. |

The former crash-source-contract and soak-inventory wrapper gates were removed.
They only re-read the retained carrier and checked that another gate existed;
they did not protect an additional runtime or policy invariant.
