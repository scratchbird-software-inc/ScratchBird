# ScratchBird Linux Configuration Directory

This guide describes the Linux host configuration directory used by the
2026.07.03 pre-release package when installed under `/opt/ScratchBird`.

## Directory Contract

Use `/etc/scratchbird` for host-specific configuration and secrets. Use
`/opt/ScratchBird` for package-owned binaries, libraries, resources, examples,
and documentation.

Recommended layout:

```text
/etc/scratchbird/
  SBsrv.conf
  SBgate.conf
  SBmgr.conf
  SBParser.conf
  env.d/
  parsers/
  policies/
  tls/
```

The package templates are staged in:

```text
server/ipc-server/config/SBsrv.conf
server/ipc-server/config/SBgate.conf
server/ipc-server/config/SBmgr.conf
server/sbparser/config/SBParser.conf
```

Copy those templates into `/etc/scratchbird` during installation, then make
host-specific edits in `/etc/scratchbird`. Do not edit the package copy under
`/opt/ScratchBird`.

## Ownership And Permissions

Recommended ownership:

```text
/etc/scratchbird                 root:scratchbird 0750
/etc/scratchbird/*.conf          root:scratchbird 0640
/etc/scratchbird/env.d           root:scratchbird 0750
/etc/scratchbird/parsers         root:scratchbird 0750
/etc/scratchbird/policies        root:scratchbird 0750
/etc/scratchbird/tls             root:scratchbird 0750
/etc/scratchbird/tls/private.key scratchbird:scratchbird 0600
```

TLS certificate chains may be group-readable when required by the listener.
Private keys should be readable only by the runtime account that starts
`SBgate` or by root before privilege drop.

## Configuration Files

`SBsrv.conf` configures the server process. It owns the database path,
runtime/control directories, server logging, local password provider state,
memory/cache policy, metrics policy, listener launch policy, and the SBPS
endpoint used by parser workers.

`SBgate.conf` configures the native listener. It owns bind address, port,
TLS requirement, parser package identity, parser executable path, warm worker
pool sizing, accept limits, and the server endpoint used for parser handoff.

`SBmgr.conf` configures the local single-node manager. It owns manager runtime
paths, control paths, restart policy, drain timeout, and the commands used to
start `SBsrv` and `SBgate`.

`SBParser.conf` documents the default SBsql parser worker profile. In normal
server operation, `SBParser` is launched by the listener with worker-specific
environment and command-line values. Keep this file with the other host
configuration so the installed parser profile and cache/resource limits are
visible to administrators.

## Resource Paths

Install shared resources under:

```text
/opt/ScratchBird/resources/
```

The staged resource package is:

```text
server/resources/resources/
```

It contains:

- `policy-packs/default-local-password/` for create-time default security and
  policy material. The required policy defaults are in
  `policy-packs/default-local-password/policies/policy_defaults.json`; new
  database creation validates this file through the pack manifest and then uses
  the durable catalog copy as authority.
- `seed-packs/initial-resource-pack/` for create-time character set, collation,
  timezone, locale, diagnostic, and language seed material.
- `seed-packs/initial-resource-pack/resources/charsets/`
- `seed-packs/initial-resource-pack/resources/collations/`
- `seed-packs/initial-resource-pack/resources/timezones/`
- `seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack/`
- `cluster-catalog/` for cluster catalog metadata used by configured cluster
  boundary profiles.

Host policy overrides belong in `/etc/scratchbird/policies`. The shipped
default policy pack under `/opt/ScratchBird/resources` is create-time input and
should remain package-owned and checksummed.

## Runtime Paths

Recommended runtime paths:

```text
/var/lib/scratchbird      database files and durable node state
/var/log/scratchbird      logs when not using stderr or journald
/run/scratchbird          process control sockets and PID/state files
```

The packaged templates use relative paths such as `runtime/control` and
`data/default.sbdb`. For a system installation, either run the services with
`WorkingDirectory=/opt/ScratchBird` and override runtime/database paths on the
command line, or edit the `/etc/scratchbird` copies to use absolute paths under
`/var/lib/scratchbird`, `/var/log/scratchbird`, and `/run/scratchbird`.

## Validation

Before starting the service, validate staged package checksums from the package
root:

```sh
cd "$SB_BUNDLE"
sha256sum -c SHA256SUMS
```

Validate host configuration:

```sh
/opt/ScratchBird/bin/SBsrv --config /etc/scratchbird/SBsrv.conf --validate-config
/opt/ScratchBird/bin/SBgate --config=/etc/scratchbird/SBgate.conf --validate-config
/opt/ScratchBird/bin/SBmgr --config /etc/scratchbird/SBmgr.conf --validate-config
```

`SBParser` is validated through the listener/worker route. Direct production
execution requires `--listener-worker` and a server endpoint supplied by
`SBgate`/`SBsrv`.

## Change Control

When replacing a package under `/opt/ScratchBird`, keep the current
`/etc/scratchbird` directory and compare new template files with the deployed
copies. Resource packs should be replaced only by package updates or a signed
resource update. Local policy changes should be made through catalog/policy
administration or `/etc/scratchbird/policies`, not by editing installed default
resource files.
