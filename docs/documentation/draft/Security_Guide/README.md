# Security Guide

## Purpose

This guide is the deep-dive companion to the overview material in the
Operations and Administration manual and the Language Reference. It covers the
implementation-level detail that operators, integrators, and security reviewers
need when configuring authentication providers, understanding the security
model, hardening cryptographic policy, or deploying ScratchBird in environments
that require platform-specific configuration.

This is a **draft**. No claims herein constitute a production security
certification or a promise of external audit compliance.

## Security By Design, Not Bolted On

ScratchBird was designed from the start to be operated to a high-assurance,
government-grade security posture **if the operator chooses to implement it** —
rather than requiring such controls to be retrofitted onto an existing system.
The trust-separation architecture (see
[trust_and_separation_architecture.md](trust_and_separation_architecture.md))
assumes outer layers are hostile by default: the engine does not trust the
client driver, the listener, the parser, or the manager, and it revalidates and
fail-closes instead of extending trust. The authentication, authorization,
policy, masking, protected-material, and audit mechanisms documented here exist
so an operator can raise the posture to what their environment requires without
re-architecting.

The practical consequence: the strong controls are **available from the start**
but are largely **opt-in**. A minimal deployment and a hardened one run the same
engine; they differ in how much of this security surface the operator
configures. This guide documents the mechanisms and their source-level
enforcement; it asserts **no** specific accreditation (FIPS, Common Criteria, or
equivalent), which must be validated independently against the target build.

## Structure

This guide is organized in three parts. Part 1 covers the security model,
authentication architecture, plugin families, platform configuration, and
cryptographic policy. Part 2 covers authorization objects — roles, groups,
rights, grants, domain and column security, and protected material. Part 3
covers the trust and separation architecture that underpins the whole model.
All three parts are required reading for a complete security understanding.

## Part 1 — Authentication and Cryptographic Policy

| Page | Contents |
|------|----------|
| [security_model_overview.md](security_model_overview.md) | Layered model, principal kinds, fail-closed principle, epochs |
| [authentication_and_providers.md](authentication_and_providers.md) | Provider/plugin architecture, plugin trust, challenge/credential/token flow |
| [auth_plugin_families.md](auth_plugin_families.md) | Per-family reference for all 18 plugin families declared in source |
| [platform_configuration.md](platform_configuration.md) | Linux, Windows, and BSD configuration differences (only verified) |
| [security_policies_and_crypto.md](security_policies_and_crypto.md) | Policy-pack model, cryptographic policy, token and credential hardening |

## Part 2 — Authorization Objects (Roles, Grants, Domain Security)

| Page | Contents |
|------|----------|
| [standard_roles_and_groups.md](standard_roles_and_groups.md) | Built-in roles and groups seeded by the default policy pack |
| [system_management_rights.md](system_management_rights.md) | System management privilege surfaces and right names |
| [grants_and_privileges.md](grants_and_privileges.md) | GRANT/REVOKE model, privilege inheritance, deny edges |
| [domain_and_column_security.md](domain_and_column_security.md) | Domain-level security, column-level masking and grants |
| [protected_material.md](protected_material.md) | Protected material lifecycle, key cache, legal hold, rotation |

## Part 3 — Trust and Separation Architecture

| Page | Contents |
|------|----------|
| [trust_and_separation_architecture.md](trust_and_separation_architecture.md) | The layered trust model: why a compromised driver, listener, parser, or manager still cannot reach data; SBLR revalidation; module/build separation; fail-closed controls; compromise scenarios |

## Cross-References

The pages in this guide assume familiarity with the overview material in:

- [Operations and Administration: Identity, Security, and Policy](../Operations_Administration/identity_security_and_policy.md)
- [Language Reference: Security and Sandboxing](../Language_Reference/core_paradigms/security_and_sandboxing.md)

Those pages define the operator-visible concepts (principals, grants, schema
roots, protected material, refusal classes). The pages here explain the source
structures, provider registry, platform behavior, and policy-pack format that
underpin those concepts.

## Reading Model

For a first read, follow this order:

1. `security_model_overview.md` — understand the three-layer model and
   fail-closed invariant before reading anything else.
2. `authentication_and_providers.md` — understand how a provider is declared,
   trusted, and invoked.
3. `auth_plugin_families.md` — select and review the plugin families relevant
   to your deployment.
4. `platform_configuration.md` — apply the platform-specific notes for your
   operating system.
5. `security_policies_and_crypto.md` — review and harden your cryptographic
   policy before deploying.
6. Part 2 pages — understand authorization objects once the authentication
   layer is configured.
7. `trust_and_separation_architecture.md` — understand why the whole model
   holds even when an outer layer is compromised. Readers focused on threat
   modelling or evaluating deployment isolation may read this first.
