# Driver Source Import Manifest

Search key: `SCRATCHBIRD_DRIVER_SOURCE_IMPORT_MANIFEST`

Date: 2026-05-08

## Source Boundary

The active driver, adaptor, and tooling source trees were copied from the
adjacent driver development project into the current repository under
`project/drivers/`. The copied source is now local to the public project tree and
must not depend on the old project path at runtime or build time.

## Destination Map

| Category | Destination | Imported entries |
| --- | --- | --- |
| Driver | `project/drivers/driver/` | `cpp`, `dart`, `dotnet`, `elixir`, `go`, `jdbc`, `mojo`, `node`, `odbc`, `pascal`, `php`, `python`, `r`, `ruby`, `rust`, `swift` |
| Adaptor | `project/drivers/adaptor/` | `scratchbird-dbeaver-driver`, `scratchbird-hibernate-dialect`, `scratchbird-metabase-driver`, `scratchbird-prisma-adapter`, `scratchbird-sqlalchemy-dialect`, `scratchbird-superset-driver`, `scratchbird-typeorm-adapter` |
| Tool | `project/drivers/tool/` | `cli` |

## Excluded During Import

The import intentionally excludes generated or non-source artifacts:

- language dependency trees such as `node_modules/`, `vendor/`, `.build/`, `.dart_tool/`, `target/`, and `deps/`;
- local build outputs such as `build/`, `dist/`, `bin/`, `obj/`, `.gradle/`, and `.pytest_cache/`;
- archived evidence/artifact directories copied from old validation runs;
- Python bytecode caches;
- binary package and compiled-object artifacts such as `*.gem`, `*.so`, `*.dll`, `*.o`, `*.a`, and `*.rlib`.

## Benchmark Implication

The Execution_Plan 10 benchmark harness can now be adapted to use current-repo driver
source instead of the old external driver path. Actual benchmark execution still
requires each needed driver/tool to be built against the current server and wire
API from this repository.
