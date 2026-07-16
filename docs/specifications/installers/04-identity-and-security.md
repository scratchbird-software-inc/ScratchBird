# Identity and Security Topology

Status: public specification baseline
Search key: `INSTALLER-IDENTITY`

Platform installation and database security bootstrap are separate operations.
An installer places binaries, resources, policy templates, configuration
defaults, and locked service identities. It never creates or opens a database,
collects a database password, creates a principal or role, maps an OS identity to
a database identity, provisions a security database, or activates a service.

The first database and first `sysarch` principal are created only by a later,
explicit `SBsec bootstrap --mode=embedded` operation. Root/Administrator is the
sole create-time OS authorization gate. After that gate, the locked
`scratchbird` service identity is used only for directory/file ownership,
privilege drop or ACL handoff, and process execution. It never names or grants a
database principal, authentication right, role, or security authority. The
engine-owned MGA create transaction materializes the selected policy and first
principal atomically; credentials are accepted by `SBsec`, never by an installer.

## Native-SBsql identity options

These are post-install database-policy choices, modeled as points on an
authentication-source × authorization-source matrix. Installer component
selection may place the provider implementation needed by a future choice, but
it does not select, configure, or activate that choice for a database.

| Option | AuthN source | AuthZ source | Initial `sysarch` | Where users/rights live |
| --- | --- | --- | --- | --- |
| (a) Self-contained | local password provider | local | the creator | inside the database |
| (b) External authN + hybrid authZ | LDAP / Kerberos / SSO / OIDC | mixed: directory-group→role map, otherwise local | mapped from a directory group (e.g. an administrators group → `sysarch`) | mappings external, rights local |
| (c) Central authN + authZ | the manager's master/security database (local or remote, dedicated) | the same security database | provisioned in the security database | in the shared security database, group-wide |

### (a) Self-contained

Local authentication and authorization; the creator is added with the `sysarch`
role; all users and rights live inside the database. No external dependency, fully
portable. This is the explicit first-database bootstrap posture. `SBsec` prompts
for the requested initial principal credential through protected input and the
engine records it inside the database transaction; no credential sidecar exists.

### (b) External authN + hybrid authorization

Authentication is delegated to a directory or single-sign-on system
(LDAP / Kerberos / SSO / OIDC). Authorization is **hybrid**: an authenticated
database administrator later configures a directory-group→role map, and all
other authorization is kept locally. Provider connection material and mappings
are database/security configuration, not installer inputs. The matching provider
element must already be installed from the catalog (02) before activation.

### (c) Central authN + authorization

Both authentication and authorization live in a dedicated security database — the
manager's master/security database — shared by a group of databases, local or
remote. An explicit authenticated administration operation provisions and links
that authority after the required manager/provider components are installed.
The platform installer does not collect its location or credentials and does not
provision or link any database.

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

- The identity options above scope to native SBsql only. Database/security
  administration presents the choice; the platform installer does not.
- Each exposed foreign dialect carries its own listener-handled authentication.
- To expose a foreign dialect across a network boundary, bind its listener to the
  reachable network directly (05); there is no manager in that path.

## Enforcement posture

The installer only places allowlisted provider binaries, public resources, and
inactive configuration templates. Security-provider configuration and
enforcement are engine-owned, materialized through explicit MGA-backed database
operations, and fail-closed. The outer layers (parsers, listeners, manager,
drivers) and the OS service identity are untrusted with respect to database
authority. Changing identity topology is an authenticated database policy
operation, not an installer action or reinstall.
