# Build Artifact Isolation Policy

Search key: `DRIVER_BUILD_ARTIFACT_ISOLATION_POLICY`

All generated driver/adaptor/tool output must be rooted under:

```text
build/drivers/
```

Required subroots:

- `build/drivers/driver/<name>/`
- `build/drivers/adaptor/<name>/`
- `build/drivers/tool/<name>/`
- `build/drivers/_deps/<ecosystem>/`
- `build/drivers/_logs/<ctest-label>/`
- `build/drivers/_packages/<category>/<name>/`
- `build/benchmarks/execution_plan10/`

Forbidden source-tree outputs:

- build directories: `build/`, `dist/`, `target/`, `.build/`, `.dart_tool/`,
  `.gradle/`, `bin/`, `obj/`, `Testing/`
- dependency trees: `node_modules/`, `vendor/`, `deps/`
- caches: `__pycache__/`, `.pytest_cache/`
- binary artifacts: `*.a`, `*.dll`, `*.dylib`, `*.exe`, `*.gem`, `*.o`,
  `*.pyc`, `*.rlib`, `*.so`

The CTest gate `driver_build_artifact_isolation_gate` fails if any forbidden
output exists below `project/drivers/`.
