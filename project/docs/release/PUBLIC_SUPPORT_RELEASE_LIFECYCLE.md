# Public Support And Release Lifecycle

PUBLIC_SUPPORT_RELEASE_LIFECYCLE

Authority: public_release_evidence_only.

This policy defines the first public Core Beta support boundary. It is release
evidence only and does not define engine behavior, storage authority,
transaction finality, security authorization, optimizer decisions, cluster
production authority, or agent runtime authority.

## Supported Platforms

The supported platforms for the first public Core Beta release are bounded by
`docs/build_requirements/README.md`.

| Platform | Support status | Release proof boundary |
| --- | --- | --- |
| Linux x86_64, Ubuntu 24.04 LTS | supported first proof lane | Full configure, build, CTest, install, hardening, crash/fault, packaging, and public export evidence are required. |
| Windows x64, Windows 11 or Windows Server 2022/2025 | target platform pending native CI/runtime proof | Configure, build, CTest, platform abstraction, install, permission, entropy, filesystem, and selected crash/restart proof are required before support is claimed. |
| FreeBSD x86_64, FreeBSD 14.x | target platform pending native runner proof | Configure, build, CTest, platform abstraction, entropy, filesystem, locking, and selected crash/restart proof are required before support is claimed. |
| macOS | out of scope for first public release | No support claim is made and release gates must not imply macOS support. |

Unsupported platforms must fail closed through documented diagnostics or remain
outside the public support matrix.

## File-Format And Upgrade Promises

The file-format and upgrade promises for the first public Core Beta release are:

- Public file-format metadata is versioned by public release version metadata.
- Supported upgrade paths must pass `public_upgrade_migration_gate`.
- Downgrade requests must be refused with stable diagnostics.
- Unknown newer formats must fail closed instead of being opened as current.
- Policy-pack schema compatibility and catalog materialization must remain
  create-time and durable-catalog based after database creation.
- No support promise is made for private, unreleased, or lawyer-review-pending
  package content.

## Security Patch Window

The security patch window for Core Beta starts on the public release date and
continues until the next public minor release supersedes it or until an
announced end-of-support date is published. During that window, supported
platforms receive fixes for reproducible security defects that affect public
Core Beta code and are validated by public release gates. Security disclosure
process details are tracked separately by the public SECURITY policy gate.

## Preview Status

Core Beta is a preview release for public review, integration testing, and
operator validation. Preview status means:

- public C API and C++ wrapper compatibility are frozen for the Core Beta ABI
  version, subject to the compatibility policy;
- cluster-positive production behavior is not a public support claim without an
  external provider proof;
- parser and driver previews are advisory until server authorization and engine
  execution gates accept the request;
- unsupported features are documented and must fail closed;
- release evidence cannot replace engine-owned MGA transaction inventory,
  authorization policy state, or recovery classification.

## Unsupported Features

Unsupported features are governed by the public unsupported-feature matrix.
Unsupported, external-provider-required, compile-time-disabled, and
policy-blocked features must publish deterministic fail-closed diagnostics and
must not advertise runtime execution authority.

## First-Release Support Boundaries

First-release support includes public build verification, install and service
hardening evidence, admin runbooks, upgrade diagnostics, support-bundle
triage, and reproducible public release artifacts for the supported platform
matrix.

First-release support excludes:

- private cluster implementation claims;
- production support for platforms outside the support matrix;
- unsupported security providers or external provider families;
- private package, unreleased package, or local machine path dependencies;
- legal/IP closure artifacts that are still waiting for lawyer approval.
