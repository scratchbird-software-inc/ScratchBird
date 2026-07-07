# Linux Installation Under /opt/ScratchBird

This guide describes a full Linux installation layout for the ScratchBird
2026.07.03 pre-release package tree. It is intended for beta testing and
installer construction.

## Inputs

Set `SB_BUNDLE` to the root of the staged package tree before running the
commands:

```sh
export SB_BUNDLE=/path/to/packaging/2026.07.03
```

The package tree must include:

- `server/engine/` for engine libraries.
- `server/ipc-server/` for `SBsrv`, `SBgate`, `SBmgr`, server/listener/manager
  libraries, and config templates.
- `server/sbparser/` for `SBParser`, SBParser libraries, and the server-side
  SBParser UDR support library.
- `server/resources/` for default policy packs, initial resource seed packs,
  character sets, collations, timezone data, and SBsql language resources.
- `drivers/`, `adapters/`, `tools/`, and `udr/` package directories.
- Root manifests: `FILE_LOCATION_MANIFEST.json`, `RELEASE_MANIFEST.json`, and
  `SHA256SUMS`.

## Target Layout

Recommended installed layout:

```text
/opt/ScratchBird/
  bin/
  lib/
  server/
  drivers/
  adapters/
  tools/
  udr/
  resources/
  docs/
  legal/
  manifests/

/etc/scratchbird/
  SBsrv.conf
  SBgate.conf
  SBmgr.conf
  SBParser.conf
  env.d/
  parsers/
  policies/
  tls/

/var/lib/scratchbird/
/var/log/scratchbird/
/run/scratchbird/
```

`/opt/ScratchBird` is package-owned. `/etc/scratchbird` is host-owned
configuration. `/var/lib/scratchbird`, `/var/log/scratchbird`, and
`/run/scratchbird` are runtime paths.

## Create The Runtime Account

```sh
sudo groupadd --system scratchbird || true
sudo useradd --system --gid scratchbird --home-dir /var/lib/scratchbird \
  --shell /usr/sbin/nologin scratchbird || true
```

## Verify The Package Before Installing

```sh
export SCRATCHBIRD_SOURCE=/path/to/ScratchBird
cd "$SB_BUNDLE"
sha256sum -c SHA256SUMS
python3 "$SCRATCHBIRD_SOURCE/project/tools/release/verify_prerelease_packaging_bundle.py" "$SB_BUNDLE"
```

Run the verifier from the source checkout before copying the package to a
target host.

## Create Directories

```sh
sudo install -d -o root -g root -m 0755 /opt/ScratchBird
sudo install -d -o root -g root -m 0755 /opt/ScratchBird/bin
sudo install -d -o root -g root -m 0755 /opt/ScratchBird/lib
sudo install -d -o root -g root -m 0755 /opt/ScratchBird/server
sudo install -d -o root -g root -m 0755 /opt/ScratchBird/drivers
sudo install -d -o root -g root -m 0755 /opt/ScratchBird/adapters
sudo install -d -o root -g root -m 0755 /opt/ScratchBird/tools
sudo install -d -o root -g root -m 0755 /opt/ScratchBird/udr
sudo install -d -o root -g root -m 0755 /opt/ScratchBird/resources
sudo install -d -o root -g root -m 0755 /opt/ScratchBird/docs
sudo install -d -o root -g root -m 0755 /opt/ScratchBird/legal
sudo install -d -o root -g root -m 0755 /opt/ScratchBird/manifests

sudo install -d -o root -g scratchbird -m 0750 /etc/scratchbird
sudo install -d -o root -g scratchbird -m 0750 /etc/scratchbird/env.d
sudo install -d -o root -g scratchbird -m 0750 /etc/scratchbird/parsers
sudo install -d -o root -g scratchbird -m 0750 /etc/scratchbird/policies
sudo install -d -o root -g scratchbird -m 0750 /etc/scratchbird/tls

sudo install -d -o scratchbird -g scratchbird -m 0750 /var/lib/scratchbird
sudo install -d -o scratchbird -g scratchbird -m 0750 /var/log/scratchbird
sudo install -d -o scratchbird -g scratchbird -m 0750 /run/scratchbird
```

## Copy Package Payloads

```sh
sudo rsync -a "$SB_BUNDLE/server/engine/lib/" /opt/ScratchBird/lib/
sudo rsync -a "$SB_BUNDLE/server/ipc-server/bin/" /opt/ScratchBird/bin/
sudo rsync -a "$SB_BUNDLE/server/ipc-server/lib/" /opt/ScratchBird/lib/
sudo rsync -a "$SB_BUNDLE/server/sbparser/bin/" /opt/ScratchBird/bin/
sudo rsync -a "$SB_BUNDLE/server/sbparser/lib/" /opt/ScratchBird/lib/
sudo rsync -a "$SB_BUNDLE/tools/cli/bin/" /opt/ScratchBird/bin/

sudo rsync -a "$SB_BUNDLE/server/" /opt/ScratchBird/server/
sudo rsync -a "$SB_BUNDLE/drivers/" /opt/ScratchBird/drivers/
sudo rsync -a "$SB_BUNDLE/adapters/" /opt/ScratchBird/adapters/
sudo rsync -a "$SB_BUNDLE/tools/" /opt/ScratchBird/tools/
sudo rsync -a "$SB_BUNDLE/udr/" /opt/ScratchBird/udr/
sudo rsync -a "$SB_BUNDLE/server/resources/resources/" /opt/ScratchBird/resources/
sudo rsync -a "$SB_BUNDLE/docs/" /opt/ScratchBird/docs/

sudo rsync -a "$SB_BUNDLE/server/engine/legal/" /opt/ScratchBird/legal/
sudo install -m 0644 "$SB_BUNDLE/FILE_LOCATION_MANIFEST.json" \
  /opt/ScratchBird/manifests/FILE_LOCATION_MANIFEST.json
sudo install -m 0644 "$SB_BUNDLE/RELEASE_MANIFEST.json" \
  /opt/ScratchBird/manifests/RELEASE_MANIFEST.json
sudo install -m 0644 "$SB_BUNDLE/SHA256SUMS" /opt/ScratchBird/manifests/SHA256SUMS
```

The first group of `rsync` commands flattens the common runtime binaries and
libraries into `/opt/ScratchBird/bin` and `/opt/ScratchBird/lib`. The package
directories are also copied intact so installer builders and support teams can
inspect package manifests, proofs, examples, and support material.

## Install Host Configuration

```sh
sudo install -m 0640 -o root -g scratchbird \
  "$SB_BUNDLE/server/ipc-server/config/SBsrv.conf" /etc/scratchbird/SBsrv.conf
sudo install -m 0640 -o root -g scratchbird \
  "$SB_BUNDLE/server/ipc-server/config/SBgate.conf" /etc/scratchbird/SBgate.conf
sudo install -m 0640 -o root -g scratchbird \
  "$SB_BUNDLE/server/ipc-server/config/SBmgr.conf" /etc/scratchbird/SBmgr.conf
sudo install -m 0640 -o root -g scratchbird \
  "$SB_BUNDLE/server/sbparser/config/SBParser.conf" /etc/scratchbird/SBParser.conf
```

Edit the `/etc/scratchbird` copies for the target host. For a system
installation, use absolute paths for data, control, runtime, log, TLS, and
resource roots.

Recommended resource root:

```text
/opt/ScratchBird/resources
```

Recommended durable paths:

```text
/var/lib/scratchbird
/var/log/scratchbird
/run/scratchbird
```

## Register Libraries And PATH

```sh
echo /opt/ScratchBird/lib | sudo tee /etc/ld.so.conf.d/scratchbird.conf
sudo ldconfig
```

Optional shell profile:

```sh
cat >/tmp/scratchbird.sh <<'EOF'
export SCRATCHBIRD_HOME=/opt/ScratchBird
export SCRATCHBIRD_CONFIG=/etc/scratchbird
export SCRATCHBIRD_RESOURCES=/opt/ScratchBird/resources
export PATH=/opt/ScratchBird/bin:$PATH
EOF
sudo install -m 0644 /tmp/scratchbird.sh /etc/profile.d/scratchbird.sh
rm -f /tmp/scratchbird.sh
```

## Validate Installed Binaries

```sh
/opt/ScratchBird/bin/SBsrv --version
/opt/ScratchBird/bin/SBgate --version
/opt/ScratchBird/bin/SBmgr --version
/opt/ScratchBird/bin/SBParser --probe-worker --allow-probe-auth
```

Validate configuration:

```sh
/opt/ScratchBird/bin/SBsrv --config /etc/scratchbird/SBsrv.conf --validate-config
/opt/ScratchBird/bin/SBgate --config=/etc/scratchbird/SBgate.conf --validate-config
/opt/ScratchBird/bin/SBmgr --config /etc/scratchbird/SBmgr.conf --validate-config
```

## Example systemd Units

The exact service split depends on whether `SBsrv` manages `SBgate` directly or
the node manager owns both processes. The following examples show the standard
paths for an `/opt/ScratchBird` installation.

`/etc/systemd/system/scratchbird-server.service`:

```ini
[Unit]
Description=ScratchBird Server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=scratchbird
Group=scratchbird
WorkingDirectory=/opt/ScratchBird
Environment=SCRATCHBIRD_HOME=/opt/ScratchBird
Environment=SCRATCHBIRD_CONFIG=/etc/scratchbird
Environment=SCRATCHBIRD_RESOURCES=/opt/ScratchBird/resources
ExecStart=/opt/ScratchBird/bin/SBsrv --config /etc/scratchbird/SBsrv.conf --foreground
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

`/etc/systemd/system/scratchbird-manager.service`:

```ini
[Unit]
Description=ScratchBird Single Node Manager
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=scratchbird
Group=scratchbird
WorkingDirectory=/opt/ScratchBird
Environment=SCRATCHBIRD_HOME=/opt/ScratchBird
Environment=SCRATCHBIRD_CONFIG=/etc/scratchbird
Environment=SCRATCHBIRD_RESOURCES=/opt/ScratchBird/resources
ExecStart=/opt/ScratchBird/bin/SBmgr --config /etc/scratchbird/SBmgr.conf --foreground
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Enable only the ownership model selected for the host. Do not enable separate
manager and server supervision for the same process tree unless the configured
manager profile expects that topology.

## Drivers, Adapters, And Tools

Driver packages are installed intact under `/opt/ScratchBird/drivers/<name>`.
Each driver package contains its executable or runtime payload, examples,
support material, proof sidecars, and its copy of the SBsql language resource
pack.

Adapter packages are installed intact under
`/opt/ScratchBird/adapters/<name>`. Application-specific installation steps are
documented in each adapter package support directory.

Command-line tools are installed under `/opt/ScratchBird/tools/cli` and the
common executables are flattened into `/opt/ScratchBird/bin`.

## Final Smoke Check

```sh
test -x /opt/ScratchBird/bin/SBsrv
test -x /opt/ScratchBird/bin/SBgate
test -x /opt/ScratchBird/bin/SBmgr
test -x /opt/ScratchBird/bin/SBParser
test -f /opt/ScratchBird/lib/libSBParser_udr.a
test -d /opt/ScratchBird/resources/policy-packs/default-local-password
test -d /opt/ScratchBird/resources/seed-packs/initial-resource-pack/resources/charsets
test -d /opt/ScratchBird/resources/seed-packs/initial-resource-pack/resources/collations
test -d /opt/ScratchBird/resources/seed-packs/initial-resource-pack/resources/timezones
test -d /opt/ScratchBird/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack
```

After these checks pass, start the selected service topology and inspect the
server, listener, parser, and manager logs for startup diagnostics.
