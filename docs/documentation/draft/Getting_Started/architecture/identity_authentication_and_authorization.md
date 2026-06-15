# Identity, Authentication, And Authorization

## Purpose

After reading this page you will understand why ScratchBird treats identity, authentication, and authorization as distinct stages — and why that separation matters for both security and flexibility.

Many systems conflate these ideas: if a client can reach the server and supply a password, it is "in." ScratchBird draws sharper lines. Reaching a listener does not establish identity. Establishing identity does not guarantee authorization. Being authorized for some objects does not mean access to all objects. Each stage is a distinct check, and failure at any stage returns a controlled diagnostic rather than silently reducing access.

This page explains the high-level model. Exact providers, commands, and policy options depend on the current build and configuration.

## Core Terms

| Term | Meaning |
| --- | --- |
| Identity | The user, service, agent, or principal UUID associated with a session or operation. |
| Authentication | The process of proving the identity. |
| Authorization | The grants, roles, schema roots, object visibility, and policy rules available to that identity. |
| Session | A connected execution context with identity, parser route, transaction state, and schema context. |
| Principal | A user, role, group, service, or other authority-bearing entity. |
| Grant | Permission given to a principal or object. |
| Policy | A rule that can admit, deny, mask, restrict, or require diagnostics for an operation. |
| Sandbox | The visible namespace boundary for a session or parser route. |
| Protected material | Sensitive values that must be referenced, stored, redacted, and used through controlled mechanisms. |

## Connection Flow

![diagram](./identity_authentication_and_authorization-1.svg)

Authentication proves who the session claims to be. Authorization decides what that identity may do after it is known. Notice that the engine loads the identity's grants, schema roots (the branches of the schema tree visible to this identity), and policy context before deciding whether to open the session — authorization is materialized at session start, not looked up on each individual request.

## Identity

Identity should be durable and unambiguous. ScratchBird documentation commonly describes authority-bearing identities as UUID-backed.

An identity may represent:

- an interactive user;
- a service account;
- an administrative principal;
- a background agent;
- a group or role relationship;
- a parser or tool route acting on behalf of a user.

The identity attached to a session matters for:

- current and home schema;
- visible schema root;
- object grants;
- row-level policy;
- protected-material access;
- external access policy;
- support-bundle redaction;
- diagnostic detail.

## Authentication

Authentication establishes the identity. Depending on build and configuration, authentication may be local, shared, delegated, or tool-mediated.

The documentation should be read cautiously:

- a named provider must exist in the build;
- configuration must admit it;
- the target platform must support it;
- diagnostics must make failures clear;
- secrets must not be passed as ordinary parser text unless a documented mechanism allows it.

Authentication failure should return a controlled diagnostic and should not open a database session.

## Authorization

Authorization controls what the authenticated identity can do.

Authorization can include:

- database attach rights;
- schema visibility;
- object privileges;
- routine execution rights;
- catalog projection visibility;
- parser route admission;
- external access policy;
- protected-material use;
- row-level security;
- masks and filtered views;
- administrative operation rights.

Authorization is materialized before engine execution. A parser can present the request, but SBcore owns final admission.

## Schema Roots And Sandboxes

Authorization includes namespace scope.

![diagram](./identity_authentication_and_authorization-2.svg)

A native administrative SBsql session may see broad parts of the schema tree when authorized. A compatibility parser session normally sees its assigned workarea as the root. The client should not be able to name arbitrary objects outside that root.

Catalog projection objects can be special: they may expose selected metadata from outside a user's direct sandbox if the projection object itself has been granted that authority. That does not give the user direct access to the underlying objects.

## Grants, Roles, And Policies

Grants and roles describe permissions. Policies can add contextual rules.

| Mechanism | Typical Use |
| --- | --- |
| Grant | Allow a principal to select, insert, update, delete, execute, create, alter, or administer a specific object or object class. |
| Role | Group privileges so they can be assigned and activated consistently. |
| Schema root | Limit what namespace branch a session can see. |
| Row-level policy | Limit which rows are visible or changeable. |
| Mask | Transform protected output values. |
| External access policy | Control whether a session or routine may reach files, network routes, or other external resources. |
| Protected-material policy | Control whether a session may reference, unwrap, rotate, or use sensitive values. |

Policy should fail closed when a safe result cannot be determined.

## Parser Route And Authority

Parser routes do not grant authority by themselves.

A parser route can affect:

- language accepted;
- catalog projection shape;
- default schema behavior;
- visible workarea;
- diagnostic rendering;
- feature refusal.

The authenticated identity and engine authorization still decide whether the resulting operation is admitted.

## Message Vectors

ScratchBird uses message-vector diagnostics to represent failures and refusals. A message vector (the structured diagnostic carrier that SBcore produces when something is refused or fails) should tell you which stage denied the request — authentication, session setup, object visibility, privilege, policy, or sandbox. This matters because the remediation differs: a missing privilege requires a grant, while a sandbox denial requires checking the session's visible root.

Common identity and authorization diagnostics include: authentication failed, identity provider unavailable, session denied, parser route denied, object not visible, privilege missing, policy denied, sandbox denied, protected material unavailable, external access denied, and diagnostic redacted. The exact rendering can differ by parser or tool, but the refusal should be controlled.

## Operational Guidance

For early deployments and tests:

- use explicit identities instead of anonymous defaults;
- keep parser routes scoped to what is being tested;
- avoid broad grants in examples;
- qualify names in administrative scripts;
- verify denied cases as well as allowed cases;
- confirm support-bundle redaction before sharing diagnostics;
- keep raw secrets out of scripts and ordinary configuration files;
- document which identity source a build is using.

## What This Page Does Not Claim

This page does not claim:

- a particular authentication provider is available;
- every policy surface is complete;
- every parser renders the same diagnostics;
- all external access is allowed;
- a connected client can administer every database object;
- sandboxed users can inspect unrelated schema branches directly.

Check the current build, configuration, and Language Reference before relying on a security surface.

## Where To Go Next

- [Recursive Schema Tree](recursive_schema_tree.md)
- [Engine Parser Boundary](engine_parser_boundary.md)
- [Schemas, Objects, And Names](../using_scratchbird/schemas_objects_and_names.md)
- [Configuration Basics](../administration/configuration_basics.md)
- [Security And Privilege Statements](../../Language_Reference/syntax_reference/security_and_privilege_statements.md)
- [Policy, Mask, And RLS](../../Language_Reference/syntax_reference/policy_mask_and_rls.md)
