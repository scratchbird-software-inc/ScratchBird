# Choosing A Mode Summary

## Purpose

Before you configure anything, you need to know which shape fits your application. ScratchBird can run as a library inside your process, as a local service that several programs share, or as a network-accessible server — and those shapes involve meaningfully different components, lifecycle responsibilities, and security considerations.

This page orients you to the four operating modes and helps you pick the right one to read first. It is not a sizing guide, benchmark, support statement, or deployment recommendation. The right mode depends on the current build output, target platform, configuration, parser packages, resource files, tests, and the application boundary you intend to use.

## The Four Shapes

| Mode | Short Description | Main Entry | Network Listener Required | Read More |
| --- | --- | --- | --- | --- |
| Embedded engine | The application links to SBcore (the core database engine) and uses the engine in its own process. | SBcore library/API | No | [Embedded Engine](embedded_engine.md) |
| Single-node IPC server | Local clients connect to SBsrv (the local server process) through a shared IPC endpoint. | SBsrv IPC endpoint | No | [Single-Node IPC Server](single_node_ipc_server.md) |
| Standalone server | Clients connect through SBgate (the listener and router) and a parser package that handles their protocol. | SBgate and parser packages | Yes | [Standalone Server](standalone_server.md) |
| Managed group deployment | Multiple installations use SBmgr (the manager front-door) for consistent identity and policy conventions. | SBmgr plus local services | Depends on local service shape | [Managed Group Deployment](group_deployment.md) |

## Decision Flow

The following flowchart starts from the boundary your application needs and works outward. Most new deployments land at embedded or single-node IPC; the more complex shapes add real value only when their specific capabilities are required.

![diagram](./choosing_a_mode_summary-1.svg)

## Quick Recommendations

| Need | First Mode To Evaluate |
| --- | --- |
| One application owns all access and can carry engine lifecycle responsibility. | Embedded engine. |
| Several local processes need a shared database service without accepting network traffic. | Single-node IPC server. |
| A client connects over a listener or requires parser routing for a client protocol. | Standalone server. |
| Several installations need consistent identity validation, policy conventions, and manager-mediated entry. | Managed group deployment. |
| You are only trying to learn SBsql syntax. | Use the simplest mode available in your build, then read [First SBsql Session](../using_scratchbird/first_sbsql_session.md). |
| You are testing a compatibility parser. | Standalone server, because parser routing and protocol-facing behavior are part of the test. |

## Comparison Table

The table below summarizes the key differences across modes to help you spot where your requirements fit.

| Area | Embedded Engine | Single-Node IPC Server | Standalone Server | Managed Group Deployment |
| --- | --- | --- | --- | --- |
| Process boundary | Same process as application. | Separate local server process. | Listener and server processes. | Manager-front-door convention over local services. |
| Client location | Application-local. | Same machine. | Network-facing client boundary where configured. | Operator-defined local installations. |
| Primary component | SBcore. | SBsrv. | SBgate, parser package, SBsrv, SBcore. | SBmgr plus configured local services. |
| Parser route | Optional, depending on application surface. | Depends on local client route. | Required for protocol or SQL client traffic. | Depends on the local service selected by SBmgr. |
| Lifecycle owner | Application. | Local service supervisor or operator. | Service supervisor or operator. | Operator-managed entry and local services. |
| Best first proof | Open database, run transaction, close cleanly. | Start server, attach local clients, run transaction, detach. | Connect through listener, route parser, run transaction, disconnect. | Authenticate through manager, open local route, run scoped session. |
| Main risk to understand | Application crash and engine lifecycle are tied together. | Local IPC configuration and service lifecycle. | Listener, parser, authentication, and routing configuration. | Shared identity and policy expectations across installations. |

## What All Modes Share

Regardless of which mode you pick, the same engine authority model applies underneath. The mode changes how a client reaches the engine; it does not move durable object identity, final transaction authority, recovery decisions, or materialized authorization out of SBcore.

![diagram](./choosing_a_mode_summary-2.svg)

## What To Verify Before Choosing

Before settling on a mode for anything beyond exploration, confirm these points — discovering a gap after you have started configuring is more disruptive than checking first.

- The required binaries or libraries exist in the build output.
- Required parser packages are staged and registered.
- Resource files are present.
- Configuration files are explicit and valid.
- Authentication and authorization behavior are understood.
- Diagnostics can be collected and redacted.
- Start, stop, attach, detach, and restart tests pass for the target platform.
- The selected mode has proof coverage for the workflow you need.

## Conservative Mode Selection

Use the smallest mode that satisfies the application boundary. Adding unnecessary layers increases configuration surface, failure modes, and security exposure without providing benefit.

- Do not add a listener if the application only needs embedded access.
- Do not expose network-facing routes when local IPC is enough.
- Do not use a compatibility parser for native SBsql work unless that parser route is the thing being tested.
- Do not treat a managed group deployment as shared storage or distributed query behavior.
- Do not infer availability from a diagram; check the current build output and tests.

## Where To Go Next

- [Embedded Engine](embedded_engine.md)
- [Single-Node IPC Server](single_node_ipc_server.md)
- [Standalone Server](standalone_server.md)
- [Managed Group Deployment](group_deployment.md)
- [First Database](../using_scratchbird/first_database.md)
- [Configuration Basics](../administration/configuration_basics.md)
