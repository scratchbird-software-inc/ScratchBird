# Configuration Reference

## Purpose

This chapter defines the operational configuration areas an administrator must understand before starting ScratchBird components or admitting users.

## Initial Coverage

- operating mode selection;
- database routes;
- parser routes;
- identity provider configuration;
- authorization and schema-root policy;
- resource file locations;
- storage paths;
- diagnostics and support-bundle settings;
- resource limits and backpressure;
- startup validation;
- refusal behavior when configuration is incomplete or unsafe.

## Configuration Principles

- Prefer explicit configuration for security-sensitive behavior.
- Keep raw secrets out of ordinary configuration files.
- Treat parser routes as separately admitted capabilities.
- Validate configuration before accepting client work.
- Preserve diagnostics while applying redaction policy.

## Related Pages

- [Identity, Security, And Policy](identity_security_and_policy.md)
- [Parser Registration And Routes](parser_registration_and_routes.md)
- [Getting Started: Configuration Basics](../Getting_Started/administration/configuration_basics.md)
