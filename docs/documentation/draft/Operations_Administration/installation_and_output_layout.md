# Installation And Output Layout

Before you can run ScratchBird you need to understand what a staged build output looks like — which files are required, which are optional, where they go, and what the relationships between them are. This chapter answers those questions. It is intentionally concrete: the paths, filenames, and groupings here reflect what the build actually produces, not an idealized layout.

## What A Staged Output Is

ScratchBird does not install into a system-wide prefix like `/usr/local`. Instead, the build system stages a self-contained output tree under `output/<platform>` within the build directory (where `<platform>` is a string such as `linux`). This approach means the entire deployment can be inspected, copied, and relocated as a directory tree. The `SB_BUILD_PUBLIC_STANDALONE_OUTPUT` CMake option, which is `ON` by default, drives this staging step.

A staged output is not a source tree, not a CMake install prefix, and not a live database location. The output tree contains only the files that belong to the runtime deployment; source files, build artifacts, and intermediate objects are not included.

## Top-Level Directory Structure

The staged output on Linux follows a conventional Unix-like layout:

```
output/linux/
  bin/          executable binaries
  lib/          shared libraries
  etc/          configuration templates
  share/        resources, documentation, and examples
  pdb/          platform debug information (where applicable)
  STANDALONE_OUTPUT_MANIFEST.json
```

Every path used in configuration files shipped with the output — such as `bin/SBgate` in `SBsrv.conf` — is relative to this root. An operator who relocates the tree should update those paths accordingly, or run the processes from the output root so that relative paths resolve correctly.

## Binaries

### Runtime Binaries

The core runtime components are placed in `bin/`:

| Binary | Internal Target | Role |
| --- | --- | --- |
| `SBgate` | `sb_listener` | Listener: accepts TCP connections, runs the parser pool, hands off authenticated sessions to the IPC server |
| `SBsrv` | `sb_server` | IPC server: hosts the engine, manages database opens, routes parser traffic |
| `SBmgr` | `sbmn_manager` | Single-node manager: supervises SBsrv and SBgate, exposes a proxy port, manages restart policy |
| `SBParser` | `sbp_sbsql` | Core parser worker for the native SBsql dialect |

Each binary is named by its public brand (`sb_public_brand_target` in `CMakeLists.txt:3733-3737`). The internal CMake target names are different and are not visible to administrators.

### Shared Library

`lib/libSBcore.so` (on Linux) is the ScratchBird engine shared library. Applications that embed the engine directly link against this library rather than running SBsrv. The library's public brand name is `SBcore`; the static variant is `SBcore_static` and is not placed in the staged output.

### Command-Line Utilities

Five administrative utilities are placed in `bin/`:

| Binary | Internal Target | Role |
| --- | --- | --- |
| `SBsql` | `sb_isql` | Interactive SQL client (SBsql CLI) |
| `SBadm` | `sb_admin` | Administrative operations |
| `SBbak` | `sb_backup` | Backup and restore manager |
| `SBsec` | `sb_security` | Security administration |
| `SBdoc` | `sb_verify` | Doctor: verification and diagnostics |
| `SBcop` | `sbdriver_conformance` | Conformance officer: driver conformance testing |

Not all utilities may be present in every build, depending on which CMake targets were enabled.

## Configuration Templates

The `etc/scratchbird/` directory contains configuration templates for every service. These are the canonical starting point for any deployment. The templates are copied from `project/config/templates/` during the staging step (`CMakeLists.txt:2827-2828`).

| File | Service |
| --- | --- |
| `SBsrv.conf` | IPC server (SBsrv) |
| `SBgate.conf` | Listener (SBgate) |
| `SBmgr.conf` | Single-node manager (SBmgr) |
| `SBParser.conf` | Core parser (SBParser) |
| `SB_PGSQL_Parser.conf` | PostgreSQL-compatibility parser worker |
| `SB_MYSQL_Parser.conf` | MySQL-compatibility parser worker |
| `SB_MARIADB_Parser.conf` | MariaDB-compatibility parser worker |
| `SB_SQLITE_Parser.conf` | SQLite-compatibility parser worker |
| `SB_FBSQL_Parser.conf` | Firebird-compatibility parser worker |
| `SB_DUCKDB_Parser.conf`, `SB_CLICKHOUSE_Parser.conf`, ... | Additional dialect workers |

The full set of parser templates corresponds to the set of compatibility parser-worker build options available in `CMakeLists.txt`. Not all parser workers are built by default; their configuration templates are installed regardless so that operators can prepare for future enablement.

See [Configuration Reference](configuration_reference.md) for the contents and keys of each file.

## Resources

`share/scratchbird/resources/` contains the data files that the engine loads at runtime. The resource tree is installed from `project/resources/` (`CMakeLists.txt:2824-2826`). The staged output includes subdirectories for:

- `cluster-catalog/` — cluster catalog seed data
- `policy-packs/` — policy pack definitions
- `seed-packs/` — seed data packs

These directories must be present and readable when the engine starts. If a resource file is missing or corrupt, the engine will refuse to start or will fall back to built-in defaults, depending on the resource type and the configuration.

Additional resource material may be present in `share/scratchbird/docs/` and `share/scratchbird/examples/` in builds where those targets are enabled.

## Runtime Directories

The staged output does **not** contain a `runtime/` directory. Runtime directories are created by the processes themselves the first time they run. The default paths used in the shipped configuration templates are relative paths like `runtime/data`, `runtime/control`, and `runtime/listener/control`. If you run a service from the output root, these will be created under the output root. In a production deployment you will typically want to redirect runtime directories to a dedicated location outside the output tree — such as a directory under `/var/lib/scratchbird` for data and `/run/scratchbird` for sockets and PID files — by editing the configuration files before starting any service.

## Manifest

`STANDALONE_OUTPUT_MANIFEST.json` documents the contents of the staged output. Operators can use this file to verify that expected binaries and resources are present and to cross-check the build configuration that produced the output.

## What Belongs Where

| Category | Lives In | Notes |
| --- | --- | --- |
| Executables and shared library | `bin/`, `lib/` | Do not modify after staging |
| Configuration | `etc/scratchbird/` | Copy and edit for deployment; do not edit in place |
| Resources | `share/scratchbird/resources/` | Do not modify; must match the binary version |
| Runtime sockets and PID files | Configured paths, not in output tree | Created by processes at startup |
| Database files | Configured `data_dir`, not in output tree | Created by the engine when a database is first opened |
| Log files | Configured `log_file`, not in output tree | Defaults to stderr |

## Verifying the Output

Before using a staged output for the first time, confirm that:

1. All expected binaries are present in `bin/` and are executable.
2. All four service configuration templates are present in `etc/scratchbird/`.
3. The `share/scratchbird/resources/` tree is present and non-empty.
4. The `STANDALONE_OUTPUT_MANIFEST.json` is readable.

The `SBdoc` (Doctor) utility and the `SBcop` (Conformance Officer) utility provide additional validation. Consult the [Release Validation Checklist](release_validation_checklist.md) for a structured pre-deployment checklist.

## Related Pages

- [Configuration Reference](configuration_reference.md)
- [Release Validation Checklist](release_validation_checklist.md)
- [Getting Started: Configuration Basics](../Getting_Started/administration/configuration_basics.md)
