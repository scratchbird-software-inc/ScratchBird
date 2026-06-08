# Choosing A Deployment Mode

## Purpose

This page gives an administrator's view of the operating modes. It is not a performance guide or production readiness statement.

## Questions To Ask

| Question | If Yes | If No |
| --- | --- | --- |
| Does one application own the process? | Consider embedded mode. | Consider server modes. |
| Do clients all run on the same machine? | Consider single-node IPC server. | Consider standalone server. |
| Do you need donor-style network clients? | Consider standalone server with the required parser. | Use native SBsql or local APIs where possible. |
| Do several installations need shared identity conventions? | Consider managed group deployment. | Use local identity configuration. |
| Do you need strict isolation from application crashes? | Prefer a server process over embedded mode. | Embedded mode may be acceptable for controlled use. |

## Deployment Comparison

| Mode | Process Boundary | Client Scope | Operational Notes |
| --- | --- | --- | --- |
| Embedded | Same process as application. | Application-local. | Smallest shape; application owns lifecycle. |
| Single-node IPC | Separate local server process. | Local clients. | No listener required for ordinary local IPC use. |
| Standalone server | Listener and parser route to local service. | Network or donor clients. | Parser packages and listener configuration matter. |
| Managed group | Manager-front-door convention over local services. | Operator-defined. | Useful when identity and entry behavior should be coordinated. |

## Conservative Reading

Use the most restrictive mode that satisfies the application need. Add listener, parser, and manager layers only when their behavior is required and verified for the build.

## Related Pages

- [../operating_modes/choosing_a_mode_summary.md](../operating_modes/choosing_a_mode_summary.md)
- [configuration_basics.md](configuration_basics.md)
