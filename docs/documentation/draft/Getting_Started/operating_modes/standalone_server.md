# Standalone Server

## Purpose

Standalone server mode adds listener and parser routing for clients that connect over a protocol-facing entry point. This is the mode to read when you are thinking about donor-style clients, network clients, parser pools, or ordinary client/server use.

## High-Level Shape

```mermaid
flowchart LR
    Client[Client tool or application] --> Listener[SBgate]
    Listener --> Parser[Parser package]
    Parser --> IPC[Local server route]
    IPC --> Server[SBsrv]
    Server --> Engine[SBcore]
    Engine --> DB[(Database files)]
```

## Parser Role

The listener does not make donor syntax into engine authority. A parser package accepts a specific protocol or language surface, validates it according to its profile, and lowers admitted work to a ScratchBird execution request.

The engine then rechecks object identity, descriptors, transaction context, and security before execution.

## Typical Request Flow

```mermaid
sequenceDiagram
    participant Client
    participant Gate as SBgate
    participant Parser as Parser package
    participant Server as SBsrv
    participant Engine as SBcore

    Client->>Gate: connect
    Gate->>Parser: route to configured parser
    Parser->>Server: attach/open session
    Server->>Engine: authenticate and bind session
    Client->>Gate: statement or protocol request
    Gate->>Parser: parse and lower
    Parser->>Server: SBLR/request envelope
    Server->>Engine: execute admitted request
    Engine-->>Server: result or message vector
    Server-->>Parser: result
    Parser-->>Gate: protocol-specific rendering
    Gate-->>Client: response
```

## What It Is For

Standalone server mode is intended for:

- client/server evaluation;
- parser compatibility testing;
- donor-style client experiments where a parser exists;
- networked development deployments;
- applications that need a network-facing service boundary.

Suitability for any particular production environment must be verified against current release status and platform proof.

## What It Does Not Mean

The existence of a parser package does not mean every donor command, datatype, catalog projection, management utility, or driver behavior is complete. Each parser has its own supported surface and tests.

## Related Pages

- [../architecture/engine_parser_boundary.md](../architecture/engine_parser_boundary.md)
- [../using_scratchbird/donor_database_compatibility.md](../using_scratchbird/donor_database_compatibility.md)
