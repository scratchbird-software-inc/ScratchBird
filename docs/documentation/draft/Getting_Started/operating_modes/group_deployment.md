# Managed Group Deployment

## Purpose

Managed group deployment is for organizations running more than one ScratchBird installation and needing those installations to share consistent identity conventions, policy rules, and diagnostic practices. SBmgr (the manager front-door component) provides a controlled entry point that validates identity and routes sessions to the appropriate local service before a client reaches any database.

This page is important to read carefully because managed group deployment is frequently misunderstood: it is an **operational consistency pattern**, not a distributed database. Each installation keeps its own database files, its own transaction authority, and its own recovery state. SBmgr coordinates admission — it does not merge databases or enable cross-installation queries.

**Who this is for:** operators managing multiple ScratchBird installations who need common identity validation, consistent policy admission, and shared diagnostic conventions.

**How it differs from adjacent modes:** unlike Standalone Server, managed group adds SBmgr as a front-door layer above one or more local services, and it assumes more than one installation is involved. Unlike the other modes, this shape is primarily about operational consistency, not a new technical capability of the engine.

## High-Level Shape

![diagram](./group_deployment-1.svg)

## What Is Managed

The value of a managed group comes from making these operational concerns consistent across installations, rather than configuring each one independently.

| Area | Meaning |
| --- | --- |
| Entry convention | SBmgr provides a consistent front-door pattern before traffic reaches local services. |
| Identity validation | Installations can use a shared identity source or shared identity policy where configured. |
| Admission policy | Operators can apply consistent expectations for who may connect and which routes may be used. |
| Parser route policy | Parser availability can be managed deliberately instead of being assumed. |
| Diagnostics | Logs, message vectors (structured diagnostic records), support-bundle expectations, and redaction policy can be made consistent. |
| Configuration style | Installation configuration can follow the same layout and validation rules. |

## What Remains Local

Even inside a managed group, each database retains full authority over its own durable state. Operational consistency at the SBmgr layer does not change what each engine owns.

| Area | Local Authority |
| --- | --- |
| Database files | Each database has its own storage and filespace (the physical file organization backing a database) state. |
| Transactions | Each database keeps its own MGA transaction authority (the multi-generational transaction model). |
| Recovery | Each database reopens, recovers, or refuses according to its own durable state. |
| Catalog identity | Object UUIDs and descriptors belong to the database that owns them. |
| Grants and schema roots | Authorization is evaluated for the session and database context. |
| Parser workarea | A compatibility session sees the workarea (the namespace root visible to a compatibility client) assigned by its local route. |

Managed group deployment is therefore an operating pattern, not a promise that all databases become one database.

## User Connection Flow

The sequence below shows how a session flows through SBmgr before reaching the local engine. The key insight is that the manager validates identity first; the local engine materializes authorization second.

![diagram](./group_deployment-2.svg)

The user receives a session inside a specific local database context. Grants, parser route, schema root, workarea, and policy determine what the user can see and do.

## When To Evaluate This Mode

Managed group deployment is worth evaluating when:

- several installations should use the same identity conventions;
- users or agents should connect through a manager-controlled front door;
- operators need consistent diagnostics and support-bundle expectations;
- parser routes should be centrally described or consistently admitted;
- policy defaults need to be coordinated;
- installations need a common operational runbook.

If there is only one local application process, read [Embedded Engine](embedded_engine.md). If there are several local clients on one machine, read [Single-Node IPC Server](single_node_ipc_server.md). If the requirement is listener and parser routing for clients at one installation, read [Standalone Server](standalone_server.md).

## Configuration Checklist

Before using a managed group shape, explicitly define each of these items. Gaps discovered after connecting real clients are more disruptive than verifying them beforehand.

- installation identity;
- local database routes;
- SBmgr endpoint and local service routes;
- identity source and authentication behavior;
- authorization mapping for users, roles, groups, or agents;
- parser packages admitted at each installation;
- default schema roots or workareas;
- diagnostics, support-bundle, and redaction policy;
- start, stop, drain, and restart behavior;
- refusal behavior when the identity source or local service is unavailable.

## Refusal Cases

Controlled refusal is a core part of the managed group operating model — the manager should never silently pass a request it cannot validate.

SBmgr should refuse clearly when:

- identity validation fails;
- the identity source is unavailable and policy does not allow fallback;
- the requested installation or database route is unknown;
- the local service is unavailable;
- the parser route is not admitted;
- the user is not authorized for the requested schema root or workarea;
- configuration validation fails;
- diagnostics cannot be safely produced.

## Data Movement Is Separate

Managed group deployment may coexist with backup, restore, migration, import, export, CDC, or replication features where those features are implemented and admitted. Those are separate data movement surfaces.

Do not infer data movement, shared storage, or automatic query routing from the presence of SBmgr. The manager front-door role is about connection admission and local route control.

## Diagnostics To Collect

When troubleshooting managed group issues, collect evidence at both the SBmgr layer and the local service layer to understand which step refused or failed.

- SBmgr configuration validation;
- identity source selection;
- authentication result;
- local route selected;
- parser selected;
- database opened;
- session identity and schema root;
- authorization result;
- refusal message vector when connection is denied;
- local service health;
- clean drain and shutdown behavior.

Diagnostics must follow the configured redaction policy.

## What This Mode Does Not Provide

- one shared database across installations;
- distributed query planning;
- automatic replication;
- automatic failover;
- shared storage;
- cross-installation transaction finality;
- compatibility parser completeness;
- production suitability without release-specific proof.

Those behaviors require separately documented and proven surfaces.

For hands-on configuration and startup procedure for managed group deployments, see the [Operating Modes Runbook](../../Operations_Administration/operating_modes_runbook.md).

## Where To Go Next

- [Choosing A Mode Summary](choosing_a_mode_summary.md)
- [Standalone Server](standalone_server.md)
- [Choosing A Deployment Mode](../administration/choosing_a_deployment_mode.md)
- [Configuration Basics](../administration/configuration_basics.md)
- [Identity, Authentication, And Authorization](../architecture/identity_authentication_and_authorization.md)
- [Diagnostics And Support Bundles](../administration/diagnostics_and_support_bundles.md)
