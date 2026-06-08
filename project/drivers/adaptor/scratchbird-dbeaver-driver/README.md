# ScratchBird DBeaver Integration

This folder stores ScratchBird-specific DBeaver integration assets so the
DBeaver plugin and JDBC compatibility behavior are versioned together.

Canonical findings, contract, and public proof artifacts for this adapter live in:

- `../../../../public_contract_snapshot`
- `../../../../public_contract_snapshot`
- `../../fixtures/driver_adaptor_current_repo_integration/README.md`
- `../../fixtures/driver_adaptor_current_repo_integration/artifacts/DRIVER_BUILD_TEST_MATRIX.md`
- `../../fixtures/driver_adaptor_current_repo_integration/artifacts/BENCHMARK_DRIVER_READINESS_POLICY.md`

Historical predecessor artifacts were local planning inputs and are not public
release artifacts. Public release evidence for this adapter is limited to the
in-tree contract, fixture, and artifact paths listed above.

## Current Validation Snapshot

As of `2026-04-24`, the refreshed static-example ScratchBird environment is
usable for adapter work:

- live JDBC endpoint:
  `jdbc:scratchbird://127.0.0.1:13092/main?sslmode=disable`
- direct auth probe resolves to:
  `scratchbird.auth.password_compat`
- wrapper-based JDBC live smoke is green
- local DBeaver source-checkout verify is green for the ScratchBird plugin and
  test bundle
- local DBeaver source-checkout verify is green for the ScratchBird UI plugin,
  SQL editor dependency chain, and test bundle
- DBeaver aggregate verification on 2026-04-24 passed with 26 ScratchBird
  tests, including editor-page catalog coverage and mutation authz-probe
  planning coverage
- stock DBeaver CE `26.0.2` p2 install is green from both the repository
  folder and the generated archive zip
- stock test bundle packaging is now available for handoff to non-source
  tester machines
- stock DBeaver CE `26.0.2` Xvfb GUI smoke is green for live connection
  bootstrap with the bundled ScratchBird JDBC jar
- the adapter now uses first-class `ScratchBirdSchema` objects for schema
  metadata and deterministic recursive tree ordering
- recursive root display policy is now explicit:
  user/application roots first, reserved management roots next, `sys` last
- the UI plugin contributes ScratchBird navigator context actions, reviewed
  form/report shell surfaces, SQL editor manual validation, live Java v3
  parser annotations, expanded quick-assist fixes for removed aliases, and
  quick-assist Java v3 parser completion proposals
- the generic form shell validation tab and copied review packets include
  Java v3 parser diagnostics and completion hints for generated command
  previews
- the generic form shell now routes SQL object selections to table,
  non-relational table, view, field, constraint, index, sequence, routine,
  trigger, and `sys.domains` form definitions and includes an object-context
  tab/review-packet section for audit
- the generic form shell now renders an object-specific editor-page catalog
  for every registered form, including form-specific controls, validation
  widgets, and evidence anchors in both dialog tabs and copied review packets
- mutation-mode server authorization plans now include an explicit read-only
  `sys.security.permission_probe` admission query alongside
  `sys.server_capabilities` and branch-specific representative read-only
  probes
- parser-derived DDL object routes now emit object-model identity/parentage
  hints and read-only live evidence hints, including exact domain object
  routes alongside the canonical `sys.domains` review scope
- scheduler/security lifecycle DDL now gets canonical review-scope hints and
  probe routes for `sys.jobs` and `sys.security`
- DML mutation review now warns for protected/control/client-only ScratchBird
  surfaces from referenced SQL paths and selected management targets, while
  normal concrete `data.*` object DML remains unflagged
- DML mutation review now also warns when referenced SQL paths or selected
  management targets remain on collection surfaces such as `data` roots,
  `data.<namespace>` roots, scratch workspaces, or group-public surfaces
  instead of concrete mutable child objects
- DML mutation review now warns when a statement has neither a rooted
  ScratchBird target nor a selected object context to anchor review
- DCL object permission review now warns when object `GRANT`/`REVOKE`
  statements with `ON` have neither rooted ScratchBird securable paths nor a
  selected object context, while principal and grant posture review remains
  anchored on `sys.security`
- DCL object permission review now also warns when referenced `ON` paths or
  selected management targets remain on securable collection surfaces such as
  `data` roots, `data.<namespace>` roots, `sys.domains`, scratch workspaces,
  or group-public surfaces instead of concrete securable child objects
- DCL object permission review now also warns when referenced `ON` paths or
  selected management targets remain on root, administrative collection, or
  client-only metrics surfaces instead of concrete securable child objects
- DCL object permission review now scopes those object warnings to the `ON`
  object clause, so rooted principal paths after `TO`/`FROM` are not treated
  as securable targets and do not suppress unrooted `ON` target warnings
- DCL grant-principal review now warns when principal clauses or selected
  principal review targets remain on broad surfaces such as `users`,
  `users.public`, `users.home`, `users.groups`, or `sys.security` instead of
  concrete user, role, group, or policy principals
- transaction-publication review now warns when explicit transaction-control
  batches contain schema/security metadata mutation, and dual-anchor review
  now warns when one batch mixes schema and security metadata mutation
- uncommitted-metadata-read review now warns when catalog or parser-assist
  metadata is read after uncommitted schema/security metadata mutation inside
  an explicit transaction

Preserved evidence for that rerun lives under:

- `../../../../artifacts/driver-test-env/live-gate-20260421T164813Z/`
- Legacy Execution_Plan 83 evidence has not been imported into this current driver
  tree; do not resolve it through sibling repository paths.

Use the repo wrapper for JDBC validation:

- `cd ../../driver/jdbc && ./gradlew ...`

Do not rely on a machine-global `gradle` binary for this lane; local systems
may still have an older installation that cannot evaluate the current JDBC
build.

## Contents

- `plugins/org.jkiss.dbeaver.ext.scratchbird/`
  ScratchBird DBeaver extension plugin (recursive schema tree + Generic DDL folders)
- `plugins/org.jkiss.dbeaver.ext.scratchbird.ui/`
  ScratchBird DBeaver UI plugin (context menus, form shell, parser validation,
  live editor annotations, and quick-assist fixes/completions)
- `features/org.jkiss.dbeaver.ext.scratchbird.feature/`
  p2 feature descriptor for the ScratchBird plugin
- `repository/`
  p2 update-site packaging (`category.xml`, repository module)
- `pom.xml`
  Standalone Tycho reactor for plugin/feature/repository build
- `scripts/build-p2-update-site.sh`
  Build and package a stock-installable p2 update-site zip
- `scripts/build-stock-test-bundle.sh`
  Build a tester-facing zip bundle with the p2 archive plus Unix and Windows
  stock-install helpers
- `scripts/install-into-dbeaver.sh`
  Idempotent installer for a local DBeaver source checkout
- `scripts/install-into-stock-dbeaver.sh`
  Install the bundled ScratchBird feature zip into an existing packaged
  DBeaver install on Unix-like systems
- `scripts/install-into-stock-dbeaver.bat`
  Install the bundled ScratchBird feature zip into an existing packaged
  DBeaver install on Windows
- `scripts/generate-plugin-icons.sh`
  Regenerate the ScratchBird-branded DBeaver plugin icons from the canonical
  ScratchBird SVG logo
- `test/org.jkiss.dbeaver.ext.scratchbird.test/`
  DBeaver-side integration tests for plugin metadata behavior, URL-mode
  connection handling, recursive namespace semantics, UI extension
  declarations, parser integration, and root ordering policy

## JDBC Driver Improvements Included

The JDBC driver includes a metadata compatibility switch for recursive schema
clients:

- Property: `metadataExpandSchemaParents` (aliases:
  `metadata_expand_schema_parents`, `expandSchemaParents`,
  `dbeaver_expand_schema_parents`)
- Default: `false`
- Effect when `true`: `DatabaseMetaData.getSchemas()` emits dotted parent
  segments (for example `users`, `users.alice`, `users.alice.dev`), while
  preserving pattern filtering.

This keeps default metadata behavior unchanged for non-DBeaver clients.

The plugin manifest now also publishes the full ScratchBird JDBC
ingress/auth/bootstrap property surface so stock DBeaver connection forms can
set manager-proxy, token/assertion, channel-binding, and dormant-reattach
options without hand-editing raw URLs.

## Install Into Stock DBeaver (No DBeaver Source Build)

These steps assume:

- ScratchBird driver repo: `.`
- You are using a regular DBeaver binary download (not a source checkout)

### 1) Build the p2 Update Site

```bash
cd project/drivers/adaptor/scratchbird-dbeaver-driver
./scripts/build-p2-update-site.sh
```

The build script uses the JDBC wrapper in
`project/drivers/driver/jdbc/gradlew`, stages both the JDBC driver and the
DBeaver plugin tree under `build/drivers/adaptor/scratchbird-dbeaver-driver`,
and then places the current `scratchbird-jdbc-*.jar` into the plugin bundle at
`drivers/scratchbird/scratchbird-jdbc.jar` before packaging.

Outputs:

- Repository folder:
  `build/drivers/adaptor/scratchbird-dbeaver-driver/p2-work/source/repository/target/repository`
- Installable zip:
  `build/drivers/adaptor/scratchbird-dbeaver-driver/dist/scratchbird-dbeaver-update-site-*.zip`

The 2026-04-21 live-gate bundle also contains validated archives, including:

- `artifacts/driver-test-env/live-gate-20260421T164813Z/scratchbird-dbeaver-update-site-20260421T125923.zip`
- `artifacts/driver-test-env/live-gate-20260421T164813Z/scratchbird-dbeaver-update-site-20260421T132748.zip`

### 1a) Build A Cross-Platform Stock-Test Bundle

If you want one handoff zip for tester machines, build:

```bash
cd project/drivers/adaptor/scratchbird-dbeaver-driver
./scripts/build-stock-test-bundle.sh
```

That bundle contains:

- the fresh `scratchbird-dbeaver-update-site-*.zip`
- `install-into-stock-dbeaver.sh`
- `install-into-stock-dbeaver.bat`
- `README.txt`

The current validated bundle path will depend on the build timestamp.

The latest validated bundle is:

- `build/drivers/adaptor/scratchbird-dbeaver-driver/dist/scratchbird-dbeaver-stock-test-bundle-*.zip`

That bundle contains the p2 update site, the Unix stock installer, the Windows
batch stock installer, and SHA-256 checksums. The included Windows batch
installer is packaged for off-host testing but was not executed on this Linux
machine.

### 2) Install Plugin in DBeaver UI

1. Open DBeaver
2. Go to `Help` -> `Install New Software...`
3. Click `Add...` -> `Archive...`
4. Select the generated zip from step 1
5. Choose `ScratchBird Extension`
6. Complete install and restart DBeaver

### 2a) Optional Headless Install Smoke Against a Stock DBeaver Clone

If you want to revalidate the packaged install path without touching the system
copy:

```bash
ART="${SCRATCHBIRD_DBEAVER_ARTIFACT_ROOT:-/tmp/scratchbird-dbeaver-artifacts}"
ZIP="$(ls -1t "$ART"/scratchbird-dbeaver-update-site-*.zip | head -n 1)"
DST="$ART/dbeaver-ce-stock-archive-clone-$(date +%Y%m%dT%H%M%S)"
mkdir -p "$DST"
rsync -a /usr/share/dbeaver-ce/ "$DST"/
"$DST/dbeaver" -nosplash -consoleLog \
  -application org.eclipse.equinox.p2.director \
  -repository "jar:file:$ZIP!/" \
  -installIU org.jkiss.dbeaver.ext.scratchbird.feature.feature.group \
  -destination "$DST" \
  -profile DefaultProfile \
  -bundlepool "$DST" \
  -profileProperties org.eclipse.update.install.features=true
"$DST/dbeaver" -nosplash -consoleLog \
  -application org.eclipse.equinox.p2.director \
  -destination "$DST" \
  -profile DefaultProfile \
  -bundlepool "$DST" \
  -listInstalledRoots
```

### 2b) Install From The Bundle Scripts

Unix-like systems:

```bash
./install-into-stock-dbeaver.sh
```

or:

```bash
./install-into-stock-dbeaver.sh /path/to/dbeaver-install
```

Windows:

```bat
install-into-stock-dbeaver.bat
```

or:

```bat
install-into-stock-dbeaver.bat DBEAVER_INSTALL_DIR
```

If you omit the install path, the stock installers attempt to discover DBeaver
from `DBEAVER_HOME`, `PATH`, and common install locations. They also accept a
launcher path through the same argument.

If you keep the bundled `scratchbird-dbeaver-update-site-*.zip` next to the
install script, both installers auto-detect it. You can also pass the zip path
explicitly as the second argument. The Unix helpers also re-exec under `bash`
automatically if they are started with `sh`.

### 3) Bundled JDBC Library

The stock update-site and `install-into-dbeaver.sh` developer flow now bundle
the current ScratchBird JDBC jar automatically. Manual Driver Manager library
attachment is only needed if you bypass the provided scripts and load the
plugin sources directly.

### 4) Optional: Enable Parent Schema Expansion (JDBC Layer)

If you want parent schema rows emitted directly by JDBC metadata:

- Add connection property `metadataExpandSchemaParents=true`

Use this only when needed; default `false` keeps non-DBeaver metadata behavior
unchanged.

### Policy Note

If `Help` -> `Install New Software...` is disabled in your DBeaver install,
software install/update policies are being enforced by that environment.
In that case, use an unmanaged install or ask your administrator to allow
plugin installation.

## Install Into DBeaver Source Checkout (Developer Flow)

These steps assume:

- ScratchBird driver repo: `.`
- DBeaver repo: `../dbeaver`
- DBeaver common repo: `../dbeaver-common`

### 1) Copy Plugin/Test Sources Into DBeaver Checkout

```bash
cd project/drivers/adaptor/scratchbird-dbeaver-driver
./scripts/install-into-dbeaver.sh ../dbeaver
```

This copies plugin/test-plugin folders and patches:

- `../dbeaver/plugins/pom.xml`
- `../dbeaver/test/pom.xml`
- `../dbeaver/features/org.jkiss.dbeaver.db.feature/feature.xml`
- `../dbeaver/features/org.jkiss.dbeaver.test.feature/feature.xml`

The installer matches existing module/plugin IDs semantically, so reruns do not
duplicate lines if the target files already contain ScratchBird entries with
different whitespace formatting. It also stages the current ScratchBird JDBC
jar into the copied plugin so the source-checkout runtime path matches the
stock packaged install path.

### 2) Build/Verify In DBeaver

```bash
cd ../dbeaver
../dbeaver-common/mvnw -f product/aggregate/pom.xml \
  -pl ../../../dbeaver-common/modules/org.jkiss.utils,../../../dbeaver-common/modules/com.dbeaver.jdbc.api,../../plugins/org.jkiss.dbeaver.model,../../plugins/org.jkiss.dbeaver.model.rcp,../../plugins/org.jkiss.dbeaver.model.jdbc,../../plugins/org.jkiss.dbeaver.model.lsm,../../plugins/org.jkiss.dbeaver.model.sql,../../plugins/org.jkiss.dbeaver.model.sql.jdbc,../../plugins/org.jkiss.dbeaver.registry,../../plugins/org.jkiss.dbeaver.ui,../../plugins/org.jkiss.dbeaver.ui.forms,../../plugins/org.jkiss.dbeaver.ui.editors.base,../../plugins/org.jkiss.dbeaver.ui.editors.connection,../../plugins/org.jkiss.dbeaver.ui.editors.data,../../plugins/org.jkiss.dbeaver.ui.editors.entity,../../plugins/org.jkiss.dbeaver.ui.navigator,../../plugins/org.jkiss.dbeaver.data.transfer,../../plugins/org.jkiss.dbeaver.data.transfer.ui,../../plugins/org.jkiss.dbeaver.tasks.ui,../../plugins/org.jkiss.dbeaver.tasks.native,../../plugins/org.jkiss.dbeaver.tasks.native.ui,../../plugins/org.jkiss.dbeaver.ui.editors.sql,../../plugins/org.jkiss.dbeaver.ext.generic,../../plugins/org.jkiss.dbeaver.ext.scratchbird,../../plugins/org.jkiss.dbeaver.ext.scratchbird.ui,../../test/org.jkiss.dbeaver.ext.scratchbird.test \
  -am verify -DskipITs
```

Expected: `BUILD SUCCESS`.

If the local DBeaver checkout was reset or no longer contains the ScratchBird
plugin/test modules, rerun:

```bash
cd project/drivers/adaptor/scratchbird-dbeaver-driver
./scripts/install-into-dbeaver.sh ../dbeaver
```

## Update Workflow

When integration code changes:

1. Update files under this folder
2. Rebuild the p2 archive: `./scripts/build-p2-update-site.sh`
3. Reinstall/update in DBeaver via `Help` -> `Install New Software...`
4. If you reseed a DBeaver source checkout, rerun `./scripts/install-into-dbeaver.sh`

## Remaining Open Validation

- deeper GUI validation of recursive navigator browse, datatype presentation,
  SQL editor behavior, and future admin/dashboard surfaces
- Phase B and Phase C work beyond the now-proven live connection bootstrap
- broader semantic lint policy beyond the current route-aware DDL/DML/DCL and
  transaction-publication/dual-anchor and uncommitted-metadata-read checks,
  scheduler/security lifecycle routing, protected-surface DML warnings,
  collection-surface DML warnings, unrooted DML warnings,
  DCL securable-object warnings, DCL securable collection-surface warnings,
  DCL broad-surface warnings, DCL ON-target scoping, DCL broad-principal
  warnings, transaction-publication warnings, dual-anchor schema/security
  mutation warnings, uncommitted-metadata-read warnings, alias migrations,
  parser completion proposals, Ctrl+Space popup, object-model hints, and
  live-evidence hints
- server implementation and live result evidence for
  `sys.security.permission_probe`
- human GUI screenshot evidence plus live stock install/remove traces for the
  current stock QA bundle
