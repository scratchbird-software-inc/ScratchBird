# ScratchBird Embedding and API Reference

This manual documents the public API surface that embedders use to link ScratchBird's core engine library (SBcore) directly into an application process. It is the reference for everyone who:

- links `ScratchBird::sb_engine` as a CMake target and calls the C ABI functions,
- uses the thin C++ RAII wrappers in `scratchbird::engine`,
- constructs and submits SBLR execution envelopes through the engine dispatch path, or
- needs to understand the ABI versioning contract and stability policy.

This manual does not cover server-mode operation, the wire protocol, driver packages, or SBsql syntax. For those topics see the cross-links below.

## Documented Invariant

**The engine executes SBLR/internal authority only. SQL text is not runtime authority inside the engine.** SQL text provided to a parser package is translation evidence; it becomes runtime authority only after a validated SBLR envelope is accepted and dispatched through the engine API and security gates. This invariant is stated in `CORE_BETA_PUBLIC_API_ABI.md` (`engine_sblr_internal_api_only`) and is enforced throughout all public surfaces.

## ABI/Versioning Summary

| Attribute | Value |
| --- | --- |
| ABI family | `sb_engine_public_abi` |
| Version | `1.0.0` |
| Packed macro | `SB_ENGINE_ABI_VERSION_PACKED` = `0x00010000` (65536) |
| Source header | `project/include/scratchbird/engine/version.h` |
| Runtime version function | `sb_engine_abi_version_packed()` |
| Build ID function | `sb_engine_abi_build_id()` |
| Freeze ID | `core_beta_public_api_abi_freeze_2026_05` |

## Directory Map

| Chapter | Purpose |
| --- | --- |
| [Overview and ABI](overview_and_abi.md) | What the embedding API is, SBcore as the in-process library, ABI version, header inventory, and public-vs-internal distinctions. |
| [Lifecycle: Engine, Session, Transaction](lifecycle_engine_session_transaction.md) | The object/lifecycle model: engine handle, session, transaction. Construction, teardown, ownership, and the C++ RAII wrappers. |
| [Types, Descriptors, and Values](types_descriptors_and_values.md) | The canonical type model from `descriptor.hpp`, `execution_type_descriptor.hpp`, `types.hpp`, and `value.hpp`. |
| [Results and Cursors](results_and_cursors.md) | How `result.hpp` returns and surfaces result data, row batches, command completion, and execution summaries. |
| [Diagnostics and Errors](diagnostics_and_errors.md) | Error codes, diagnostic severity, the diagnostic set view, and how refusals surface at the API boundary. |
| [SBLR Envelope](sblr_envelope.md) | The SBLR request envelope as the engine-facing execution representation: encoding, dispatch, and the Priority D registry. |
| [Compatibility and Stability Policy](compatibility_and_stability_policy.md) | What is stable, what may change, how ABI version is checked at runtime, and the removal gate. |
| [Examples](examples.md) | Walk-through of the real programs and scripts in `project/examples/`. |

## Reading Model

If you are integrating SBcore for the first time, read [Overview and ABI](overview_and_abi.md) to orient yourself, then [Lifecycle: Engine, Session, Transaction](lifecycle_engine_session_transaction.md) to understand the object model, then [SBLR Envelope](sblr_envelope.md) to understand how work is submitted. The [Examples](examples.md) chapter shows the real end-to-end C smoke program alongside the QA shell scripts.

If you are auditing ABI stability, go directly to [Compatibility and Stability Policy](compatibility_and_stability_policy.md).

## Cross-Links

- Embedded engine operating mode: [../Getting_Started/operating_modes/embedded_engine.md](../Getting_Started/operating_modes/embedded_engine.md)
- SBsql language reference: [../Language_Reference/README.md](../Language_Reference/README.md)
- Operations and Administration Guide: [../Operations_Administration/README.md](../Operations_Administration/README.md)

## Draft Status

This is a draft manual. Every function name, type name, constant, and enum value has been verified against the public header files listed in `CORE_BETA_PUBLIC_API_ABI_MANIFEST.json`. Claims that could not be verified from source have been omitted and are noted in individual chapters. No performance promises, production-readiness claims, or compatibility promises beyond what the freeze document explicitly states are made here.
