# Tools

Product and developer tools. The first code-stage tool is expected to be `sb_registry_lint`.

## LLVM local tool dependency

`project/tools/llvm/lib/` is a local generated dependency location. Built LLVM
shared libraries placed there are generated artifacts and must not be tracked in
git. A developer or build agent that needs LLVM-backed JIT/AOT probes must build
or install the required LLVM version locally and copy or symlink the generated
runtime library into this directory before running those probes.

## Engine public ABI boundary

`project/tools` is not the public `sb_engine` ABI conformance surface.

The public `sb_engine` ABI conformance fixtures live under:

```text
project/tests/engine_public_abi/
```

Those fixtures must include only public headers under `project/include/scratchbird/engine/` and must link the public `sb_engine` target or installed `ScratchBird::sb_engine` target.

Existing tools that link `sb_engine_internal_api`, `sb_engine_sblr`, optimizer, planner, executor, metrics, storage, security, or other private module targets are explicitly classified as internal implementation/developer probes. Their names are retained for historical continuity, but they are not external ABI promises and must not be referenced as public conformance evidence.

Rules:

- New public ABI tests must be added under `project/tests/engine_public_abi/` or another explicitly named public ABI test subtree.
- New `project/tools` probes may use private engine internals only when they are intentionally internal/developer probes and are gated by an explicit CMake option.
- `sb_server`, `sb_listener`, `sbmn_manager`, parser workers, and future external consumers must not include or link private engine internals.
- Any future migration of a legacy tool probe to public ABI must remove private target links and private includes in the same change.
