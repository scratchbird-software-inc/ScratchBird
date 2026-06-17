# Installer Behavior and UX

Status: public specification baseline
Search key: `INSTALLER-UX`

This document defines how an installer behaves: its packaging shape, its
privilege model, the machine-probe phase, the two-stage selection, and the
repair/update/uninstall and silent-install flows. Mobile is a separate
dependency-packaging track, covered at the end.

## Packaging shape: native-first, fat-offline, coordinator

Each installer is a **coordinator/bundle over per-element sub-packages** rendered in
the platform's native idiom, carrying all payloads (fat offline).

| Platform | Coordinator | Per-element unit |
| --- | --- | --- |
| Windows | WiX **Burn** bundle — one UI, one elevation, one Add/Remove entry, repair/update | per-element MSI / merge module (`.msm`) |
| macOS | distribution `.pkg` (`productbuild`) with `choices` for selection | per-element component `.pkg` |
| Linux | a coordinator that detects the distribution and drives the native package manager over the bundled element packages | per-element `.deb` / `.rpm` / AUR `PKGBUILD` + persona metapackages |

For fat-offline, the coordinator installs the *selected subset* from the bundled
package set, resolving dependencies from within the bundle rather than from a
remote repository.

## Privilege model: late, single elevation

The installer is split into an unprivileged front-end and a privileged executor:

1. The **front-end** runs without elevation: probe, target-group selection,
   component menu, configuration (engine mode, identity topology, listeners/network,
   policy), and a review **summary**.
2. **Elevation is requested once, at commit**, after the summary — UAC on Windows,
   Authorization Services on macOS, `pkexec`/`sudo` (polkit) on Linux.
3. The **executor** performs file placement, package installs, service registration,
   and PATH/registry changes.

Per-user-only elements install without any elevation prompt. Per-machine is the
default; per-user is the fallback where elevation is unavailable.

## Probe phase (before selection)

The installer probes the machine, read-only, to drive branching and suggestions:

| Probed | Used for |
| --- | --- |
| Existing install / running instance | Detect repair/update/modify/uninstall (package database, service state, listening ports, data-dir version stamp). A running instance on a known listener port is the quickest "already here" signal. |
| RAM, per-volume free disk, CPU cores | Seed editable operating-policy presets (07): memory budget, data-directory placement, worker/connection counts. |
| Network interfaces | Offer interface/network bindings for listeners and the manager (05); detect `(interface, port)` conflicts. |
| OS version / architecture | Filter which catalog elements are eligible. |
| Admin rights | Per-machine needs elevation; offer per-user fallback if denied. |

Detection mechanics per platform: Windows — MSI product code + registry + service
query + port; macOS — `pkgutil` receipts + launchd + port; Linux — `dpkg`/`rpm`
database + `systemctl` + port + data-dir version stamp.

## Branching on an existing install

When the probe finds an existing install, the same installer offers: **Repair**,
**Update**, **Modify** (add/remove components), **Uninstall**, or **Side-by-side**
(a distinct version). The installer is the uninstaller — the probe makes it
state-aware rather than requiring a separate tool.

## Two-stage selection

1. **Choose an overall target group** (persona, 02): App developer, Operator,
   Single-driver, Embedder, Evaluator. This pre-selects a sane component set.
2. **Editable component menu**, grouped by category (Engine · Parsers · Drivers ·
   Tools · AI · Docs · Examples). Toggling resolves dependencies automatically,
   shows per-element *what/why/size*, and displays a live total with a free-space
   check.

Configuration steps follow as applicable: engine install mode (embedded vs service),
identity topology (04, only when a first database is created), exposed dialects /
listener network bindings (05), and the operating-policy preset (07).

## Silent / headless install

The element catalog (02) generates an **answer-file schema**, so the same selection
and configuration can be supplied non-interactively for CI, MDM, and enterprise
(GPO) deployment. Probe results can be overridden in the answer file.

## Mobile track (separate)

Android and iOS are not installers — they ship **dependency packages** consumed by
an app build, and only the embeddable engine and client-driver elements apply
(filtered by `platforms[]`, 02):

| Platform | Distribution |
| --- | --- |
| Android | AAR / Maven (driver); engine `.so` via NDK where embedded |
| iOS | Swift Package / CocoaPods / XCFramework (in-process only; no service) |

There is no wizard, no coordinator, no elevation, and no service on mobile.
