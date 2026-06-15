# Standalone Server

## Purpose

Standalone server mode is the full client/server shape where clients connect over a network through SBgate (the listener and router) and a parser package that speaks the client's protocol. This is the mode to evaluate when a client needs a network-facing entry point, protocol negotiation, a compatibility parser, or a test of end-to-end client/server behavior.

The defining boundary is that clients do not reach SBcore (the core database engine) directly. They connect to SBgate, which routes them to a parser package (a component that accepts one client language or protocol family), and that parser lowers admitted work to an internal representation before it reaches the engine.

**Who this is for:** operators validating network-facing deployments, developers testing compatibility parsers, and anyone whose client tool must connect through a listener rather than a local IPC channel.

**How it differs from adjacent modes:** unlike Single-Node IPC Server, standalone server adds a network listener (SBgate) and requires a parser package for every client connection. Unlike Managed Group Deployment, it handles one installation rather than coordinating identity and policy across several.

## High-Level Shape

![diagram](./standalone_server-1.svg)

## What This Mode Is For

Standalone server mode is the right page to read when you are evaluating:

- network-facing client access;
- listener startup and shutdown;
- parser selection and parser pool behavior;
- compatibility client or tool experiments where a parser exists;
- native SBsql over a listener route;
- protocol negotiation and refusal behavior;
- end-to-end client/server smoke tests.

Actual suitability depends on the current release, target platform, parser status, configuration, and proof results.

## Component Responsibilities

Understanding the role of each component makes it easier to diagnose problems at the right layer.

| Component | Responsibility In This Mode |
| --- | --- |
| Client | Connects through the configured listener route and sends language or protocol requests. |
| SBgate | Accepts client connections, performs listener-level routing, and hands work to the selected parser path. |
| Parser package | Accepts one client language or protocol family, binds visible names, lowers admitted work to SBLR (the internal request representation passed to the engine), and renders client-shaped results or diagnostics. |
| SBsrv | Provides the local service route to SBcore where configured. |
| SBcore | Owns durable catalog identity, descriptors, transactions, storage, recovery, authorization, and engine diagnostics. |
| Configuration | Defines listener endpoints, parser registration, identity sources, database routes, resource files, policy, and diagnostics. |

## Request Flow

The following sequence shows what happens from the moment a client connects to the moment it receives a result. Notice that SBcore is reached only after the connection has been authenticated and the parser has lowered the request.

![diagram](./standalone_server-2.svg)

## Parser Routing

The listener selects a configured parser path — it does not make syntax into engine authority on its own. The parser accepts or refuses the client surface, then submits a bound request to the engine path.

Parser routing should be explicit enough that you can answer these questions for any connected session:

- which parser handled the connection;
- which database or workarea (the namespace root visible to a compatibility client) the session entered;
- which identity was authenticated;
- which schema root the session sees;
- which unsupported or denied requests are refused by the parser;
- which requests reach engine authority.

## Compatibility Parser Boundaries

A compatibility parser is scoped to its own client family. Being explicit about what a parser must not do prevents accidental bypass of engine authority.

It should not:

- accept unrelated dialects silently;
- bypass engine transactions;
- write storage directly;
- grant access outside its configured workarea;
- treat physical page-copy data as logical restore input;
- perform low-level repair or verification through a compatibility route;
- claim unsupported features by returning success without doing the work.

It should:

- accept the supported client surface;
- lower supported work to SBLR;
- apply parser-specific defaults explicitly;
- return controlled diagnostics for unsupported, denied, unsafe, or unavailable behavior;
- keep catalog projections within the configured authority model.

## First Standalone Server Smoke Test

With more components involved than in embedded or IPC mode, a first standalone test needs to verify each layer in turn before declaring success.

1. Required binaries, parser packages, and resource files are staged together.
2. Configuration validates before accepting clients.
3. SBsrv can open the database route.
4. SBgate starts and listens on the intended endpoint.
5. The selected parser package is available and registered.
6. A client can connect and authenticate.
7. The parser opens the expected schema root or workarea.
8. A create, insert, select, and commit cycle succeeds.
9. A controlled invalid request returns the expected diagnostic.
10. The client disconnects cleanly.
11. Listener drain or stop behavior completes.
12. The database reopens with committed data visible.

## Diagnostics To Collect

Standalone server mode has more moving parts than embedded or IPC mode. When something goes wrong, capturing diagnostics at each layer points to the right fix faster.

- configuration validation result;
- listener endpoint and route selection;
- parser registration and version;
- authentication result;
- session identity and schema root;
- database open result;
- transaction state;
- message vectors for unsupported or denied requests;
- parser-to-engine request identifiers where available;
- clean shutdown, drain, and restart evidence.

Diagnostics should be redacted before sharing outside trusted support channels.

## Security And Exposure

Network-facing entry points require explicit configuration — do not rely on defaults to provide security. Before allowing access beyond a local test environment, verify:

- only intended endpoints are listening;
- authentication is configured;
- authorization and schema roots are explicit;
- parser routes are limited to the needed surfaces;
- diagnostics do not expose protected material;
- server-local file access is denied unless an explicit documented policy admits a safe operation;
- unsupported management or low-level actions refuse clearly.

This guide describes the concepts to verify, not a certified deployment shape.

## What This Mode Does Not Provide

- implementation of every command in every parser package;
- compatibility with every external client tool;
- shared identity conventions across separate installations;
- cross-installation query planning;
- automatic data movement;
- physical backup or repair through parser routes;
- production readiness without release-specific proof.

## When To Choose Another Mode

If several installations need consistent manager-mediated entry, shared identity, or coordinated policy admission, read [Managed Group Deployment](group_deployment.md).

For hands-on listener startup, parser registration, and configuration procedure, see the [Operating Modes Runbook](../../Operations_Administration/operating_modes_runbook.md).

## Where To Go Next

- [Choosing A Mode Summary](choosing_a_mode_summary.md)
- [Single-Node IPC Server](single_node_ipc_server.md)
- [Managed Group Deployment](group_deployment.md)
- [Reference-System Compatibility](../using_scratchbird/reference_system_compatibility.md)
- [Engine Parser Boundary](../architecture/engine_parser_boundary.md)
- [Configuration Basics](../administration/configuration_basics.md)
- [Diagnostics And Support Bundles](../administration/diagnostics_and_support_bundles.md)
