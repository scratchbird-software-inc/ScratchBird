# ScratchBird Manager Enterprise Runbook

SBMN_MANAGER_ENTERPRISE_RUNBOOK

Authority: public_release_evidence_only.

This runbook describes operator checks for the single-node manager. It does not
define storage, transaction, parser, security, or cluster authority. The engine
executes SBLR and internal procedures only. SQL text is converted outside the
engine, and MGA remains the authority for transaction visibility, rollback,
recovery, sweep, archive, and shutdown outcomes.

## Install Shape

The public Linux installation root is `/opt/ScratchBird/`. The manager binary is
installed with the server, listener, SBsql parser worker, SBsql parser UDR,
tools, drivers, adapters, resources, and default policy packs. The source-tree
configuration template is `project/config/templates/SBmgr.conf`; installer
packaging copies it into the selected etc directory and rewrites paths for the
target host.

The manager must run under a dedicated service account. Runtime and control
directories must be private to that account. The control socket, lifecycle
journal, owner token, PID file, audit log, metrics file, and support bundle
staging directory are manager-owned runtime artifacts and are recreated or
validated on startup.

## Release Profile

The first release profile is `enterprise`. In this profile:

- literal MCP secrets are rejected;
- local temporary token stores are rejected unless a developer profile is
  selected explicitly;
- listener handoff requires materialized DBBT keyring evidence;
- direct-native bypass is fenced unless a native-only developer profile is
  selected explicitly;
- command authorization is checked by command-specific manager rights;
- support bundle generation requires the support export right.

Developer, test, native-only, and diagnostic profiles are separate explicit
profiles. They are not aliases for enterprise operation.

## Listener Handoff

Manager-to-listener handoff uses DBBT and LPREFACE. The DBBT token binds the
database UUID, listener id, issued-at time, expiry, session nonce, and key id.
LPREFACE carries the manager-issued handoff to the listener. The listener must
reject wrong listener id, wrong database UUID, expired token, replay, malformed
DBBT, malformed LPREFACE, and malformed acknowledgement.

Listener management commands are structured envelopes. The manager does not
grant raw command-text authority to the listener. Each command carries identity,
request id, nonce, timestamp, listener id, database UUID, operation, arguments,
and the command-specific right.

## Operator Checks

Before enabling a manager profile:

1. Run `sbmn_manager --validate-config --config <path>` and require zero
   diagnostics.
2. Confirm runtime and control directories are owned by the manager account and
   are not group or world writable.
3. Confirm DBBT keyrings and protected secret references are private regular
   files when listener control is enabled.
4. Confirm TLS material is present when proxy TLS is enabled.
5. Confirm the listener control directory is configured when enterprise
   listener/parser routing is required.
6. Confirm support bundle export is granted only to support-authorized
   principals.
7. Confirm audit and metrics paths are writable by the manager account and
   readable only by intended operators.

## Health And Support

The manager publishes structured status, metrics, audit, and support bundle
material. Support bundle output redacts passwords, secrets, tokens, credentials,
verifiers, private keys, and local path details. Support output is for
diagnostics only; it is not transaction, recovery, parser, engine, or security
authority.

Operational health uses:

- lifecycle state and lifecycle journal;
- heartbeat state for the managed server endpoint;
- restart backoff and quarantine state;
- proxy connection counters and high-water values;
- audit event coverage for management decisions;
- support bundle manifest and redaction report.

## Failure Handling

Startup refuses unsafe release profile settings, malformed configuration,
unsafe keyring permissions, overlong control socket paths, duplicate owners,
stale owner ambiguity, and direct-native enterprise bypass. Restart decisions
use bounded backoff and quarantine rules. Commit, rollback, crash recovery, and
visibility decisions remain MGA-owned engine behavior.
