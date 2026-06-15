# DBeaver Adaptor

> **beta_2 / release_candidate.**
> This page describes the ScratchBird DBeaver plugin, which extends DBeaver CE
> and DBeaver Pro to connect to ScratchBird via the ScratchBird JDBC driver.

## Purpose

The `scratchbird-dbeaver-driver` adaptor integrates ScratchBird — a Convergent
Data Engine (CDE) speaking SBWP v1.1 on default port 3092 — into DBeaver
Community Edition (CE) and compatible DBeaver distributions.  It is an Eclipse
plugin bundle (p2 feature) that teaches DBeaver ScratchBird-specific behaviour:
recursive schema tree navigation, ScratchBird data types, SQL editor parser
annotations, quick-assist fixes, UI context actions, and mutation-review
warnings.

The adaptor delegates all network I/O to the ScratchBird JDBC driver
(`driver:jdbc`).  The plugin bundle includes the JDBC JAR automatically; manual
Driver Manager attachment is only needed when bypassing the provided installers.

**Release status:** beta_2, release_candidate.
**Validated against:** DBeaver CE 26.0.2.

---

## Manifest Metadata

| Field | Value |
|---|---|
| `component_id` | `adaptor:scratchbird-dbeaver-driver` |
| `driver_package_uuid` | `019e12a0-0017-7000-8000-000000000017` |
| `api_surface_set` | `application_adapter` |
| `ingress_mode_set` | `manager_proxy`, `driver_embedded_jdbc` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `jdbc_mapping` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `connection_thread_confined` |
| `pooling_capability` | `delegates_to_jdbc` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `adaptor_dbeaver_gate` |
| Delegates to | `driver:jdbc` (ScratchBird JDBC driver) |
| Build system | Tycho 4.0.8 (Maven), Eclipse 2025-12 target platform |
| JDBC driver class | `com.scratchbird.jdbc.SBDriver` |
| License | MPL-2.0 |

**Delegation posture:** `delegates_to_jdbc`.  The plugin shapes DBeaver UI,
metadata, and SQL-editor requests; the JDBC driver handles all transport and
protocol binding.  Authentication authority, MGA transaction finality,
authorization, and UUID identity remain engine-owned.

---

## Eclipse Plugin Modules

The adaptor is structured as an Eclipse Tycho project with four modules:

| Module | Artifact ID | Role |
|---|---|---|
| `org.jkiss.dbeaver.ext.scratchbird` | Model plugin | JDBC metadata, recursive schema tree (`ScratchBirdSchema`), Generic DDL folders, `SBDriver` wiring |
| `org.jkiss.dbeaver.ext.scratchbird.ui` | UI plugin | Navigator context actions, form shell, parser validation, live editor annotations, quick-assist fixes and completion proposals |
| `org.jkiss.dbeaver.ext.scratchbird.feature` | p2 feature | Feature descriptor bundling both plugins for p2 install |
| `repository` | p2 update-site | Category.xml and p2 repository module |

Build artifacts (from `pom.xml`):

- `groupId`: `com.scratchbird.dbeaver`
- `artifactId`: `scratchbird-dbeaver-driver-build`
- `version`: `1.0.1-SNAPSHOT`
- `Bundle-Version`: `1.0.1.qualifier` (model plugin MANIFEST.MF)
- `Bundle-RequiredExecutionEnvironment`: JavaSE-21

---

## Installation into Stock DBeaver (No Source Build)

This is the recommended path for end users.

### 1. Build the p2 Update Site

```bash
cd project/drivers/adaptor/scratchbird-dbeaver-driver
./scripts/build-p2-update-site.sh
```

The script invokes the JDBC wrapper at `project/drivers/driver/jdbc/gradlew`,
stages the current JDBC JAR into the plugin bundle, and produces:

- **Repository folder:**
  `build/drivers/adaptor/scratchbird-dbeaver-driver/p2-work/source/repository/target/repository`
- **Installable zip:**
  `build/drivers/adaptor/scratchbird-dbeaver-driver/dist/scratchbird-dbeaver-update-site-*.zip`

Alternatively, build a cross-platform tester bundle (includes installer scripts
and checksums):

```bash
./scripts/build-stock-test-bundle.sh
# Output: build/drivers/adaptor/scratchbird-dbeaver-driver/dist/scratchbird-dbeaver-stock-test-bundle-*.zip
```

### 2. Install in DBeaver

1. Open DBeaver.
2. Go to **Help → Install New Software...**
3. Click **Add... → Archive...** and select the generated zip from step 1.
4. Select **ScratchBird Extension**.
5. Complete the install wizard and restart DBeaver.

**Headless (p2 director) install** is also supported for automated environments:

```bash
dbeaver -nosplash -consoleLog \
  -application org.eclipse.equinox.p2.director \
  -repository "jar:file:<path-to-zip>!/" \
  -installIU org.jkiss.dbeaver.ext.scratchbird.feature.feature.group \
  -destination <dbeaver-install-dir> \
  -profile DefaultProfile \
  -bundlepool <dbeaver-install-dir> \
  -profileProperties org.eclipse.update.install.features=true
```

**Stock bundle installer scripts** (from `scripts/`):

| Script | Platform | Usage |
|---|---|---|
| `install-into-stock-dbeaver.sh` | Unix-like | `./install-into-stock-dbeaver.sh [/path/to/dbeaver]` |
| `install-into-stock-dbeaver.bat` | Windows | `install-into-stock-dbeaver.bat [DBEAVER_INSTALL_DIR]` |

If the `scratchbird-dbeaver-update-site-*.zip` is next to the installer script,
it is auto-detected.  The JDBC JAR is bundled in the update site; manual Driver
Manager attachment is not needed when using the provided installers.

> If **Help → Install New Software...** is disabled in your DBeaver install,
> p2 install policies are being enforced.  Use an unmanaged install or contact
> your administrator.

---

## Installation into DBeaver Source Checkout (Developer Flow)

```bash
cd project/drivers/adaptor/scratchbird-dbeaver-driver
./scripts/install-into-dbeaver.sh ../dbeaver
```

This copies plugin and test-plugin folders and patches the DBeaver aggregator
POMs (`plugins/pom.xml`, `test/pom.xml`) and feature XMLs.  Then build:

```bash
cd ../dbeaver
../dbeaver-common/mvnw -f product/aggregate/pom.xml \
  -pl ...,../../plugins/org.jkiss.dbeaver.ext.scratchbird,\
../../plugins/org.jkiss.dbeaver.ext.scratchbird.ui,\
../../test/org.jkiss.dbeaver.ext.scratchbird.test \
  -am verify -DskipITs
```

Expected result: `BUILD SUCCESS`.

---

## Configuring a Connection in DBeaver

The plugin manifest exposes the full ScratchBird JDBC ingress/auth/bootstrap
property surface in DBeaver's connection form.  Standard fields:

| Field | Manifest key | Notes |
|---|---|---|
| Host | `host` | Hostname or IP address |
| Port | `port` | Default: 3092 |
| Database | `database` | Database name |
| Username | `user` | ScratchBird user principal |
| Password | `auth_method` | `engine_local_password` or `scram_ready` |

TLS: set `sslmode=require` (or stronger) in the connection properties.
The `scratchbird_tls_1_3_floor` TLS profile enforces TLS 1.3 as the minimum.
See [../tls_profiles.md](../tls_profiles.md).

Additional JDBC properties available in the connection form include:
`sslmode`, `sslrootcert`, `sslcert`, `sslkey`, `currentSchema`, manager-proxy
fields (`managerAuthToken`, `managerUsername`, etc.), token/assertion,
channel-binding, and dormant-reattach options.  See
[../drivers/jdbc.md](../drivers/jdbc.md) for the full JDBC property reference.

Authentication is engine-owned.  See [../authentication.md](../authentication.md).

### Live JDBC Endpoint (Validated)

From the DBeaver README validation snapshot (2026-04-24):

```
jdbc:scratchbird://127.0.0.1:13092/main?sslmode=disable
```

(Port 13092 is a local test fixture; production port is 3092.)

---

## Recursive Schema Tree

The plugin uses first-class `ScratchBirdSchema` objects for schema metadata
and deterministic recursive tree ordering.  Root display policy (from README):

> User/application roots first, reserved management roots next, `sys` last.

The JDBC driver includes a metadata compatibility switch for recursive-schema
clients:

| JDBC property | Aliases | Default | Effect when `true` |
|---|---|---|---|
| `metadataExpandSchemaParents` | `metadata_expand_schema_parents`, `expandSchemaParents`, `dbeaver_expand_schema_parents` | `false` | `DatabaseMetaData.getSchemas()` emits dotted parent segments (e.g. `users`, `users.alice`, `users.alice.dev`) while preserving pattern filtering |

Enable this only when needed; the default preserves standard JDBC metadata
behaviour for non-DBeaver clients.

---

## SQL Editor Features

The UI plugin (`org.jkiss.dbeaver.ext.scratchbird.ui`) contributes:

- ScratchBird navigator context actions
- Form/report shell surfaces with a generic form shell validation tab
- SQL editor manual validation with live Java v3 parser annotations
- Quick-assist fixes for removed aliases
- Quick-assist Java v3 parser completion proposals (Ctrl+Space)
- Object-model identity/parentage hints and read-only live evidence hints
- Mutation-review warnings for:
  - DDL/DML/DCL on protected/control/client-only ScratchBird surfaces
  - DML on collection surfaces instead of concrete mutable child objects
  - Unrooted DML (no rooted ScratchBird target or selected object context)
  - DCL `GRANT`/`REVOKE ON` referencing collection or broad surfaces
  - DCL broad-principal warnings (`users`, `users.public`, etc.)
  - Transaction-publication batches containing schema/security metadata mutation
  - Dual-anchor batches mixing schema and security metadata mutation
  - Uncommitted-metadata-read warnings after uncommitted schema/security mutation in an explicit transaction

---

## Type Mapping

This adaptor uses the `jdbc_mapping` type-mapping profile (inherited from the
underlying JDBC driver).  See [../type_mapping.md](../type_mapping.md) for the
full mapping table.

---

## Capabilities and Limitations

| Capability | Status |
|---|---|
| JDBC connection | Supported |
| Recursive schema tree | Supported (`ScratchBirdSchema`, deterministic ordering) |
| `metadataExpandSchemaParents` | Optional JDBC property; default `false` |
| SQL editor with parser annotations | Supported (Java v3 parser) |
| Quick-assist and completion | Supported |
| Mutation-review warnings | Supported (DDL/DML/DCL/transaction-publication) |
| Authentication | Engine-owned via JDBC driver |
| MGA transaction finality | Engine-owned; adaptor cannot override |
| Authorization | Engine-owned; server revalidates all claims |
| Deeper GUI validation (Phase B/C) | Pending (see DBeaver README open items) |
| `sys.security.permission_probe` | Server implementation pending |
| Human GUI screenshot evidence | Pending |

---

## Diagnostics

SQLSTATE codes use the `native_sqlstate` diagnostic mapping profile, surfaced
through the JDBC driver.
See [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md).

Test suite: `test/org.jkiss.dbeaver.ext.scratchbird.test/` (26 ScratchBird
tests covering plugin metadata, URL-mode connection, recursive namespace
semantics, UI extension declarations, parser integration, and root ordering
policy; validated 2026-04-24).

---

## See Also

- [../README.md](../README.md) — Client and Driver Guide overview
- [../drivers/jdbc.md](../drivers/jdbc.md) — ScratchBird JDBC driver (underlying driver)
- [../connection_and_dsn.md](../connection_and_dsn.md) — DSN and connection-string reference
- [../authentication.md](../authentication.md) — Authentication methods
- [../tls_profiles.md](../tls_profiles.md) — TLS profiles
- [../type_mapping.md](../type_mapping.md) — Type mapping profiles
- [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md) — Diagnostics and SQLSTATE
