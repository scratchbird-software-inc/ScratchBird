# Operating Policy Presets

Status: public specification baseline
Search key: `INSTALLER-POLICY`

When the engine is installed (especially in service mode), the installer proposes
an initial **operating policy** seeded from the machine probe (06). Policy is a
**series of presets the user picks from**, all editable — not a single
conservative/assertive toggle. How the engine runs is a separate axis from what is
installed: a developer may install the full SDK yet run a small on-demand policy,
and a spare machine may run a dedicated-server policy regardless of persona.

## Presets

| Preset | Strategy it encodes | Typical picker |
| --- | --- | --- |
| Developer — on demand | small resident footprint, manual start/stop, autostart off, loopback, tiny cache; coexists politely on a shared workstation | application developer |
| Balanced / shared host | modest fixed share of RAM, autostart optional | mixed-use machine |
| Dedicated server | assertive — large RAM share, autostart on, worker/connection counts from CPU, data directory on a dedicated volume | operator / DBA on dedicated hardware |
| Minimal / embedded | smallest cache, in-process, data directory under the user profile | embedder, constrained host |
| Evaluation / sandbox | tiny, sample data, easy teardown | evaluator |
| Custom | start from the probed numbers, edit every knob | anyone |

Each preset is the same set of knobs with a different strategy. The probe fills in
the machine-specific numbers; the preset chooses the strategy; the user edits the
result. The presets are data-driven (named entries with formulas) so the same
definitions power the wizard and the silent-install answer file (06).

## Knobs

| Knob | Seeded from | Notes |
| --- | --- | --- |
| Engine memory budget | free RAM | page/buffer cache + work memory, as a capped fraction of RAM |
| Data directory | per-volume free disk | placed on the roomiest eligible volume; shows free space; warns when low |
| Autostart | preset / persona | off by default for developer presets |
| Max connections / workers | CPU cores | |
| Background maintenance cadence | preset | the engine's idle/background maintenance rhythm |
| Listener bind / port / firewall | network probe (05) | safe default loopback, firewall closed |

Embedded mode uses a light policy (cache + data directory under the user profile,
loopback). Service mode uses the full policy plus the safe-by-default service
settings (loopback, autostart off, firewall closed unless opted in).

## Resource division across databases

Because one server process manages one database (01), a managed multi-database host
runs several server processes. Policy must **divide host RAM and CPU across the N
server processes**, not size a single engine. For the dedicated-server preset on a
multi-database host, the installer asks how many databases the host will carry
before sizing each server's memory budget, and reserves headroom for the manager
and listeners.

## Proxy-manager sizing

The manager's resource needs depend on its connection behavior (04):

| Manager mode | Resource profile |
| --- | --- |
| Redirect | control path only — authenticate and hand off; light |
| Proxy | data path — carries connection traffic; size for throughput and concurrent connections |

A bastion/proxy manager (03) must therefore be sized for bandwidth and connection
count, not only for hosting the master/security database. The dedicated-server
preset accounts for this when the manager is configured to proxy.

## Reconfiguration

Operating policy is engine configuration materialized from catalog policy; it is
editable after install. The installer seeds the initial values — it does not lock
them.
