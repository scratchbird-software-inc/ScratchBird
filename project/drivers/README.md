# Drivers

Client drivers, integration adaptors, and CLI/tooling source live here.

## Layout

- `driver/`: actual client drivers and language bindings.
- `adaptor/`: framework, ORM, BI, and application integration adaptors.
- `tool/`: command-line and operational tools.

Drivers, adaptors, and tools must not link directly to private engine internals.
They must communicate through the supported public server, wire, parser, or
driver APIs for their category.

## Imported Source

This tree was initially imported from the historical driver project into the
current repository:

- `ScratchBird-driver/lanes/active/drivers/` to `driver/`
- `ScratchBird-driver/lanes/active/adapters/` to `adaptor/`
- `ScratchBird-driver/lanes/active/tooling/` to `tool/`

Generated dependency trees, build outputs, caches, and binary package artifacts
were intentionally excluded from the import. Rebuild those locally from source
inside the current repository when needed.

## CTest Gates

Current-repo driver gates are enabled from the project build with:

```bash
cmake -S project -B build/driver_gates -DSB_BUILD_DRIVER_GATES=ON
ctest --test-dir build/driver_gates -L driver --output-on-failure
```

The CTest runner stages ecosystems that normally write into their source tree
and redirects build output, dependency caches, package output, bytecode caches,
and logs under `build/drivers`.
