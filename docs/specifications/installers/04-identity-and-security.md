# Identity and Security Topology

Status: public specification baseline
Search key: `INSTALLER-IDENTITY`

When an installation creates a first database, it must choose how identities are
authenticated and authorized. This is a **native-SBsql** decision — it governs how
native SBsql clients authenticate (through the manager or against the database
itself). Foreign-dialect clients are always authenticated at their own listener and
are outside this model (see "Foreign-dialect authentication" below).

The choice seeds the initial security-provider chain (it is materialized from
catalog policy and is fail-closed). It is **reconfigurable later** — not a one-way
door — and the installer collects it only when a first database is created;
embedded/development installs that defer database creation default to option (a).

## Native-SBsql identity options

Modeled as points on an authentication-source × authorization-source matrix.

| Option | AuthN source | AuthZ source | Initial `sysarch` | Where users/rights live |
| --- | --- | --- | --- | --- |
| (a) Self-contained | local password provider | local | the creator | inside the database |
| (b) External authN + hybrid authZ | LDAP / Kerberos / SSO / OIDC | mixed: directory-group→role map, otherwise local | mapped from a directory group (e.g. an administrators group → `sysarch`) | mappings external, rights local |
| (c) Central authN + authZ | the manager's master/security database (local or remote, dedicated) | the same security database | provisioned in the security database | in the shared security database, group-wide |

### (a) Self-contained

Local authentication and authorization; the creator is added with the `sysarch`
role; all users and rights live inside the database. No external dependency, fully
portable. This is the default-local-password posture and the default for
standalone, embedded, evaluation, and appliance installs. The installer collects
the initial `sysarch` credentials.

### (b) External authN + hybrid authorization

Authentication is delegated to a directory or single-sign-on system
(LDAP / Kerberos / SSO / OIDC). Authorization is **hybrid**: the installer collects
a directory-group→role map (for example, members of a chosen administrators group
become `sysarch`), and all other authorization is kept locally. The installer
collects the directory connection (URL / realm / issuer) and the group→role
mapping. Choosing this option auto-selects the matching authentication-provider
element from the catalog (02).

### (c) Central authN + authorization

Both authentication and authorization live in a dedicated security database — the
manager's master/security database — shared by a group of databases, local or
remote. The bootstrap `sysarch` is provisioned there. The installer collects the
location/connection of the security database and links the new database to it.
Choosing this option implies a manager (03) and auto-selects the matching provider
element.

## How the manager fits

In options (b) and (c) the manager is the native-SBsql early authenticator: it
authenticates the client against the configured source, then either redirects the
client to the target database's listener or proxies the connection through itself
into an isolated backend (03, bastion). The manager holds the master/security
database that, in option (c), is the central authority for a group. Option (a) is
the manager-less case.

| Manager connection behavior | When used |
| --- | --- |
| Redirect | The client can reach the target listener's network; the manager hands a token and the client connects to the listener directly. |
| Proxy | The backend is on an isolated network; the manager forwards the connection through itself. Puts the manager in the data path (size accordingly — 07). |

The behavior may be set globally or per target. Network bindings for the manager
(front/listen network and back/proxy-to network) are covered in 05.

## Foreign-dialect authentication

Foreign-dialect connections (MySQL, PostgreSQL, and other compatibility dialects)
**never use the manager**. Each such client connects directly to its dialect
listener, which performs that dialect's **native authentication handshake** and
maps the result to an SB identity. This is true regardless of whether a manager
exists for the native-SBsql side. Consequently:

- The identity options above scope to native SBsql only. The installer presents
  the choice as "how do native SBsql clients authenticate."
- Each exposed foreign dialect carries its own listener-handled authentication.
- To expose a foreign dialect across a network boundary, bind its listener to the
  reachable network directly (05); there is no manager in that path.

## Enforcement posture

The installer only **seeds** the security-provider chain; enforcement is engine-
owned, materialized from catalog policy, and fail-closed. The outer layers
(parsers, listeners, manager, drivers) are untrusted with respect to data
authority. Changing the identity topology after install is a policy reconfiguration,
not a reinstall.
