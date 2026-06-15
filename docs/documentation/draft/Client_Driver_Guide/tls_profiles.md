# TLS Profiles

## Purpose

This page describes the TLS profile that all ScratchBird drivers and adaptors use when
establishing encrypted connections. It covers what the profile name means, what the client
must present, and when plaintext transport is available.

All values on this page are verified from `project/drivers/DriverPackageManifest.csv` and
`project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md`,
`project/drivers/driver/go/S1_CONN_IMPLEMENTATION.md`.

This is a **draft**. Components are in `beta_2` / `release_candidate` status.

---

## The scratchbird_tls_1_3_floor Profile

Every driver and adaptor in the manifest declares a single TLS profile:

| Profile Name | Manifest Value |
| --- | --- |
| ScratchBird TLS 1.3 floor | `scratchbird_tls_1_3_floor` |

Source: `DriverPackageManifest.csv` column `tls_profile_set` — value `scratchbird_tls_1_3_floor`
for all 21 drivers, all 12 adaptors, and the CLI tool.

The name encodes the floor requirement: **TLS 1.3 is the minimum acceptable version**. A
connection that cannot negotiate TLS 1.3 or higher must not proceed unless the connection is
explicitly configured as plaintext (see below).

---

## What Clients Must Present

When the `scratchbird_tls_1_3_floor` profile is active, the client:

1. Initiates a TLS handshake after the TCP connection is established and before sending any
   SBWP frames.
2. Must negotiate TLS 1.3 or higher. Connections that can only negotiate TLS 1.2 or earlier
   are refused by the server.
3. Must present a valid certificate chain that the server trusts, if the server is configured
   to require mutual TLS. The exact server-side certificate policy is an operator concern; see
   [Security Guide: security_policies_and_crypto.md](../Security_Guide/security_policies_and_crypto.md).
4. After TLS is established, sends the SBWP `STARTUP` frame over the encrypted transport.

---

## Plaintext Transport (sslmode=disable)

Drivers accept `sslmode=disable` in the DSN to open a plaintext connection without TLS
wrapping. This is intended for development, local loopback, and operator-controlled
environments where TLS termination happens at the network layer.

| DSN Key | Value | Effect |
| --- | --- | --- |
| `sslmode` | `disable` | Opens plaintext transport without TLS handshake. |
| `sslmode` | `require` | Requires TLS; refuses if not available. |
| `sslmode` | `verify-ca` | Requires TLS and validates server certificate against a trusted CA. |
| `sslmode` | `verify-full` | Requires TLS, validates CA, and validates server hostname. |

Source: `project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md` — "`sslmode=disable` now
opens plaintext transport without TLS wrapping."

Source: `project/drivers/driver/go/S1_CONN_IMPLEMENTATION.md` — "`sslmode` accepts JDBC-compatible
aliases and normalizes to `disable|require|verify-ca|verify-full`".

Do not use `sslmode=disable` on untrusted networks. The engine's transport security policy may
also refuse plaintext connections depending on the operator's configuration.

---

## OpenSSL Dependency

The SBWP wire implementation uses OpenSSL for TLS. The worker source includes
`<openssl/ssl.h>` and `<openssl/err.h>`.

Source: `project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp` — OpenSSL includes at
top of file.

The exact OpenSSL version compatibility range is a build-system concern and is not documented
here; see `project/drivers/tool/cli/docs/BUILD_MATRIX.md` (not verified in this audit).

---

## Cross-References

- [connection_and_dsn.md](connection_and_dsn.md) — DSN keys, ingress modes, session opening sequence
- [wire_protocol_sbwp.md](wire_protocol_sbwp.md) — SBWP framing after TLS is established
- [Security Guide: security_policies_and_crypto.md](../Security_Guide/security_policies_and_crypto.md) — server-side cryptographic policy
- [Security Guide: platform_configuration.md](../Security_Guide/platform_configuration.md) — platform-specific TLS configuration
