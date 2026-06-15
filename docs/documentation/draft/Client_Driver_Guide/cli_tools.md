# CLI Tools

## Purpose

This page documents the ScratchBird CLI utilities: the primary tools (`sb_isql`, `sb_admin`,
`sb_backup`, `sb_security`, `sb_verify`) and the compatibility front-ends
(`sb_fb_isql`, `sb_my_isql`, `sb_pg_isql`). It also documents `sbdriver_conformance`,
the conformance manifest runner.

All claims are grounded in the source files and package contract:
`project/drivers/tool/cli/sb_isql.cpp`,
`project/drivers/tool/cli/sb_admin.cpp`,
`project/drivers/tool/cli/sb_backup.cpp`,
`project/drivers/tool/cli/sb_security.cpp`,
`project/drivers/tool/cli/sb_verify.cpp`,
`project/drivers/tool/cli/sb_fb_isql.cpp`,
`project/drivers/tool/cli/sb_my_isql.cpp`,
`project/drivers/tool/cli/sb_pg_isql.cpp`,
`project/drivers/tool/cli/README.md`,
`project/drivers/tool/cli/package_contract.json`.

This is a **draft**. CLI tools carry `beta_2` driver status and `release_candidate` release
bucket. Linux is supported with CI coverage; Windows is experimental (CI build attempt
enabled); macOS is untested and not currently in CI.

---

## CLI Component Identity

| Property | Value |
| --- | --- |
| component_id | `tool:cli` |
| driver_family | `native_cli` |
| driver_status | `beta_2` |
| release_bucket | `release_candidate` |
| wire_protocol | `sbwp_v1_1` |
| auth_authority | `engine` |
| transaction_authority | `mga_engine` |
| license | `MPL-2.0` |

Source: `project/drivers/tool/cli/package_contract.json`.

---

## Connection Modes

The network-backed CLI tools (`sb_isql`, `sb_admin`, `sb_security`) support four connection
modes via `--mode`:

| Flag Value | Description |
| --- | --- |
| `--mode=embedded` | Maps to local IPC transport (in-process or local IPC in the beta C++ network client) |
| `--mode=local-ipc` | IPC path to the running server SBPS Unix endpoint (`ipc_method` + `ipc_path` required) |
| `--mode=inet` | Direct TCP listener mode (`direct_listener` ingress) |
| `--mode=managed` | Manager proxy front-door mode (`manager_proxy` ingress) |

Full control is available via `--connection=<connection_string>` with optional
`--conn-opt key=value` overrides.

Source: `project/drivers/tool/cli/README.md` — "Connection Modes".

---

## Auth and Bootstrap Flags

The network-backed tools expose the shared staged auth/bootstrap contract:

| Flag | Purpose |
| --- | --- |
| `--probe-auth-surface` | Perform pre-connect auth negotiation and exit (no full session opened) |
| `--show-auth-context` | Print resolved auth context after a real connect |
| `--auth-token=<tok>` | Supply a generic token-auth payload |
| `--auth-method-id` | Explicit auth method identifier |
| `--auth-method-payload` | Auth method payload |
| `--auth-payload-json` | Auth payload in JSON form |
| `--auth-payload-b64` | Auth payload in base64 form |
| `--auth-provider-profile` | Auth provider profile selector |
| `--auth-required-methods` | Require specific auth methods |
| `--auth-forbidden-methods` | Forbid specific auth methods |
| `--auth-require-channel-binding` | Require channel binding |
| `--workload-identity-token` | Workload identity token |
| `--proxy-principal-assertion` | Proxy principal assertion |

Unsupported admitted methods remain fail-closed through the shared C++ driver surface.

Source: `project/drivers/tool/cli/README.md` — "Auth / Bootstrap".

---

## sb_isql — Interactive SBsql Shell

`sb_isql` is the primary interactive SQL shell for ScratchBird, using the SBsql dialect.
It is Firebird ISQL compatible with PostgreSQL psql extensions.

**Usage:**
```
sb_isql <database_path> [options]
```

**Key options:**

| Flag | Description |
| --- | --- |
| `-U, --user=<username>` | Username |
| `-P, --password=<pass>` | Password (prompted if absent) |
| `-p, --port=<n>` | TCP port (default: 3092) |
| `-H, --host=<host>` | Host (default: localhost) |
| `-c, --command=<sql>` | Execute a single SQL command and exit |
| `-f, --file=<file>` | Execute commands from a file and exit |
| `-o, --output=<file>` | Write output to a file |
| `-t, --tuples-only` | Print tuples only (suppress headers/footers) |
| `-A, --no-align` | Unaligned output mode |
| `-F, --field-separator=<s>` | Field separator (default: `|`) |
| `-q, --quiet` | Quiet mode |
| `-e, --echo` | Echo commands before execution |
| `-b, --bail` | Stop on first error |
| `-v, --verbose` | Verbose mode |
| `--schema-tree` | Print schema tree and exit |

**SET commands (Firebird ISQL compatible):**
`SET BAIL`, `SET TERM`, `SET COUNT`, `SET HEADING`, `SET ECHO`, `SET LIST`, `SET NULL`,
`SET WIDTH`, `SET STATS`, `SET PLAN`, `SET PLANONLY`, `SET EXPLAIN`.

**Meta-commands (psql-style, start with `\`):**
`\?` (help), `\q` (quit), `\d` / `\dt` (list or describe tables), `\di` (indexes),
`\du` (users), `\l` (databases), `\c <database>` (connect to database).

Source: `project/drivers/tool/cli/sb_isql.cpp` — file header doc comment.

---

## sb_admin — Administration Tool

`sb_admin` provides scheduler administration and engine metrics access.

**Usage:**
```
sb_admin <database> <subcommand> [options]
```

**Subcommands:**

| Subcommand | Description |
| --- | --- |
| `job list [--like <pattern>]` | List scheduled jobs |
| `job runs <job_name>` | Show execution history for a job |
| `metrics` | Show engine metrics |
| `jit <compile\|rebuild\|inspect> <object_uuid>` | JIT compile, rebuild, or inspect an object |
| `jit retire <artifact_uuid>` | Retire a JIT artifact |

**Key options:** `-U/--user`, `-P/--password`, `-p/--port`, `--database=<name>`, `-q/--quiet`.

Source: `project/drivers/tool/cli/sb_admin.cpp` — file header doc comment.

---

## sb_backup — Backup and Restore Tool

`sb_backup` creates and restores ScratchBird database backups. Backups use the `.sbbak` format
with optional LZ4 compression and CRC32c integrity checksums.

**Usage:**
```
sb_backup <command> [options]
```

**Commands:**

| Command | Description |
| --- | --- |
| `backup <database> <backup_file>` | Create a backup |
| `restore <backup_file> <database>` | Restore from a backup |
| `verify <backup_file>` | Verify backup file integrity |
| `info <backup_file>` | Show backup metadata |

**Key options:** `--compress`, `--no-compress`, `-v/--verbose`, `-q/--quiet`, `-p/--progress`.

For operator procedures covering backup scheduling, restore validation, and data movement,
see [Operations Administration: Backup, Restore, and Data Movement](../Operations_Administration/backup_restore_and_data_movement.md).

Source: `project/drivers/tool/cli/sb_backup.cpp` — file header doc comment.

---

## sb_security — Security Administration Tool

`sb_security` manages users, roles, permissions, and audit configuration.

**Usage:**
```
sb_security <command> <database> [options]
```

**Command groups:**

| Group | Commands |
| --- | --- |
| User management | `user list`, `user create`, `user delete`, `user password`, `user enable`, `user disable`, `user info`, `user unlock` |
| Role management | `role list`, `role create`, `role delete`, `role grant`, `role revoke`, `role members`, `role grants` |
| Permission | `grant <privilege> on <object> to <user/role>`, `revoke <privilege> on <object> from <user/role>`, `show grants for <user/role>`, `show grants on <object>` |
| Audit | `audit status`, `audit enable [category]`, `audit disable [category]`, `audit log [filter]` |
| Security checks | `check`, `check passwords`, `check permissions`, `check audit` |

**Key options:** `-U/--user`, `-P/--password`, `-p/--port`, `-v/--verbose`, `-q/--quiet`,
`--json` (JSON output format).

For engine-side user and role management policy, see
[Security Guide: standard_roles_and_groups.md](../Security_Guide/standard_roles_and_groups.md) and
[Security Guide: grants_and_privileges.md](../Security_Guide/grants_and_privileges.md).

Source: `project/drivers/tool/cli/sb_security.cpp` — file header doc comment.

---

## sb_verify — Database Verification Tool

`sb_verify` checks database integrity and page consistency. It is used for offline verification
of a database file, not for live connection health.

**Usage:**
```
sb_verify <database_path> [options]
```

**Key options:**

| Flag | Description |
| --- | --- |
| `--full` | Full verification (all checks) |
| `--quick` | Quick check (header and page checksums only) |
| `--pages` | Verify all pages |
| `--repair` | Attempt repair (dangerous — use with caution) |
| `-v/--verbose` | Verbose output |
| `-q/--quiet` | Only show errors |
| `-o/--output=<file>` | Write report to file |

**Exit codes:**

| Code | Meaning |
| --- | --- |
| `0` | No issues found |
| `1` | Minor issues (warnings) |
| `2` | Serious issues (errors) |
| `3` | Critical issues (corruption) |
| `4` | Usage or argument error |

Source: `project/drivers/tool/cli/sb_verify.cpp` — file header doc comment.

---

## sbdriver_conformance — Conformance Manifest Runner

`sbdriver_conformance` runs typed conformance assertions against a live ScratchBird endpoint
using a JSON manifest.

**Manifest assertions supported:**
`expect_columns`, `expect_column_type_oids`, `expect_first_row_json`,
`expect_first_row_types`, `expect_rows_json`.

**Usage:**
```bash
export SB_CONFORMANCE_DSN="scratchbird://user:pass@localhost:3092/mydb?protocol=native"
lanes/active/tooling/cli/conformance/run_sbdriver_conformance_sample.sh
```

**Runner flags:** `--binary-params`, `--text-params`, `--manifest <path>`,
`--output <path>`, `--no-build`.

Source: `project/drivers/tool/cli/README.md` — "Conformance Sample".

---

## Compatibility Front-Ends

### sb_fb_isql — Firebird SQL Compatibility Shell

`sb_fb_isql` provides a Firebird SQL syntax compatibility front-end. It uses the
`FirebirdQueryCompiler` to parse Firebird SQL and compile it to SBLR bytecode for execution.

Key options: `-c/--command`, `-f/--file`, `-q/--quiet`, `-s/--dialect=<1|2|3>` (default: 3),
`--stats`, `--version`.

Source: `project/drivers/tool/cli/sb_fb_isql.cpp` — file header doc comment and includes.

### sb_my_isql — MySQL Compatibility Shell

`sb_my_isql` provides a MySQL wire protocol compatibility front-end for testing. It uses
the `mysql_adapter` from the FDW (Foreign Data Wrapper) layer.

Source: `project/drivers/tool/cli/sb_my_isql.cpp` — file header doc comment and includes.

### sb_pg_isql — PostgreSQL Compatibility Shell

`sb_pg_isql` provides a PostgreSQL wire protocol compatibility front-end for testing. It uses
the `postgresql_adapter` from the FDW layer.

Source: `project/drivers/tool/cli/sb_pg_isql.cpp` — file header doc comment and includes.

**Build note:** The compatibility front-ends (`sb_pg_isql`, `sb_my_isql`, `sb_fb_isql`) require
FDW adapters from the engine repository and are built with `-DSB_BUILD_CLI_FDW=ON`.

Source: `project/drivers/tool/cli/README.md` — "Optional: `-DSB_BUILD_CLI_FDW=ON`".

---

## Build

```bash
cmake -S . -B build_cli -DSB_BUILD_CLI=ON -DSB_BUILD_CPP=ON -DSB_BUILD_ODBC=OFF
cmake --build build_cli --config Release
ctest --test-dir build_cli --output-on-failure
```

Expected build outputs:
`bin/sb_isql`, `bin/sb_admin`, `bin/sb_backup`, `bin/sb_security`, `bin/sb_verify`,
`bin/sbdriver_conformance`.

Source: `project/drivers/tool/cli/package_contract.json` — `artifact_verification`.

---

## Authority Boundaries

The CLI tools follow these authority boundaries (from `package_contract.json`):

- The CLI may shape command-line options, prompts, and conformance manifests.
- The C++ driver (`driver:cpp`) handles transport and protocol binding.
- The server revalidates authentication, authorization, SBLR, UUID, cache, schema, and
  transaction claims.
- MGA transaction finality remains engine-owned.

The CLI never auto-replays a whole transaction. Retry rule: `40xxx` requires a fresh statement
boundary; `08xxx` requires reconnect or reopen.

Source: `project/drivers/tool/cli/package_contract.json` — `delegation_posture`.

---

## Cross-References

- [connection_and_dsn.md](connection_and_dsn.md) — DSN keys and ingress modes used by the CLI
- [authentication.md](authentication.md) — auth methods the CLI tools negotiate
- [conformance_baseline.md](conformance_baseline.md) — sbdriver_conformance and the S1-S5 gates
- [Operations Administration: Backup, Restore, and Data Movement](../Operations_Administration/backup_restore_and_data_movement.md) — operator backup procedures
- [Operations Administration: Service Lifecycle](../Operations_Administration/service_lifecycle.md) — service state context for CLI operations
- [Security Guide: authentication_and_providers.md](../Security_Guide/authentication_and_providers.md) — engine-side auth provider detail
