# Authentication

## Purpose

This page describes how a ScratchBird driver supplies credentials to the server. It covers
auth method negotiation, the two admitted methods available in all current drivers
(`engine_local_password` and `scram_ready`), and the staged auth-surface probe that
allows a client to discover the server's admitted methods before opening a full session.

This page covers the **client side** of authentication — what the driver sends. The engine's
provider registry, plugin trust evaluation, and policy-pack behavior are covered in
[Security Guide: authentication_and_providers.md](../Security_Guide/authentication_and_providers.md)
and [Security Guide: auth_plugin_families.md](../Security_Guide/auth_plugin_families.md).

All values on this page are verified from `project/drivers/DriverPackageManifest.csv` and
`project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md`.

This is a **draft**. Components are in `beta_2` / `release_candidate` status.

---

## Auth Methods

The manifest column `auth_method_set` lists the auth methods a driver negotiates. All standard
drivers declare the same two-method set:

| Auth Method | Manifest Value | Description |
| --- | --- | --- |
| Engine local password | `engine_local_password` | Password authentication managed by the ScratchBird engine's local credential store. The driver sends a password credential; the engine verifies it against the local store. |
| SCRAM-ready | `scram_ready` | The connection is prepared to participate in SCRAM (Salted Challenge Response Authentication Mechanism) authentication. Drivers implement `SCRAM_SHA_256` and `SCRAM_SHA_512` challenge-response exchanges. |

Source: `DriverPackageManifest.csv` column `auth_method_set` — value `engine_local_password;scram_ready` for all 21 drivers and all 12 adaptors.

---

## Auth Method Negotiation

The sequence for auth method negotiation is:

1. The driver sends a `STARTUP` message that includes a preferred `auth_method` (or leaves it
   for the server to select).
2. The server responds with an `AuthRequest` message identifying the method it has selected
   from the driver's admitted set and its own policy.
3. The driver executes the selected method:

| Method | Driver Execution |
| --- | --- |
| `PASSWORD` | The driver sends a password credential directly. Maps to `engine_local_password`. |
| `SCRAM_SHA_256` | The driver executes a SCRAM-SHA-256 challenge-response exchange. |
| `SCRAM_SHA_512` | The driver executes a SCRAM-SHA-512 challenge-response exchange. Both SHA variants are part of `scram_ready`. |
| `TOKEN` | The driver sends a generic token payload. Used for managed/proxy bootstrap flows. |

Source: `project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md` — "direct auth execution in
`_startup_and_auth()` for: `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, generic `TOKEN`".

### Fail-Closed Methods

Drivers fail closed — rather than guessing — when the server requests a method that is admitted
in principle but not executable directly by the client:

| Method | Behavior |
| --- | --- |
| `MD5` | Driver rejects with an unsupported error. MD5 is admitted by the server but not locally executable in the driver. |
| `PEER` | Driver rejects with a broker-required error. PEER authentication requires a broker path, not direct client execution. |
| `REATTACH` | Driver rejects with a broker-required error. Generic REATTACH requires a broker path. |

Source: `project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md` — "fail-closed unsupported/
broker-required handling for admitted-but-not-local auth methods".

The Go driver confirms the same pattern: "admitted but unsupported or broker-required methods
(`MD5`, `PEER`, `REATTACH`) now fail closed with `0A000` instead of guessing through generic
payload replay."

Source: `project/drivers/driver/go/S1_CONN_IMPLEMENTATION.md`.

---

## Staged Auth Surface Probe

Drivers expose a pre-connect probe that discovers the server's admitted auth methods without
opening a full session:

- `probe_auth_surface(...)` — performs pre-connect auth negotiation and returns the set of
  admitted methods, plugin IDs, and whether any method requires a broker.
- `get_resolved_auth_context()` — returns the resolved auth context after a real connect.

This probe is available in both direct-listener and manager-proxy modes:

| Mode | Probe Behavior |
| --- | --- |
| `direct_listener` | Connects to the listener, exchanges STARTUP, receives auth challenge, and exits before completing auth. |
| `manager_proxy` | Probes the manager's front-door for admitted methods and managed token bootstrap availability. |

Source: `project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md` — "staged auth/bootstrap
surfaces … `probe_auth_surface(...)`, `get_resolved_auth_context()`".

The CLI tools (`sb_isql`, `sb_admin`, `sb_security`) expose the same probe via
`--probe-auth-surface` and `--show-auth-context` command-line flags.
See [cli_tools.md](cli_tools.md).

---

## Manager Proxy Token

When using `manager_proxy` ingress mode, the driver must supply a `manager_auth_token` before
any socket connection is attempted. The driver fails fast (before opening a socket) if this
token is absent.

Source: `project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md` — "fail-fast validation in
`_connect()` so `front_door_mode=manager_proxy` without `manager_auth_token` errors before
any socket connect attempt".

---

## DSN Auth Fields

The following DSN / connection config fields control auth method selection and credential supply:

| Key | Purpose |
| --- | --- |
| `auth_method` | Request a specific auth method by name |
| `auth_token` | Generic token-auth payload |
| `auth_method_id` | Explicit auth method identifier |
| `auth_payload_json` | Auth payload in JSON form |
| `auth_payload_b64` | Auth payload in base64 form |
| `auth_provider_profile` | Auth provider profile selector |

Source: `project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md` — "auth startup config fields
on `ConnectionConfig`".

---

## MGA Reauth

SBWP includes a `Reauth` message type (`0x22`). Sessions may be re-authenticated after
initial connection within the same transport. This is a server-initiated flow; the client
responds to a new `AuthRequest`. Drivers that implement the `kFeatureReauth` feature bit
(feature bit 22) support this path.

Source: `project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp` — `kReauth = 0x22`,
`kFeatureReauth = FeatureBit(22)`.

---

## Cross-References

- [connection_and_dsn.md](connection_and_dsn.md) — DSN keys, ingress modes, session opening
- [Security Guide: authentication_and_providers.md](../Security_Guide/authentication_and_providers.md) — engine-side provider registry and trust model
- [Security Guide: auth_plugin_families.md](../Security_Guide/auth_plugin_families.md) — all 18 plugin families and their capabilities
- [Security Guide: security_policies_and_crypto.md](../Security_Guide/security_policies_and_crypto.md) — policy-pack model and cryptographic hardening
