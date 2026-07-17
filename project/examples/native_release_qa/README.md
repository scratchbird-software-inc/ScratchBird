# Native SBSQL Nightly QA Bootstrap

The native nightly contains `SBsrv`, the shared `SBgate` listener, `SBmgr`, and
the standalone `SBParser` SBSQL worker. It does not contain an emulation parser,
an emulation UDR, or a parser-specific listener executable.

The installed resource tree includes the complete, manifest-hashed initial
charset, collation, timezone, and SBSQL language pack plus the complete default
local-password policy pack. `SBsrv` discovers those packs relative to its
installed executable. The two environment variables below remain explicit
operator overrides:

- `SCRATCHBIRD_RESOURCE_SEED_PACK_ROOT`
- `SCRATCHBIRD_POLICY_SEED_PACK_ROOT`

LLVM is also mandatory for the release-complete engine:

- Debian/Ubuntu DEB installations pull `libllvm23`; RPM/AUR packages declare
  `llvm-libs >= 23`. For the portable Linux tarball, install the equivalent
  package so the system loader can resolve the recorded versioned LLVM SONAME.
- Windows ZIP/MSI payloads already include the selected LLVM DLL and its
  required non-system DLL closure beside the ScratchBird executables.
- macOS QA tarballs/PKGs do not bundle Homebrew LLVM. Run `brew install llvm`
  before testing and retain the stable `$(brew --prefix llvm)` installation.

## Prepare a private QA instance

The checked-in configurations intentionally require TLS but contain no
certificate or private key. Create a user-owned instance and local short-lived
QA certificate after installation:

Python 3 is required to run the helper. `--generate-self-signed-tls` also
requires an `openssl` command on `PATH`; this is normally provided by the
OpenSSL package on Linux, Homebrew/MacPorts or an operator toolchain on macOS,
and MSYS2, WinGet, or an operator toolchain on Windows. The portable path on all
platforms is to use an already provisioned certificate with `--tls-cert` and
`--tls-key`.

```text
export SB_SERVICE_IDENTITY="scratchbird"
export SB_SERVICE_GROUP="scratchbird"
sudo -u scratchbird python3 /opt/ScratchBird/share/scratchbird/examples/native_release_qa/prepare_native_qa_instance.py \
  --install-root /opt/ScratchBird \
  --instance-root /var/lib/scratchbird/native-qa \
  --service-identity "$SB_SERVICE_IDENTITY" \
  --service-group "$SB_SERVICE_GROUP" \
  --generate-self-signed-tls
```

For an extracted archive, point `--install-root` at its `opt/ScratchBird`
directory. On Windows use `py` or `python` and the corresponding extracted
paths. The helper finds the sibling `etc/scratchbird` directory automatically;
`--config-root` can override it.

For an MSI system installation, prepare the exact instance used by the
installer-registered service. Run this explicit activation command from an
elevated PowerShell:

```powershell
$ServiceIdentity = "NT SERVICE\scratchbird"
$ServiceGroup = "ScratchBird"
py "$env:ProgramFiles\ScratchBird\share\scratchbird\examples\native_release_qa\prepare_native_qa_instance.py" `
  --windows-system-service `
  --install-root "$env:ProgramFiles\ScratchBird" `
  --instance-root "$env:ProgramData\ScratchBird" `
  --service-identity $ServiceIdentity `
  --service-group $ServiceGroup `
  --generate-self-signed-tls
```

This mode requires the exact canonical Program Files and ProgramData roots,
the exact manual/stopped `scratchbird` SCM registration, administrator
authority, and the restricted `NT SERVICE\scratchbird` identity. Before making
QA changes it compares every live config with the materialized packaged
default. Any existing operator change is refused. The explicit activation
replaces only proven-pristine defaults, creates protected TLS/DBBT material,
and configures the stopped service's protected environment. It never creates a
database.

For a portable ZIP or a deliberately private `$HOME\scratchbird-native-qa`
instance, use the earlier private-instance form with paths inside the extracted
payload. That instance is foreground-only. `Start-Service scratchbird` never
uses a private instance because the installed service is permanently bound to
`$env:ProgramData\ScratchBird\config\SBsrv.conf`.

On Linux and macOS, run the helper as the dedicated local `scratchbird` account
that will own and execute the installation. The helper refuses UID 0 and any
other service identity or group, preventing a root-owned or operator-owned
runtime profile. The local `scratchbird` group must be the account's primary
filesystem group; that relationship grants no database-bootstrap authority.
Linux permits only that exact numeric group. macOS Open Directory may report
additional host-computed memberships for a locked local account; those results
are diagnostic and do not become an allowlist. The system package rejects every
explicit non-`scratchbird` membership and all administrator membership, sets
`InitGroups=false` for its LaunchDaemons, and separately proves that the actual
launchd process has no group beyond its primary `scratchbird` filesystem group.
The macOS account is hidden, uses `/usr/bin/false`, has a literal `*` password
lock, and has no `AuthenticationAuthority` or `ShadowHashData` record. It is a
headless process/filesystem identity, not a database principal.

On Windows, the service must first be registered with the exact service name
`scratchbird`, so Windows exposes the passwordless virtual identity
`NT SERVICE\scratchbird`. Run the helper from an elevated administrator
account; membership in the local `ScratchBird` filesystem-operations group is
not required and grants no database-bootstrap authority. Private mode grants
the exact service SID access to the private instance; system-service mode
verifies and uses the installer's protected ACLs. Both modes reject
LocalSystem, Administrator, local-SAM service users, domain service identities,
and arbitrary `NT SERVICE` identities.

For a certificate issued outside the helper, use both `--tls-cert` and
`--tls-key`. On POSIX systems the supplied key must have no group or other
permission bits. The helper never copies operator-supplied private material and
never writes key bytes to its manifest or logs.

The helper:

- refuses to overwrite a non-empty instance directory;
- creates private runtime, control, database, log, and listener directories;
- writes instance-specific copies of all four native configuration files and
  the create-time platform profile;
- wires the installed resource and policy packs into `SBsrv`;
- generates a random protected DBBT key for the current environment bridge;
- keeps TLS required and adds only local certificate/key paths;
- validates `SBsrv`, `SBgate`, and `SBmgr` before reporting success;
- verifies that the packaged `SBsec` and `SBsql` tools are present for the
  create-time bootstrap and live-login steps below.

Configuration validation does not start the server or claim a successful live
connection. The first-start and `SBsql` commands below are the tester's live
network acceptance step; preserve their output when reporting a nightly issue.

`SBParser.conf` records the independently packaged native SBSQL worker
contract. The worker is launched and configured by `SBgate`; it is not a
separate network service and does not call another parser.

`SBmgr` is packaged and its disabled-proxy configuration is validated. The QA
bootstrap does not activate the manager front door: doing so additionally
requires operator-provisioned command-scoped manager authentication material,
listener-control identity, database UUID, and an explicit nonzero backend port
distinct from the client-facing port 3092. The shipped backend value `0` is an
unset sentinel. Native network testing therefore
uses the server-owned `SBsrv -> SBgate -> SBParser` path above; manager-mediated
testing remains fail-closed until those inputs are explicitly configured.

The generated self-signed certificate is for loopback QA only, defaults to 14
days, and must not be reused for production. Use an operator-managed certificate
chain for any non-test deployment.

## Create the database and first SysArch principal

The shipped and generated configurations retain `auto_create = false`. The
database must not exist when `SBsec bootstrap` runs: this command creates the
database, its complete transaction-1 resource and policy catalog, and the first
SysArch-equivalent principal as one engine-owned create transaction. It refuses
an existing file and never writes credential authority to a sidecar.

Root/Administrator is the sole create-time OS authorization gate. On POSIX,
SBsec validates the exact local locked `scratchbird` user and primary group,
then permanently drops to that identity before creating files. On Windows, it
validates that `NT SERVICE\scratchbird` resolves as a `SidTypeUser` service SID
under the `NT SERVICE` authority before applying its ACL. The service identity
and group are ownership/execution targets only and never name or grant the
created database principal.
Atomic create-new semantics refuse an existing database. The password is read
from a no-echo prompt; never put it in an argument or environment variable.

On POSIX:

```text
export SB_INSTANCE=/var/lib/scratchbird/native-qa
sudo /opt/ScratchBird/bin/SBsec \
  bootstrap qa_admin "$SB_INSTANCE/data/default.sbdb" \
  --mode=embedded \
  --platform-profile="$SB_INSTANCE/config/SBbootstrap.profile" \
  --resource-seed-pack-root=/opt/ScratchBird/share/scratchbird/resources/seed-packs/initial-resource-pack \
  --policy-seed-pack-root=/opt/ScratchBird/share/scratchbird/resources/policy-packs/default-local-password
```

On Windows, open an elevated PowerShell. For the MSI system-service flow, the
profile must contain exactly `NT SERVICE\scratchbird` and the database target
is the canonical ProgramData instance:

```powershell
$Instance = "$env:ProgramData\ScratchBird"
Set-Location "$env:ProgramFiles\ScratchBird"
.\bin\SBsec.exe bootstrap qa_admin "$Instance\data\default.sbdb" `
  --mode=embedded `
  --platform-profile="$Instance\config\SBbootstrap.profile" `
  --resource-seed-pack-root="$PWD\share\scratchbird\resources\seed-packs\initial-resource-pack" `
  --policy-seed-pack-root="$PWD\share\scratchbird\resources\policy-packs\default-local-password"
```

Re-running either command against the created database must be refused. A
failed pre-publication creation leaves no reusable credential state; follow the
reported cleanup/quarantine diagnostic rather than retrying against a partial
file.

## First start

Supply the protected DBBT material generated by the helper and start the server
as the configured service identity, without any create-if-missing permission:

```text
export SB_INSTANCE=/var/lib/scratchbird/native-qa
export SCRATCHBIRD_LISTENER_DBBT_KEY_HEX="$(tr -d '\r\n' < "$SB_INSTANCE/secrets/listener-dbbt-key.hex")"
cd /opt/ScratchBird
sudo -u scratchbird bin/SBsrv --config "$SB_INSTANCE/config/SBsrv.conf" --foreground
```

On Windows, `Start-Service` is valid only for the explicit
`--windows-system-service` flow above, after `SBsec bootstrap` created
`$env:ProgramData\ScratchBird\data\default.sbdb`. The helper has already put
the DBBT bridge into the protected service environment without printing the
key. Start the installer-registered `scratchbird` service through the Service
Control Manager so the process runs as `NT SERVICE\scratchbird`:

```powershell
Start-Service -Name scratchbird
```

For a private portable instance, do not run `Start-Service`. Set
`SCRATCHBIRD_LISTENER_DBBT_KEY_HEX` from that private instance's protected key
file and run its `SBsrv --config ... --foreground` command instead.

The DBBT value is secret session material: do not place it on a command line,
commit it, or include it in test reports.

No default cleartext administrator password is shipped. Use the `qa_admin`
credential just created by `SBsec` and allow the tools to prompt for the
password.
From another shell, connect through the TLS-required native SBSQL listener and
trust only the generated QA certificate:

```text
/opt/ScratchBird/bin/SBsql default --mode=inet --host=127.0.0.1 --port=3092 \
  --sslmode=verify-full \
  --conn-opt "sslrootcert=$SB_INSTANCE/tls/server-cert.pem" \
  --user=qa_admin
```

An orderly foreground test stop is `Ctrl-C`. An authenticated remote stop uses
the same trust settings:

```text
/opt/ScratchBird/bin/SBadm default --mode=inet --host=127.0.0.1 --port=3092 \
  --sslmode=verify-full \
  --conn-opt "sslrootcert=$SB_INSTANCE/tls/server-cert.pem" \
  --user=qa_admin lifecycle shutdown
```

Wait for `SBsrv` and its managed `SBgate`/`SBParser` children to exit before
cleanup. A private foreground instance is user-owned and can be removed after
shutdown. The Windows system-service mode deliberately activates canonical
ProgramData configuration and a protected service environment; preserve or
remove those only through an explicit operator maintenance action, never by
deleting the entire ProgramData tree during package uninstall.
