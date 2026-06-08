# LLVM Tool Dependency Staging

This directory stages compiled LLVM libraries used by ScratchBird private tooling and runtime capability detection.

## Current staged library

- `lib/libLLVM-23.so`

## Rules

- Set `LLVM_SOURCE_ROOT` when a local LLVM source checkout is needed. CMake
  accepts that path as an explicit cache/provenance input.
- This tools directory may stage compiled LLVM libraries when the local checkout has not produced a build artifact yet.
- CMake must prefer this staged tools path before falling back to local LLVM
  build directories or system library paths.
- ScratchBird defaults to dynamic LLVM loading. Static LLVM linkage is allowed
  only through the explicit `SB_LLVM_LINK_MODE=static` configuration.
- Staged LLVM libraries are private implementation dependencies and must not be published with open-source packages.
