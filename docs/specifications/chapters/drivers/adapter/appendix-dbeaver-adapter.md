# Appendix: DBeaver Adapter Lane

Status: public release contract baseline
Search key: `DRIVER-ADAPTER-DBEAVER`

This appendix is the public, non-draft DBeaver adapter surface for the current
ScratchBird source tree. It replaces stale historical lane paths and the older
partial-plugin posture. It does not override private canonical engine,
transaction, security, or driver-core specifications.

## Lane Identity

| Field | Value |
| --- | --- |
| Component id | `adaptor:scratchbird-dbeaver-driver` |
| Kind | `adapter` |
| Package name | `scratchbird-dbeaver-driver` |
| Public source root | `project/drivers/adaptor/scratchbird-dbeaver-driver` |
| Driver package manifest | `project/drivers/DriverPackageManifest.csv#adaptor:scratchbird-dbeaver-driver` |
| Current manifest status | `beta_2` |
| Release bucket | `release_candidate` |
| Existing legacy profile | `adaptor_dbeaver_gate` |
| DBeaver management release controller | `project/tools/release/dbeaver_management_platform_gate.py` |
| Release contract input | `project/tools/release/dbeaver_management_release_contract.json` |
| Install lifecycle fixture | `project/tests/release/dbeaver_management_install_lifecycle_fixture.json` |
| Static management corpus gate | `project/tests/conformance/drivers/validate_dbeaver_management_platform_corpus.py` |

## Scope

DBeaver is the ScratchBird management-console adapter lane. The adapter must
provide a stock DBeaver CE integration path for connection configuration,
metadata browsing, SBsql editor assistance, management review workflows,
monitoring surfaces, install lifecycle validation, and deterministic refusal of
unsupported or unavailable surfaces.

The current source is no longer only a packaged database-type bootstrap. It
contains the model plugin, UI plugin, p2 feature/repository packaging, source
checkout and stock-install scripts, recursive schema/navigation support, local
Java v3 parser integration, SQL validation and quick assist hooks, management
dialog workflow surfaces, static security/redaction checks, static GUI proof
checks, DBeaver management corpus fixtures, and a DBeaver-specific release
preflight gate.

The current source still does not prove release-complete management-platform
closure. The public release contract must treat live server management proof,
stock DBeaver GUI automation, release artifact promotion, manual QA, workspace
redaction, and full mutation apply/verify coverage as required before final
closure.

## Authority Boundaries

DBeaver is never the authentication, authorization, transaction-finality,
catalog-truth, SBLR/UUID, or hidden-object visibility authority.

All local SBsql parsing, editor diagnostics, completion, prompt planning,
resolver cache entries, generated DDL/SBsql text, generated SBLR/UUID payloads,
data-editor plans, import/export plans, object graph plans, and management
previews are client-side advisory material until the ScratchBird server admits
and revalidates them for the active principal, role/group set, policy epoch,
catalog/security epoch, UUID visibility set, language resource hash, and active
MGA transaction context.

Transaction finality belongs to ScratchBird MGA transaction inventory and
server-owned transaction lifecycle. DBeaver may request commit, rollback,
autocommit-compatible work, savepoint behavior, cancellation, reconnect, and
status refresh through admitted server/JDBC routes, but it must not present the
client UI, local parser, or generated text/SBLR stream as finality evidence.

## Implemented Static Surfaces

The current source provides these public, inspectable surfaces:

- p2 feature and repository packaging for `org.jkiss.dbeaver.ext.scratchbird.feature`.
- Core and UI plugins `org.jkiss.dbeaver.ext.scratchbird` and `org.jkiss.dbeaver.ext.scratchbird.ui`.
- Tycho build target `1.0.1-SNAPSHOT`, JavaSE-21, Eclipse `2025-12`, and Tycho `4.0.8`.
- ScratchBird JDBC driver registration with class `com.scratchbird.jdbc.SBDriver` and default port `3092`.
- DBeaver extension-point coverage for object managers, data type provider, generic metadata, data source provider, dashboards, SQL editor add-ins, quick fixes, commands, handlers, menus, and property testers.
- Source checkout, p2 update-site, stock install, stock uninstall, and stock test bundle scripts.
- Sensitive connection property redaction and DBeaver `secured,password` property handling for token/assertion/payload fields.
- Authorization-context, session-scope, resolver-cache, feature-boundary, refusal, and management-action envelope models.
- Management workflow UI that records preview identity, server-admission status, guarded apply status, refresh/verify status, feature-boundary status, long-operation posture, and audit history.
- Static contract models for data editor, data transfer, and object graph workflows that require server admission and server-side authority before mutation, import/export, visibility, or finality claims.

## Current Proof Level

The DBeaver-specific release controller is a static/release preflight, not final
closure proof.

`project/tools/release/dbeaver_management_platform_gate.py` validates:

- supported-version and unsupported-version contract rows;
- DBeaver/Eclipse/Tycho/API drift anchors;
- p2/source/stock install lifecycle fixture coverage;
- workspace migration fixture coverage;
- SPDX, DBeaver/Eclipse/Tycho/SWT/JFace/ScratchBird JDBC notice boundaries;
- runtime network egress policy;
- release-mode package artifact, source-checkout install proof, and stock
  install lifecycle proof presence when an artifact root is supplied.

The gate does not launch the DBeaver workbench UI, exercise SWT UI flows,
connect to a live ScratchBird server, run manual QA, or prove every
multi-session/security/data-editor/data-transfer/object-graph behavior against
server truth. Its report must remain a preflight summary.

## Supported Version Contract

The current public release contract records:

| Target | Status | Boundary |
| --- | --- | --- |
| DBeaver CE `26.0.2`, JavaSE-21, Eclipse `2025-12`, Tycho `4.0.8`, Linux x86_64 | `supported_static_contract` | Static contract/API/install-script gate only; live server and stock GUI proof still required before release closure. |
| DBeaver CE `26.0.2`, JavaSE-21, Eclipse `2025-12`, Tycho `4.0.8`, Windows x86_64 | `supported_packaged_offhost` | Windows installer is packaged for off-host validation; it is not Linux-host live proof. |
| DBeaver CE versions below `26.0.2` | `unsupported_refusal` | Must fail before release promotion with `unsupported_dbeaver_version`. |
| Java runtimes below JavaSE-21 | `unsupported_refusal` | Must fail before release promotion with `unsupported_java_version`. |

The public adapter makes no DBeaver Enterprise release claim. Enterprise-only,
closed-provider-only, unsupported, unavailable, or policy-denied surfaces must
be visible deterministic refusals rather than hidden failures or implied support.

## Management Apply Boundary

Current UI workflow code keeps mutation apply disabled until the server admits
the exact preview hash and command hash for the active authorization context.
The dialog labels the unsafe state `Apply Requires Admission`; after the
server admits the exact preview/command pair, the guarded path labels the action
`Apply Admitted Preview`, submits through the ordinary ScratchBird JDBC/server
route, refreshes from server truth when possible, and records local audit
history. Server admission, authorization, UUID/SBLR validation, and MGA
transaction finality remain server-owned.

Static GUI/security proof checks verify that mismatched permission-probe rows,
preview hashes, command hashes, and non-mutation plans are refused. Live server
corpus proof is still required before claiming complete management-platform
apply/verify coverage for every object family and policy combination.

## Closure Blockers

Final DBeaver management-platform closure remains blocked until public gates or
approved private release evidence prove all of the following:

- live ScratchBird server management corpus and clean reset;
- stock DBeaver GUI automation with screenshots;
- DBeaver source-checkout plugin test execution inside the DBeaver reactor;
- p2 update site and stock bundle promotion, SBOM, signing, off-host platform
  installation, and cleanup proof beyond the Linux stock-copy lifecycle;
- workspace metadata, secure storage, exported connection, local history, clipboard, screenshot, and support bundle redaction;
- server authorization for every management action and every generated SBsql/SBLR/UUID bundle;
- preview/apply parity, stale/deleted/concurrent object refusal, and post-apply refresh from server truth;
- data editor CRUD, transactions, batching, refresh, import/export, data transfer, object graph, generated DDL/SBsql, and explain behavior against server truth;
- long-running operation progress, cancellation, timeout, reconnect, safe retry, and audit visibility;
- multi-connection and multi-session isolation;
- offline/degraded-mode behavior;
- performance, accessibility, localization, manual QA, CE/Enterprise boundary, feature-boundary refusal, network policy, and license/IP evidence.

No release artifact or status report may mark this lane complete while any item
above lacks source, test, gate output, and proof artifact references.

## Public Cross-References

- `project/drivers/DriverPackageManifest.csv`
- `project/drivers/adaptor/scratchbird-dbeaver-driver/README.md`
- `project/drivers/adaptor/scratchbird-dbeaver-driver/pom.xml`
- `project/drivers/adaptor/scratchbird-dbeaver-driver/plugins/org.jkiss.dbeaver.ext.scratchbird/plugin.xml`
- `project/drivers/adaptor/scratchbird-dbeaver-driver/plugins/org.jkiss.dbeaver.ext.scratchbird.ui/plugin.xml`
- `project/tools/release/dbeaver_management_platform_gate.py`
- `project/tools/release/dbeaver_management_release_contract.json`
- `project/tests/release/dbeaver_management_platform_gate_test.py`
- `project/tests/release/dbeaver_management_install_lifecycle_fixture.json`
- `project/tests/security/dbeaver_management_model_security_test.py`
- `project/tests/gui/dbeaver_management_ui_proof.py`
- `project/tests/conformance/drivers/fixtures/dbeaver_management_platform/manifest.json`
- `project/tests/conformance/drivers/corpora/dbeaver_management_platform/management_corpus.json`
