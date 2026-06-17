# Architecture and Runtime Tiers

Status: public specification baseline
Search key: `INSTALLER-ARCH`

An installer places and configures a set of runtime tiers. This document defines
those tiers and how they connect, because the installer's choices (which
elements, which services, which networks, which auth model) only make sense
against this architecture.

## The tiers

| Tier | Role | Cardinality |
| --- | --- | --- |
| Driver / client | Application-side connector (native SBsql driver, or a third-party driver for a foreign dialect). | many |
| Manager | **Native-SBsql-only** front door: early authenticator, holder of the master/security database, directory of databases/servers, and either a redirector or a cross-network proxy. | 0..1 per managed group |
| Listener | A single binary that, at startup, is bound to **one network, one port, and one parser dialect**. It is the pool manager in front of a pool of that one dialect's parsers. | many (one or more per server) |
| Parser | A dialect-specific unit inside a listener's pool: carries the dialect **wire protocol**, **translation layer** (dialect → SBLR), **caching**, and performs the dialect-native **authentication handshake** when no manager token is present. Talks to the server over IPC. | pooled, warm |
| Server | The engine process that manages **exactly one database**. Owns MGA visibility and transaction finality. | one per database |

Key invariant: **one server process manages one database.** Multiple databases on
a host therefore mean multiple server processes, each fronted by its own
listener(s) and parser pool(s).

The listener is **the** parser pool. There is a single listener binary; the
dialect it manages is a startup parameter. A listener on the default MySQL port
manages a pool of MySQL parsers; a listener on the native port manages SBsql
parsers; and a single server (one database) may be fronted by several listeners
at once — one per exposed dialect — so existing third-party clients connect over
their native wire protocols transparently. Wire-protocol compatibility lives at
this listener/parser tier.

## Connection flows

There are two distinct paths, separated by who they serve.

### Native SBsql (may be managed)

```
SBsql client ─► manager (early authN against master/security DB)
                 │  user selects target database
                 ▼
            token + (redirect | proxy) to the database's SBsql listener
                 ▼
            SBsql listener ─► warm SBsql parser ─IPC─► server ─► MGA
```

The manager authenticates first and hands the listener a token; the listener
trusts the token (fast path). The manager may **redirect** the client to connect
to the listener itself, or **proxy** the connection through itself into an
isolated backend network (see 03, bastion topology).

### Foreign dialect (always direct, manager-independent)

```
MySQL/PostgreSQL/… client ─► dialect listener on its native port
                              │  (no manager token)
                              ▼
                         dialect-native auth handshake
                              ▼
                         warm dialect parser ─IPC─► server ─► MGA
```

Foreign-dialect clients **never use the manager**. They connect directly to the
dialect listener on its native port; with no manager token, the listener/parser
performs that dialect's native authentication handshake. The manager's
central-authority and token model therefore governs **native SBsql access only**.

## IPC and the engine boundary

Parsers communicate with the server over IPC; the parser holds a preliminary IPC
channel to the correct server/database, kept warm in the pool so connections do
not pay a cold-start cost. The server owns the engine: SBLR execution, MGA
visibility, and transaction finality. Parsers, listeners, and the manager are
outer layers; the engine remains the sole authority for row visibility and
mutation, and the outer layers are treated as untrusted and fail closed.

## Network segmentation (introduced here, detailed in 05)

Because a listener binds to one network, and a manager may listen on one network
while proxying to another, the network on which each tier is reachable is an
explicit installer decision. This is the basis for placing the manager and native
SBsql on an internal network while exposing selected compatibility dialects on a
DMZ, or keeping everything on loopback for development. See
[05 — Listener and Network Model](05-listener-and-network-model.md) and
[03 — Deployment Topologies](03-deployment-topologies.md).
