# Identity, Security, And Policy

## Purpose

This chapter defines the operator-facing model for identity, authentication, authorization, schema roots, parser workareas, protected material, policy, and redaction.

## Initial Coverage

- identity sources;
- users, services, agents, roles, and groups;
- authentication flow;
- authorization materialization;
- schema roots and sandboxing;
- parser-visible workareas;
- grants and roles;
- policy admission;
- row-level security and masking where implemented;
- protected-material references;
- support-bundle redaction;
- refusal behavior for denied access.

## Operator Rules

- Authentication proves identity.
- Authorization admits work.
- Parser routes do not grant authority by themselves.
- Raw secrets should not appear in scripts, parser packets, ordinary configuration, or diagnostics.
- Denied access should return a controlled message vector.

## Related Pages

- [Configuration Reference](configuration_reference.md)
- [Diagnostics, Message Vectors, And Support Bundles](diagnostics_message_vectors_and_support_bundles.md)
- [Getting Started: Identity, Authentication, And Authorization](../Getting_Started/architecture/identity_authentication_and_authorization.md)
