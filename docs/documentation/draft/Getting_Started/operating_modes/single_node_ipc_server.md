# Single-Node IPC Server

## Purpose

Single-node IPC server mode solves a specific problem: you have several local clients that all need to share one database, but you do not want or need a network-facing listener. SBsrv (the local server process) runs on the same machine and accepts connections from local clients through an IPC endpoint — a local communication channel that stays on-machine.

This is the mode to evaluate when moving up from embedded mode because you need a process boundary between clients and the engine, or because multiple local programs need to share database access concurrently.

**Who this is for:** developers with several local tools or services sharing one database, operators who want service-style lifecycle management without network exposure.

**How it differs from adjacent modes:** unlike Embedded Engine, the engine runs in a separate process rather than inside the client — client crashes do not terminate the server. Unlike Standalone Server, there is no network listener; clients must be on the same machine.

## High-Level Shape

![diagram](./single_node_ipc_server-1.svg)

## What This Mode Is For

Single-node IPC server mode is the right starting point when you are evaluating:

- local multi-client access to a shared engine;
- a process boundary between client applications and SBcore;
- local automation that should not embed SBcore directly;
- service-style lifecycle management on one machine;
- smoke tests that need attach, detach, and restart behavior;
- a setup where network listener behavior is not part of the requirement.

This mode is not a remote access mode. If a client needs to connect through a network-facing listener or parser route, read [Standalone Server](standalone_server.md).

## Component Responsibilities

Each component has a defined role. Understanding those roles helps you know where to look when something goes wrong.

| Component | Responsibility In This Mode |
| --- | --- |
| Local client | Connects to the configured IPC endpoint and sends requests through the local route. |
| IPC endpoint | Provides the local communication boundary between clients and SBsrv. |
| SBsrv | Owns the local service process, opens engine sessions, routes admitted local requests, and returns results or diagnostics. |
| SBcore | Owns catalog identity, descriptors, transactions, storage, recovery, authorization, and engine diagnostics. |
| Configuration | Defines database paths, resource locations, authentication, authorization, IPC endpoints, diagnostics, and policy. |

## Request Flow

The following sequence shows a typical attach-work-detach cycle so you can reason about which component handles each step.

![diagram](./single_node_ipc_server-2.svg)

## Parser Behavior

Single-node IPC mode may expose different local request surfaces depending on build and configuration. A local client might use native SBsql (ScratchBird's native command language), a direct local API, or another configured parser route.

The same rules still apply regardless of which surface is used:

- parser packages accept and lower client syntax to internal representations;
- SBcore owns durable authority;
- unsupported or denied behavior should return a controlled diagnostic;
- parser availability must be proven by the current build and tests.

## First Local IPC Smoke Test

A first IPC test proves that the server lifecycle and client attach/detach cycle work end to end before you build anything more complex on top.

1. SBsrv starts with the intended configuration.
2. Required resource files are available.
3. A disposable database can be created or opened.
4. A local client can attach through the IPC endpoint.
5. Authentication and authorization produce the expected session.
6. A simple create, insert, select, and commit cycle succeeds.
7. A second local client can attach if the scenario requires it.
8. A controlled invalid request returns a message vector.
9. Clients detach cleanly.
10. SBsrv stops cleanly and can restart.
11. The database reopens with committed data visible.

## Local Isolation

The server process provides isolation that embedded mode does not have — but that isolation is local process isolation, not network security or a replacement for authentication and authorization.

- Client crashes do not automatically terminate the server process.
- The engine lifecycle can be supervised separately from clients.
- Multiple clients can use a shared local service boundary.
- Diagnostics can be collected centrally by the server process.

## Configuration Checklist

Before using this mode, verify these items are explicitly set rather than assumed:

- IPC endpoint location and permissions;
- database path and storage permissions;
- resource file locations;
- authentication provider or local identity configuration;
- grants, schema roots, and policy;
- diagnostic output location;
- service start and stop behavior;
- stale endpoint cleanup behavior after abnormal termination;
- parser or local API route configuration.

## Diagnostics To Expect

Knowing what normal failure messages look like helps you distinguish configuration problems from software defects. Useful diagnostics in this mode include:

- configuration validation errors;
- IPC endpoint unavailable or permission denied;
- database open refused;
- authentication failure;
- authorization denied;
- parser route missing or unavailable;
- transaction state diagnostic;
- controlled shutdown or drain messages;
- stale endpoint or stale process state.

## What This Mode Does Not Provide

- network listener access;
- parser pool management for network clients;
- remote client compatibility;
- shared identity conventions across installations;
- distributed query behavior;
- automatic backup or repair behavior;
- proof that every parser package is available.

## When To Choose Another Mode

If you need clients to connect over a network, or if you are testing a compatibility parser that requires listener and parser routing, read [Standalone Server](standalone_server.md) instead.

If several installations need to share identity conventions and consistent policy admission, read [Managed Group Deployment](group_deployment.md).

For hands-on startup and configuration procedure, see the [Operating Modes Runbook](../../Operations_Administration/operating_modes_runbook.md).

## Where To Go Next

- [Choosing A Mode Summary](choosing_a_mode_summary.md)
- [Standalone Server](standalone_server.md)
- [First Database](../using_scratchbird/first_database.md)
- [Configuration Basics](../administration/configuration_basics.md)
- [Identity, Authentication, And Authorization](../architecture/identity_authentication_and_authorization.md)
- [Diagnostics And Support Bundles](../administration/diagnostics_and_support_bundles.md)
