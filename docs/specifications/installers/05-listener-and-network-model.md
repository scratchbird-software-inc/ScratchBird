# Listener and Network Model

Status: public specification baseline
Search key: `INSTALLER-NETWORK`

The listener is a single binary. Each running listener is configured per instance,
and that configuration — especially the **network** binding — is a primary
installer concern and a security-segmentation control.

## Listener instance tuple

A listener instance is defined by:

```
(network/interface [+ optional mask], port, parser-dialect, target server)
```

| Field | Meaning |
| --- | --- |
| network/interface | The single network/interface (address) the listener binds to and accepts connections from. |
| mask (optional) | A network mask / CIDR widening which source network(s) one listener accepts from. |
| port | The TCP port. Foreign dialects use their native default ports so existing clients connect transparently; the native SBsql listener uses its documented default (3092) unless changed. |
| parser-dialect | The one dialect this listener's parser pool speaks (supplied by the selected `parser.<dialect>` element, 02). |
| target server | The single-database server this listener fronts (01). |

There is one listener binary; the dialect is a startup parameter. The catalog (02)
therefore has a single `listener` element, and instances differ only by this tuple.

## Network binding is key

A machine may be multi-homed — internal, external, DMZ, loopback. A listener
**accepts from a single network at a time** (it binds one interface/address). To
serve two networks you choose one of:

| Approach | Effect |
| --- | --- |
| Network mask | One listener accepts from a wider source range (CIDR). Fewer instances, broader acceptance. |
| Separate listener per network | One listener instance per network — **even for the same dialect**. Stricter isolation. |

So the same dialect (for example MySQL) exposed on both the internal LAN and the
DMZ is **two listener instances**, each bound to its interface.

## Conflict key

The real conflict key is **`(interface, port)`**, not the port alone. The same port
on two different interfaces (for example `3306` on the internal address and `3306`
on the DMZ address) is a legal, distinct bind. Only the same `(interface, port)`
twice collides. The installer's probe (06) enumerates the machine's interfaces and
checks each requested `(interface, port)` against existing binds and existing
services, offering an alternate when occupied.

## Default ports

| Listener | Default port | Note |
| --- | --- | --- |
| Native SBsql | 3092 | configurable |
| MySQL dialect | 3306 | native MySQL port so unmodified MySQL clients connect |
| PostgreSQL dialect | 5432 | native PostgreSQL port |
| other dialects | the dialect's native port | so existing tools connect transparently |

## Firewall

Firewall handling is per `(network, port)`, so the installer can open the DMZ for a
MySQL listener without exposing an internal-only manager or native SBsql listener.
The safe default is closed/loopback unless the operator explicitly opts a listener
onto a routable network.

## Manager network bindings

The manager (03, 04) is also network-aware and may be dual-homed:

| Manager binding | Meaning |
| --- | --- |
| Front / listen network(s) | Where native SBsql clients connect to the manager. |
| Back / proxy-to network | The network the manager forwards into when proxying to an isolated backend. |
| Redirect vs proxy policy | Whether the manager hands the client off to the listener directly (redirect) or carries the connection (proxy). Global or per target. |

## Segmentation examples

| Goal | Configuration |
| --- | --- |
| Development | All listeners bound to loopback; firewall closed. |
| Internal-only native SBsql, MySQL on DMZ | Native SBsql listener (and manager) bound to the internal interface; a MySQL listener instance bound to the DMZ interface on 3306; firewall opens DMZ:3306 only. |
| Isolated backend behind a bastion | Backend `server`/listeners bound to the internal network; manager front on the DMZ, proxying to the internal back network (03). |

These bindings are how the network reachability of each dialect-and-database
becomes an explicit, per-listener decision rather than a global "open the port"
toggle — consistent with the engine's untrusted-outer-layer, fail-closed posture.
