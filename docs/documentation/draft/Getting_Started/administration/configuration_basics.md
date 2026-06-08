# Configuration Basics

## Purpose

ScratchBird configuration controls how components start, what databases they open, what parser routes are available, and how sessions are admitted. Exact file names and options can vary by build and packaging, so this page explains the concepts.

## Configuration Areas

| Area | Examples |
| --- | --- |
| Database location | Where database files are created or opened. |
| Operating mode | Embedded, IPC server, standalone server, or managed group shape. |
| Parser routes | Which parser packages are available for which entry points. |
| Authentication | Which identity source and method are admitted. |
| Authorization | Grants, schema roots, and policy. |
| Resources | Memory, file, timeout, and backpressure policy. |
| Diagnostics | Log level, support-bundle options, redaction policy. |

## Configuration Flow

```mermaid
flowchart LR
    Files[Config files/templates] --> Validate[Validate configuration]
    Validate --> Start[Start component]
    Start --> Open[Open database/session]
    Open --> Policy[Apply policy]
    Policy --> Run[Serve requests]
```

## Principles

- Prefer explicit configuration over relying on defaults for anything security-sensitive.
- Treat parser routes as separately configured capabilities.
- Keep secrets out of ordinary configuration files unless the documented secret mechanism requires otherwise.
- Validate configuration before opening the service to users.
- Preserve diagnostics needed for support, but redact protected material.

## Related Pages

- [diagnostics_and_support_bundles.md](diagnostics_and_support_bundles.md)
- [../architecture/identity_authentication_and_authorization.md](../architecture/identity_authentication_and_authorization.md)
