# Platform Configuration

## Purpose

This page documents the authentication and security configuration differences
that are actually verified in the ScratchBird source tree for Linux, Windows,
and BSD targets. It explicitly distinguishes verified differences from
platform behavior that could not be confirmed from the available source.

The guiding principle: only document verified differences. If the source does
not differentiate a configuration option or plugin by OS, that option is
platform-neutral and is documented as such. Do not configure for differences
that are not present in the source.

## Platform Guard Verification

A search for `#ifdef _WIN32`, `#ifdef __linux__`, BSD macros, and POSIX guards
across `src/engine/internal_api/security/` yielded the following:

**Confirmed platform-specific code:**

- `protected_material_api.cpp` contains a `#if defined(_WIN32)` guard around
  the atomic file replace implementation:
  - On Windows: uses `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`
  - On non-Windows (Linux, BSD, macOS): uses `std::filesystem::rename`
  - This is an implementation detail of the protected material catalog's atomic
    write path. It is not operator-visible.

- `protected_material_api.cpp` includes `<windows.h>` under `#ifdef _WIN32` for
  the `MoveFileExW` call.

**No other platform guards were found** in the security API headers or
implementation files. The live adapter, authentication API, provider model,
plugin trust API, and crypto policy contain no `#ifdef _WIN32`, `#ifdef __linux__`,
or BSD-specific guards.

This means the security API surface itself is platform-neutral. Platform
differences arise from the runtime dependencies that specific plugin families
require, not from the engine's security API code.

## Configuration File Overview

Configuration for ScratchBird's security behavior lives in:

| File | Purpose |
|------|---------|
| `config/templates/SBsrv.conf` | Server process configuration; includes `[server.security]` section |
| `config/templates/SBgate.conf` | Listener/gateway configuration; includes TLS and auth options |
| `config/templates/SBmgr.conf` | Manager configuration; includes `management_auth_required` |

These files are cross-platform INI/flat-key format. Their keys are the same on
all supported operating systems. What differs by platform is whether the runtime
dependency for a given provider family is installed and reachable.

### SBsrv.conf Security Section

```ini
[server.security]
provider_family = local_password
provider_state = healthy
default_policy_installed = true
```

`provider_family` specifies the authentication provider family for the server
instance. The default is `local_password`. This key is platform-neutral.

`provider_state` is `healthy` in the template. `default_policy_installed = true`
indicates the default policy pack has been seeded.

### SBgate.conf TLS Setting

```ini
tls_required = true
```

TLS is required for the native listener by default. This setting is
platform-neutral. The TLS implementation uses the `tls_x509` dependency
(required by `certificate_mtls`). The underlying TLS library must be present on
the target platform.

### SBmgr.conf Management Auth

```ini
manager.management_auth_required = true
```

Management authentication is required by default. This is platform-neutral.

## Provider Families by Platform Orientation

The following table classifies each provider family by its platform orientation
based on its dependency and the behavior of the `peer`/`ident` family (which
uses POSIX socket credentials):

| Family | Platform orientation | Notes |
|--------|---------------------|-------|
| `local_password` | Platform-neutral | No external dependency |
| `scram_sha256` | Platform-neutral | No external dependency |
| `pam` | POSIX-oriented (Linux, BSD) | `pam` dependency; PAM is a POSIX standard. Windows lacks a native PAM subsystem. The `pam` dependency is expected to be absent on Windows without a compatibility layer. |
| `peer` / `ident` | POSIX-oriented (Linux, BSD) | `peer` uses OS socket credentials (`SO_PEERCRED` or equivalent). This mechanism is POSIX-specific. The source does not show a Windows implementation path for this family. |
| `certificate_mtls` | Platform-neutral | `tls_x509` dependency. TLS libraries are available on all supported platforms. |
| `ldap_ad` | Platform-neutral | `ldap_client` dependency. LDAP client libraries are available on Linux, BSD, and Windows. |
| `kerberos_pac` | Platform-neutral | `gssapi_krb5` dependency. Kerberos GSSAPI is available on Linux, BSD, and Windows (though library names differ). |
| `oidc_jwt` | Platform-neutral | `oidc_jwt_client` dependency. HTTP-based; platform-neutral. |
| `saml` | Platform-neutral | `saml_xmlsig` dependency. Platform-neutral. |
| `webauthn` | Platform-neutral | `webauthn_fido2` dependency. Platform-neutral. |
| `radius` | Platform-neutral | `radius_client` dependency. Platform-neutral. |
| `proxy_assertion` | Platform-neutral | `proxy_assertion_verifier` dependency. Platform-neutral. |
| `token_api_key` | Platform-neutral | No external dependency. |
| `bearer_token` | Platform-neutral | No external dependency. |
| `workload_identity` | Platform-neutral | `spiffe_svid_or_workload_oidc` dependency. Platform-neutral. |
| `managed_identity` | Platform-neutral | `spiffe_svid_or_workload_oidc` dependency. Platform-neutral. |

**Important caveat:** The source does not contain explicit `#ifdef _WIN32` or
`#ifdef __linux__` guards for any of the provider families. The
POSIX-orientation of `pam` and `peer`/`ident` is inferred from the nature of
their dependencies (the POSIX PAM API and POSIX socket credentials), not from
confirmed platform guards in the security code. Operators should verify against
the build system and their target platform before relying on these families.

## Linux-Specific Notes

### PAM Configuration

On Linux, the PAM provider requires:

1. The `pam` shared library to be present on the build.
2. A PAM service file in `/etc/pam.d/` whose name matches the `service` field
   in the provider payload.
3. The PAM conversation must complete with a `hidden` or `secret` prompt type.
   Any other prompt type is refused by the engine at the live adapter level.

The engine enforces `pam_policy_enabled` must be set in the provider policy
before PAM is active.

### Peer / Ident

On Linux, the `peer` family uses `SO_PEERCRED` to obtain the connecting
process's effective UID. The engine requires a successful credential exchange;
a stale or unavailable credential is rejected.

The `ident` family is available on Linux but is generally not recommended for
production deployments because RFC 1413 ident servers are not commonly
deployed and the protocol offers no cryptographic assurance.

### Protected Material File Path

On Linux, the protected material catalog atomic replacement uses
`std::filesystem::rename`. This is a single syscall on most Linux filesystems
and provides atomic replacement semantics within the same filesystem. The
`rename` call may not be atomic across filesystem boundaries. Place the
protected material catalog and its temporary write path on the same filesystem.

### Entropy / Random

The source does not contain explicit entropy source configuration for Linux.
The engine uses the C++ standard library and OpenSSL for cryptographic
operations, both of which use platform entropy sources (typically `/dev/urandom`
or `getrandom()` on Linux). This is not operator-configurable via the
SBsrv.conf security section.

## Windows-Specific Notes

### Protected Material Atomic Replacement

The source confirms one Windows-specific behavior: the atomic file replacement
in `protected_material_api.cpp` uses `MoveFileExW` with
`MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`. This is consistent with
atomic replacement semantics on Windows NTFS. The operator implication is the
same as on Linux: the temporary and target paths must be on the same volume.

### PAM and Peer/Ident

The `pam` and `peer`/`ident` families have POSIX-oriented dependencies. The
source does not contain a Windows implementation for either of these families.
Operators deploying on Windows should not configure these families. If they are
needed, verify independently that a compatible PAM subsystem or socket
credential mechanism is available on the target build.

### Kerberos on Windows

The `kerberos_pac` family uses the `gssapi_krb5` dependency. On Windows, MIT
Kerberos or another GSSAPI provider may be used. The source does not confirm
any SSPI (Security Support Provider Interface) integration. Do not assume that
native Windows SSPI Kerberos is supported unless verified against the build for
your target version.

### LDAP on Windows

The `ldap_ad` family uses the `ldap_client` dependency. On Windows, this is
expected to be a third-party LDAP client library rather than the Windows
LDAP API (`wldap32.dll`). The source does not confirm any wldap32 integration.
Verify the expected LDAP client library against your build.

## BSD-Specific Notes

### PAM on BSD

FreeBSD and OpenBSD both have native PAM implementations. The `pam` family is
expected to work on BSD with a PAM service file in the appropriate location
(typically `/etc/pam.d/` on FreeBSD). The engine's requirements (hidden prompt,
account phase, session phase) are the same on BSD as on Linux.

### Peer / Ident on BSD

The `peer` family's `SO_PEERCRED` equivalent on BSD is `LOCAL_PEERCRED`
(FreeBSD) or a similar mechanism. The source does not confirm platform-specific
handling for BSD socket credentials; the behavior at the live adapter level is
the same (the fixture path is used, and real deployment verification is
required).

### Protected Material File Path on BSD

On BSD, the `std::filesystem::rename` path is used (same as Linux). The same
filesystem-boundary constraint applies.

## Unverified Platform Behaviors

The following behaviors could not be confirmed from the source tree and should
be verified against the build before relying on them:

- Whether `peer` authentication via BSD `LOCAL_PEERCRED` is implemented in the
  real SBgate socket binding code (the source read here covers the engine
  internal API only).
- Whether the Kerberos GSSAPI integration uses MIT krb5 on all platforms or
  whether it adapts to the platform's native Kerberos library.
- Whether `ident` authentication is available as a production option on any
  platform (the probe confirms the fixture path works, not that a real RFC 1413
  client is implemented).
- Whether any platform requires additional TLS configuration (CA bundle path,
  certificate store location) beyond `tls_required = true` in `SBgate.conf`.
- Whether Windows Security Support Provider Interface (SSPI) is integrated for
  any provider family. The source shows no SSPI headers or guards.

Operators should treat these items as requiring independent verification against
the build for their target platform and ScratchBird version.

## Related Pages

- [auth_plugin_families.md](auth_plugin_families.md) — per-family dependency and requirements
- [authentication_and_providers.md](authentication_and_providers.md) — provider architecture
- [security_policies_and_crypto.md](security_policies_and_crypto.md) — cryptographic policy
