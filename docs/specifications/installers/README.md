# ScratchBird Installer Specifications

Status: public specification baseline
Search key: `INSTALLER`

This directory is the public, shareable specification for how ScratchBird is
packaged and installed on end-user machines. It describes what each installer
must accomplish, the catalog of installable elements, the deployment and
identity topologies an installer offers, the listener/network model, the
installer's behavior and user experience, the machine-probed operating-policy
presets, and the per-platform packaging and build targets.

It is intentionally public. It does not expose engine, transaction, or security
internals — it describes the install/configuration surface a user sees and the
contracts an installer honors. It covers standalone, managed, and federated
deployments of the open-source engine.

## Documents

| # | Document | Covers |
| --- | --- | --- |
| 01 | [Architecture and Runtime Tiers](01-architecture-and-runtime-tiers.md) | driver → manager / listener+parser pool → server (one DB per server); connection flows; IPC; network segmentation |
| 02 | [Element Catalog](02-element-catalog.md) | the single platform-agnostic component catalog: element schema, inventory, audiences, metapackages |
| 03 | [Deployment Topologies](03-deployment-topologies.md) | standalone, managed single-host, managed federated, bastion/proxy |
| 04 | [Identity and Security Topology](04-identity-and-security.md) | native-SBsql authentication options (self / external+hybrid / central); foreign-dialect listener auth |
| 05 | [Listener and Network Model](05-listener-and-network-model.md) | the listener tuple (network/mask, port, dialect, server); multi-homing; port/interface conflicts; firewall |
| 06 | [Installer Behavior and UX](06-installer-behavior-and-ux.md) | native-first, fat-offline, coordinator/bundle, late elevation, probe phase, two-stage selection, repair/update/uninstall, silent install, mobile |
| 07 | [Operating Policy Presets](07-operating-policy-presets.md) | probe-seeded, editable policy presets; resource division across databases |
| 08 | [Packaging and Build Targets](08-packaging-and-build-targets.md) | platform × arch matrix; native packaging tech; cross-compile-first build and signing |

## Cross-cutting principles

| Principle | Statement |
| --- | --- |
| One catalog, many frontends | Components are defined once in the platform-agnostic element catalog (02). Every installer is a frontend that renders that catalog in the platform's native idiom. Components are never described twice. |
| Native first | Native installers (MSI, `.pkg`, `deb`/`rpm`) are primary. Package-manager channels (winget, Homebrew, apt/dnf repos) are later thin wrappers over the same payloads. |
| Fat offline | Installers carry all payloads for the platform; component selection chooses what to *place*, never what to download. Air-gap and enterprise friendly. |
| Per-machine default | Installs are per-machine by default (required for services and system-wide driver registration), with a per-user fallback where no elevation is available. |
| Late, single elevation | The selection and configuration UI runs unprivileged; elevation is requested once, at commit, after the user reviews the summary. |
| Probe-driven | Before selection, the installer probes the machine (existing installs, RAM, disk, CPU, network interfaces) to drive repair/update/uninstall branching and editable policy suggestions. |
| Cross-compile first | The build pipeline cross-compiles as much as possible from one host to keep cost low; see 08. |

## Audience

Open-source users installing ScratchBird: application developers, operators/DBAs,
single-driver consumers, embedders, and evaluators. The element catalog (02)
defines audience tags; the installer presents an overall target group first, then
an editable component menu (06).
