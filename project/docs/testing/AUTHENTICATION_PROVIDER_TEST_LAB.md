# Authentication Provider Test Lab and Full-Test Requirements

## Purpose and status boundary

ScratchBird maintains two complementary authentication test layers:

1. Engine contract probes exercise authorization, policy, freshness, group
   materialization, diagnostic, and fail-closed behavior using opaque trusted
   provider results. These probes are deterministic and do not perform network
   authentication.
2. The container lab supplies disposable real protocol peers for adapter and
   listener integration testing.

The presence of a lab service does **not** mean the corresponding ScratchBird
provider adapter is implemented, connected, or validated. A provider may be
reported as end-to-end validated only after the listener or adapter exchanges
the real protocol with the lab, creates a trusted provider result internally,
and passes the positive, negative, freshness, authorization, and redaction
requirements below.

The lab lives at
[`project/tests/integration/authentication_lab`](../../tests/integration/authentication_lab).
It is excluded from ordinary builds and release gates unless explicitly enabled.

## Host requirements

The core lab requires:

- Linux, macOS, or Windows with a Linux-container runtime;
- Docker Engine 24 or newer;
- Docker Compose v2.20 or newer;
- at least 4 CPU threads, 6 GiB available memory, and 8 GiB free disk space;
- Bash 5, Python 3.11, OpenSSL 3, and curl;
- outbound registry access on the first run.

The full profile should have 8 CPU threads, 12 GiB memory, and 15 GiB free disk
space. Samba AD and SPIRE are supported most predictably on a Linux Docker host.
Rootless Docker may require additional UID/GID and cgroup configuration for
SPIRE. Samba provisioning requires the scoped `SYS_ADMIN` container capability
to write Windows ACL metadata on `SYSVOL`; the lab does not use unrestricted
privileged mode. No fixture may be connected to a production network or supplied
with production credentials.

## Pinned service inventory

The defaults are pinned in `compose.yaml`; `.env` may override them only for a
reviewed upgrade.

| Service | Default image or source | Purpose |
| --- | --- | --- |
| Keycloak | `quay.io/keycloak/keycloak:26.7.3` | OIDC discovery, JWKS, signed JWTs, SAML metadata/assertions, groups, disabled accounts, TOTP/WebAuthn enrollment |
| OpenLDAP | `bitnamilegacy/openldap:2.6.10-debian-12-r4` | Generic LDAP bind/search, nested groups, LDAP TLS fixtures |
| FreeRADIUS | `freeradius/freeradius-server:3.2.10-alpine` | Access-Accept, Access-Reject, attributes, shared-secret failure and timeout testing |
| mTLS endpoint | `nginx:1.29.8-alpine` | Client-certificate chain, EKU, revocation, missing-certificate and untrusted-CA tests |
| Proxy assertion fixture | local image from `python:3.13.7-alpine` | Signed, expired, wrong-audience, bad-signature and replayed assertions |
| Toxiproxy | `ghcr.io/shopify/toxiproxy:2.12.0` | Provider outage and latency injection for OIDC, LDAP and mTLS |
| Samba AD DC | local image from `debian:13.1-slim` | AD-compatible LDAP, Kerberos, PAC/group and disabled-account fixtures |
| Unix authentication fixture | local image from `debian:13.1-slim` | PAM and real Unix `SO_PEERCRED` behavior |
| Selenium Chromium | `selenium/standalone-chromium:4.48.0-20260905` | Browser automation and virtual WebAuthn authenticator support |
| SPIRE | `ghcr.io/spiffe/spire-server:1.11.2` and `spire-agent:1.11.2` | SPIFFE trust bundle, workload registration and SVID issuance |

The OpenLDAP image is a frozen legacy Bitnami artifact and is acceptable only
inside this isolated disposable lab. It must not be reused as a deployment
recommendation. The Samba image is built locally from distribution packages
because Samba does not provide a single preferred AD-DC test image.

## Fixture identities

| Identity | Password | Expected state and groups |
| --- | --- | --- |
| `alice` | `alice-password` | Enabled; `database-users`, nested `analytics` |
| `admin-alice` | `admin-alice-password` | Enabled; `database-users`, `database-admins` |
| `mfa-alice` | `mfa-alice-password` | Enabled; TOTP enrollment required in Keycloak |
| `disabled-alice` | `disabled-alice-password` | Disabled or deliberately rejected |

Samba applies Windows password-policy-compatible variants documented by its
entrypoint (`Alice-Password1!`, `Admin-Alice-Password1!`). The realm is
`SCRATCHBIRD.TEST`, the NetBIOS domain is `SCRATCHBIRD`, and the domain
controller is `dc1.scratchbird.test`.

All credentials are intentionally public and must never be accepted outside the
isolated lab.

## Running the lab directly

```bash
cd project/tests/integration/authentication_lab

./lab.sh validate
./lab.sh up core
./lab.sh smoke core
./lab.sh logs core
./lab.sh down
```

Use `enterprise`, `browser`, `workload`, or `full` instead of `core` for the
larger profiles. Copy `.env.example` to `.env` only when local ports conflict.

The core profile exposes services only on loopback. Provider-to-provider traffic
uses a dedicated Compose bridge; the bridge is not suitable for production
traffic or credentials. `generated/` contains disposable private keys
and is ignored by Git. `down` removes the lab volumes; deleting `generated/`
forces certificate rotation on the next start.

## Running through CTest

```bash
cmake -S project -B build/auth-lab \
  -DCMAKE_BUILD_TYPE=Release \
  -DSB_BUILD_AUTHENTICATION_LAB_TESTS=ON \
  -DSB_AUTHENTICATION_LAB_PROFILE=core

ctest --test-dir build/auth-lab \
  --output-on-failure \
  -L authentication_lab
```

The setup and cleanup tests use a CTest fixture. Cleanup runs after a failed
smoke test, and all lab tests share a resource lock so concurrent CTest workers
cannot mutate the same Compose project.

## Provider coverage and closure requirements

| Provider family | Lab source | Required evidence before “fully tested” |
| --- | --- | --- |
| `local_password` | Existing engine credential fixtures | Correct and incorrect verifier, locked/disabled identity, secret redaction, rate/challenge policy |
| `password_compat` | Existing engine fixtures | Explicit policy enablement, channel binding, downgrade refusal |
| `scram`, `scram_sha256`, `scram_sha512` | Listener plus engine fixtures | Full exchange, nonce uniqueness, stored verifier, channel binding, bad proof, downgrade and replay refusal |
| `internal_server_authority` | Native server process | Unforgeable server authority, absent/stale authority refusal |
| `remote_security_database` | Two ScratchBird instances | Real remote connection, TLS binding, source outage, stale epoch and group materialization |
| `cluster_security` | External cluster provider | Closed-provider integration evidence; unavailable in the standalone public lab |
| `peer`, `ident` | Unix-auth container and native listener | Kernel-derived peer UID/GID/PID; caller text flags must never establish trust; Windows requires a separate named-pipe/token lane |
| `certificate_mtls` | nginx mTLS fixture and generated CA | Valid chain, missing cert, revoked cert, wrong EKU/SAN, untrusted CA, fingerprint and channel binding |
| `ldap_ad` | OpenLDAP and Samba AD | Simple/SASL bind as supported, StartTLS/LDAPS, base/filter escaping, nested groups, disabled identity, bad password, bad CA and outage |
| `kerberos_pac` | Samba AD | Service principal and keytab, correct SPN, expired ticket, wrong realm/SPN, PAC validation and effective groups |
| `pam` | Unix-auth container plus native Linux lane | Authentication and account phases, denied/expired account, conversation cancellation, module/service failure and redaction |
| `oidc_jwt`, `oauth_validator` | Keycloak | Discovery, JWKS rotation, allowed algorithms, issuer/audience/nonce/time checks, bad signature, `alg=none`, overage groups and outage |
| `saml` | Keycloak | Metadata trust, XML signature, destination/audience, time window, replay, wrapping/malformed XML and oversized attributes |
| `webauthn`, `factor_chain` | Keycloak plus Selenium virtual authenticator | Registration/assertion ceremonies, RP ID/origin/challenge, user presence/verification, counter/replay behavior and factor policy |
| `radius` | FreeRADIUS | Accept/reject, bad shared secret/authenticator, required attributes, UDP loss/timeout and group mapping |
| `proxy_assertion` | Signed assertion fixture | Allowed source, signature, audience, expiry, unique ID replay cache, channel binding and secret redaction |
| `bearer_token`, `token_api_key` | Engine security catalog | Valid/revoked/expired tokens, digest comparison, scope/groups, rotation and no plaintext persistence |
| `security_database_temporary_token` | Engine security catalog | Valid/wrong/expired/principal-mismatch token and durable revocation |
| `token_refresh_reauth` | Engine plus OIDC lab | Bound refresh proof, forced reauthentication, replay, expiry and policy disablement |
| `workload_identity`, `managed_identity` | SPIRE; cloud CI for managed identity | SVID/bundle validation, trust domain and selector, rotation/expiry; Azure/AWS/GCP metadata behavior requires vendor CI/emulators |
| `custom_cpp_plugin` | Signed test plugin | Manifest/signature/ABI admission, isolated invocation, malformed result, timeout/crash and unload/reload behavior |
| `trust_reject` | Engine contract probe | Permanent fail-closed behavior |

## MFA-specific requirements

The container lab covers software-emulated MFA but cannot replace every physical
or platform authenticator test.

- TOTP: automate Keycloak enrollment for `mfa-alice`, record the test seed only
  in generated artifacts, prove current/adjacent-window behavior, replay refusal,
  bad code and recovery flow.
- WebAuthn: use Selenium's Chromium session and the WebDriver virtual
  authenticator API to vary user presence, user verification, resident keys,
  signature counters and transports.
- Hardware-backed closure: run a separate controlled lane with at least one
  FIDO2 roaming key and one platform authenticator. Record model, firmware,
  transport, browser and OS; never store device secrets in the repository.
- Push/SMS/email OTP: use a vendor sandbox or a purpose-built message sink. A
  successful HTTP mock alone does not prove delivery-provider integration.

## Windows-domain closure

Samba AD proves most LDAP, Kerberos, SPN, PAC and nested-group behavior expected
from an AD-compatible domain. It does not prove Microsoft-specific SSPI, LSASS,
Windows access-token, domain-join, named-pipe impersonation, or Windows Hello
behavior.

Full Windows-domain evidence therefore additionally requires a disposable
Windows Server 2022 or 2025 domain-controller VM and a domain-joined Windows
runner. That lane must test:

- SSPI negotiation and channel binding;
- service-account SPNs and key rollover;
- PAC and nested/universal/domain-local groups;
- disabled, locked, expired and password-change-required accounts;
- clock skew, DNS failure, DC failover and trust failure;
- named-pipe client token/impersonation behavior where peer authentication is
  exposed on Windows.

Windows Server licensing and CI credentials must remain outside this repository.

## Fault, rotation, and negative testing

Toxiproxy exposes faultable OIDC, LDAP and mTLS paths. For example:

```bash
./lab.sh fault oidc latency 750
./lab.sh fault ldap cut
./lab.sh fault ldap restore
```

Every network provider needs evidence for connection refusal, timeout, partial
response, stale cached metadata, trust-anchor rotation and recovery. RADIUS UDP
loss should be injected using a network namespace/firewall lane because
Toxiproxy handles TCP streams. Keycloak JWKS and SAML certificate rotation must
prove both overlap and stale-key rejection. Regenerating `generated/pki` rotates
the local certificate authority.

## Evidence checklist

A provider is fully tested only when the run records:

- exact ScratchBird commit and build configuration;
- exact container image tags and resolved image digests;
- host OS/runtime versions and selected lab profile;
- positive authentication and expected principal;
- invalid credential, disabled identity and unavailable-provider results;
- signature/trust, audience, channel-binding, expiry and replay failures where
  applicable;
- external group materialization and group-limit behavior;
- stable public diagnostics without credential/token/key leakage;
- audit and metric records for success, failure, denial and outage;
- production-binary proof that test-only trusted-result hooks are absent;
- paths to complete CTest output and container logs.

Service smoke success alone proves only that the lab is healthy. End-to-end
closure requires ScratchBird to consume the service and produce all listed
evidence.

## Updating the lab

When changing an image version:

1. use a release-specific tag, never `latest`;
2. review upstream release and security notes;
3. run `./lab.sh validate`, then every affected profile's smoke test;
4. record the resolved digest from `docker image inspect` in the test evidence;
5. update this inventory and any fixture syntax affected by the upgrade;
6. verify that no generated certificate, token, password override, or container
   volume was added to Git.
