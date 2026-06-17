# Deployment Topologies

Status: public specification baseline
Search key: `INSTALLER-TOPOLOGY`

The installer's "overall target group" choice (06) implies a deployment topology,
which determines which engine-tier elements are installed, how many service
instances are registered, which networks each tier is reachable on, and which
native-SBsql identity model applies (04). This document defines the topologies the
public installer offers.

## Topologies

| Topology | Tiers installed | Services registered | Networks | Native-SBsql identity (04) | Typical persona |
| --- | --- | --- | --- | --- | --- |
| Standalone single-database | one `server` + one or more `listener` instances; **no manager** | server + listener(s) | one (often loopback) | (a) self-contained | developer, embedder, evaluator, appliance |
| Managed single-host, multi-database | one `manager` (master/security DB) + N × (`server` + its `listener` instances) on one host | manager + per-database server + listeners | one or more | (b) external+hybrid, or (c) central | team server |
| Managed federated | one `manager` (often on a dedicated security host) brokering `server`/`listener` tiers on **remote** hosts | manager here; servers/listeners on other hosts | multiple, across hosts | (c) central | department / organization |
| Bastion / proxy | dual-homed `manager` on a front network proxying to backend `server`/`listener` tiers on an **isolated** network | manager (front + back); backend services internal-only | front + back networks | (b) or (c) | exposed/segmented network |

## Standalone

A single database with its server and one or more dialect listeners, and no
manager. Native SBsql clients connect to the SBsql listener directly; the database
authenticates against itself (04, option a). This is the smallest footprint and
the default for development and embedded use. Foreign-dialect listeners may still
be present here — they are always manager-independent (01, 04).

## Managed single-host, multi-database

A manager fronts native SBsql access to several databases on one machine. Each
database is its own server process with its own listener(s). The manager holds the
master/security database and is the native-SBsql authenticator and directory. This
is the typical small-server deployment.

## Managed federated

The manager and the databases live on different hosts. The manager (frequently on
a dedicated security host) is the central authority and directory for a group of
databases spread across machines; it authenticates native SBsql clients and points
them at the appropriate remote listener. Authorization and identity are shared from
the manager's master/security database (04, option c).

## Bastion / proxy

The manager is dual-homed: it listens on a front network (for example a DMZ or
external segment) and **proxies** native SBsql connections through itself into a
backend network the client cannot reach directly. The backend `server` and
`listener` tiers are bound to the internal network only, and the manager is the
sole exposed bridge. See 04 for the redirect-vs-proxy behavior and 05 for the
network binding model.

Asymmetry to note: the manager's cross-network proxy is **native SBsql only**.
Foreign-dialect clients never use the manager, so to expose a compatibility dialect
across a network boundary you must place that dialect's **listener** directly on the
reachable network (for example bind the MySQL listener to the DMZ interface). There
is no manager proxy in front of a foreign-dialect listener.

## Relationship to elements and services

- The `manager` element is present only in managed and bastion topologies. A pure
  compatibility deployment (foreign dialects only, no native SBsql) needs no
  manager — just `server` plus the dialect listeners.
- Each exposed dialect on a database is a separate `listener` instance (05), so a
  managed multi-database host registers: the manager service, plus per database one
  server service and one listener service per exposed dialect.
- Resource policy (07) must divide host RAM/CPU across the N server processes, and
  must size the manager for throughput when it proxies (data path) rather than only
  redirects (control path).
