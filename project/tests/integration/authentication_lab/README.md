# ScratchBird Authentication Provider Test Lab

This directory contains disposable external services for authentication-provider
integration testing. It is test infrastructure, not a production deployment
example. All credentials, keys, certificates, realms, users, and shared secrets
are public test fixtures.

The complete testing and evidence requirements are documented in
[`../../../docs/testing/AUTHENTICATION_PROVIDER_TEST_LAB.md`](../../../docs/testing/AUTHENTICATION_PROVIDER_TEST_LAB.md).

## Quick start

From this directory:

```bash
./lab.sh validate
./lab.sh up core
./lab.sh smoke core
./lab.sh down
```

Profiles build on the following service sets:

| Profile | Services |
| --- | --- |
| `core` | Keycloak, OpenLDAP, FreeRADIUS, nginx mTLS endpoint, signed proxy-assertion fixture, Toxiproxy |
| `enterprise` | `core` plus Samba AD DC and Linux PAM/peer-credential fixture |
| `browser` | `core` plus Selenium Chromium for virtual WebAuthn authenticators |
| `workload` | SPIRE server and agent with a registered ScratchBird workload identity |
| `full` | Every service |

`up` generates a disposable CA and certificate set under `generated/`. `down`
removes all Compose volumes. The ignored `logs/` directory preserves explicit
smoke and service logs.

## CTest enrollment

The lab is excluded from normal builds. Enroll it explicitly:

```bash
cmake -S project -B build/auth-lab \
  -DSB_BUILD_AUTHENTICATION_LAB_TESTS=ON \
  -DSB_AUTHENTICATION_LAB_PROFILE=core
ctest --test-dir build/auth-lab --output-on-failure -L authentication_lab
```

CTest uses a setup/smoke/cleanup fixture and returns the standard skip code 77
when Docker or Docker Compose is unavailable.

## Fault injection

The OIDC, LDAP, and mTLS services have TCP paths through Toxiproxy:

```bash
./lab.sh fault oidc latency 750
./lab.sh fault oidc cut
./lab.sh fault oidc restore
```

The direct ports remain available so a test can compare healthy and faulted
provider behavior in the same run.
