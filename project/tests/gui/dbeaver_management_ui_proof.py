#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import re
import sys
import os
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
DBEAVER_ROOT = REPO_ROOT / "project/drivers/adaptor/scratchbird-dbeaver-driver"
UI_ROOT = DBEAVER_ROOT / "plugins/org.jkiss.dbeaver.ext.scratchbird.ui"
MODEL_ROOT = DBEAVER_ROOT / "plugins/org.jkiss.dbeaver.ext.scratchbird"
TEST_ROOT = DBEAVER_ROOT / "test/org.jkiss.dbeaver.ext.scratchbird.test"


def io_path(path: Path) -> str:
    if os.name != "nt":
        return str(path)
    absolute = str(path.absolute())
    if absolute.startswith("\\\\?\\"):
        return absolute
    return "\\\\?\\" + absolute


def path_exists(path: Path) -> bool:
    if os.name != "nt":
        return path.exists()
    return os.path.exists(io_path(path))


def read(path: Path) -> str:
    if not path_exists(path):
        raise AssertionError(f"missing required proof input: {path.relative_to(REPO_ROOT)}")
    with open(io_path(path), encoding="utf-8") as handle:
        return handle.read()


def require(source: str, needle: str, label: str) -> None:
    if needle not in source:
        raise AssertionError(f"{label} missing {needle!r}")


def require_java_string(source: str, value: str, label: str) -> None:
    if value not in source and value.replace('"', '\\"') not in source:
        raise AssertionError(f"{label} missing {value!r}")


def require_regex(source: str, pattern: str, label: str) -> None:
    if re.search(pattern, source, re.MULTILINE | re.DOTALL) is None:
        raise AssertionError(f"{label} missing pattern {pattern!r}")


def require_order(source: str, left: str, right: str, label: str) -> None:
    left_index = source.find(left)
    right_index = source.find(right)
    if left_index < 0 or right_index < 0 or left_index >= right_index:
        raise AssertionError(f"{label} expected {left!r} before {right!r}")


def main() -> int:
    dialog = read(UI_ROOT / "src/org/jkiss/dbeaver/ext/scratchbird/ui/ScratchBirdManagementDialog.java")
    management_editor = read(UI_ROOT / "src/org/jkiss/dbeaver/ext/scratchbird/ui/ScratchBirdManagementEditor.java")
    management_editor_input = read(UI_ROOT / "src/org/jkiss/dbeaver/ext/scratchbird/ui/ScratchBirdManagementEditorInput.java")
    navigator_object_manager = read(
        UI_ROOT / "src/org/jkiss/dbeaver/ext/scratchbird/ui/ScratchBirdNavigatorObjectManagerAdapterFactory.java"
    )
    navigator_command_handler = read(
        UI_ROOT / "src/org/jkiss/dbeaver/ext/scratchbird/ui/handlers/ScratchBirdNavigatorCommandHandler.java"
    )
    selection_utils = read(UI_ROOT / "src/org/jkiss/dbeaver/ext/scratchbird/ui/ScratchBirdSelectionUtils.java")
    workflow = read(UI_ROOT / "src/org/jkiss/dbeaver/ext/scratchbird/ui/ScratchBirdManagementWorkflow.java")
    ui_xml = read(UI_ROOT / "plugin.xml")
    ui_manifest = read(UI_ROOT / "META-INF/MANIFEST.MF")
    ui_l10n = read(UI_ROOT / "OSGI-INF/l10n/bundle.properties")
    model_xml = read(MODEL_ROOT / "plugin.xml")
    cmake = read(REPO_ROOT / "project/tests/gui/CMakeLists.txt")
    integration_test = read(
        TEST_ROOT / "src/org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdIntegrationTest.java"
    )
    manual_qa = read(REPO_ROOT / "project/tests/gui/dbeaver_management_manual_qa.md")
    data_editor_contract = read(
        MODEL_ROOT
        / "src/org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdDataEditorContract.java"
    )
    data_transfer_contract = read(
        MODEL_ROOT
        / "src/org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdDataTransferContract.java"
    )
    object_graph_contract = read(
        MODEL_ROOT
        / "src/org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdObjectGraphContract.java"
    )
    report_catalog = read(
        MODEL_ROOT
        / "src/org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdReportCatalog.java"
    )
    live_probe = read(
        MODEL_ROOT
        / "src/org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdLiveProbe.java"
    )
    management_surface_catalog = read(
        MODEL_ROOT
        / "src/org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdManagementSurfaceCatalog.java"
    )
    management_console_catalog = read(
        MODEL_ROOT
        / "src/org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdManagementConsoleCatalog.java"
    )
    sql_object_editor_catalog = read(
        MODEL_ROOT
        / "src/org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdSqlObjectEditorCatalog.java"
    )
    agent_management_catalog = read(
        MODEL_ROOT
        / "src/org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdAgentManagementCatalog.java"
    )
    agent_manifest = read(REPO_ROOT / "project/src/core/agents/agent_runtime_manifest.def")

    for slice_id in (
        "DBEAVER-MGMT-015",
        "DBEAVER-MGMT-016",
        "DBEAVER-MGMT-017",
        "DBEAVER-MGMT-018",
        "DBEAVER-MGMT-020",
        "DBEAVER-MGMT-022",
        "DBEAVER-MGMT-032",
        "DBEAVER-MGMT-035",
        "DBEAVER-MGMT-038",
        "DBEAVER-MGMT-039",
        "DBEAVER-MGMT-040",
        "DBEAVER-MGMT-041",
        "DBEAVER-MGMT-042",
        "DBEAVER-MGMT-045",
    ):
        require(workflow, slice_id, "workflow slice inventory")

    for slice_id in (
        "DBEAVER-MGMT-015",
        "DBEAVER-MGMT-032",
        "DBEAVER-MGMT-038",
        "DBEAVER-MGMT-039",
        "DBEAVER-MGMT-040",
        "DBEAVER-MGMT-041",
        "DBEAVER-MGMT-042",
        "DBEAVER-MGMT-045",
    ):
        require(cmake, slice_id, "GUI proof CTest label")

    require_order(dialog, "createWorkflowTab(tabs)", "createExecutionTab(tabs, plan)", "workflow tab order")
    require_order(dialog, "createManagementConsoleTab(tabs, managementSurface, managementConsole)", "createOverviewTab(tabs, permission)", "management console tab order")
    require_order(dialog, "createDataEditorContractTab(tabs)", "createDataTransferContractTab(tabs)", "contract tab order")
    require_order(dialog, "createDataTransferContractTab(tabs)", "createExecutionTab(tabs, plan)", "contract tabs before execution")
    require(dialog, "ScrolledComposite", "scrollable management form")
    require(dialog, "new ScrolledComposite(area, SWT.V_SCROLL | SWT.H_SCROLL)", "management form scrollbars")
    require(dialog, "setAccessibleName(scroll, \"ScratchBird management form scroll area\")", "management form scroll accessibility")
    require(dialog, "configureDialogScroll(scroll, tabs)", "management form scroll sizing")
    require(dialog, "scroll.setMinSize", "management form min scroll size")
    require(dialog, "protected boolean isResizable()", "resizable management dialog")
    require(dialog, "protected Point getInitialSize()", "responsive management dialog size")
    require(dialog, "configureMinimumShellSize", "dialog minimum shell size")
    require(dialog, "shell.setMinimumSize(MIN_DIALOG_WIDTH, MIN_DIALOG_HEIGHT)", "dialog cannot shrink below button row")
    require(dialog, "createEmbeddedEditorArea", "embedded DBeaver editor form")
    require(dialog, "createEmbeddedButtonBar", "embedded editor dynamic button row")
    require(dialog, "new GridLayout(7, true)", "embedded buttons share available width")
    require(dialog, "data.minimumWidth = 118", "embedded button minimum width")
    require(dialog, "createPlainTabFolder(area)", "management console uses bounded editor tab folder")
    require(dialog, "setTitle(managementConsole.title())", "management console title")
    require(dialog, "ScratchBird management console for", "management console message")
    require(dialog, "tabs.setSelection(0)", "dedicated first tab selected")
    require(dialog, "return area;", "management console does not fall through to generic proof tabs")
    require(dialog, "createManagementSurfaceTab", "management surface tab")
    require(dialog, "createManagementConsoleTab", "management console tab")
    require(dialog, "SashForm(container, SWT.VERTICAL)", "management console split pane")
    require(dialog, "createManagementConsoleDetailTabs", "management console detail tabs")
    require(dialog, "addTallFieldControl", "management console tall detail fields")
    require(dialog, "addResultTable", "management console result tables")
    require(dialog, "management-console-result-table", "management console result table proof id")
    require(dialog, "populateManagementResultTable", "management console server result population")
    require(dialog, "findManagementResult", "management console server result lookup")
    require(dialog, "filteredRows", "management console selected row filtering")
    require(dialog, "autoRefreshManagementConsole", "management console automatic server refresh")
    require(dialog, "ScratchBirdManagementConsoleCatalog.forSurface", "management console lookup")
    require(dialog, "createSqlObjectEditorTab", "SQL object editor tab")
    require(dialog, "ScratchBirdSqlObjectEditorCatalog.forForm", "SQL object editor lookup")
    require(dialog, "sql-object-editor-draft-sbsql", "SQL object editor editable draft")
    require(dialog, "currentCommandText", "draft-aware command text")
    require(dialog, "activeExecutionPlan", "draft-aware execution plan")
    require(dialog, "activeAuthzProbePlan", "draft-aware authorization probe")
    require(dialog, "currentPreviewHash", "draft-aware preview hash")
    require(dialog, "objectEditorValidationSummary", "SQL object editor validation summary")
    require(dialog, "lifecyclePreview", "SQL object editor lifecycle preview")
    require(dialog, "management-console-table", "management console proof table")
    require(dialog, "SWT.COLOR_DARK_GRAY", "disabled management rows remain visible")
    require(dialog, "managementConsoleSelectedDetail", "management console selected detail")
    require(dialog, "managementConsoleLiveSummary", "management console live result")
    require(dialog, "All mutating operations require ScratchBird server admission", "management mutation admission boundary")
    require(dialog, "createMonitoringTab(tabs)", "monitoring tab")
    require(dialog, "createObjectGraphTab(tabs)", "object graph tab")
    require(dialog, "createDataEditorContractTab(tabs)", "data editor contract tab")
    require(dialog, "createDataTransferContractTab(tabs)", "data transfer contract tab")
    require(dialog, "Validate Preview", "validate preview button")
    require(dialog, "Refresh Server Status", "refresh status button")
    require(workflow, "Apply Requires Admission", "apply gate fallback label")
    require(workflow, "Apply Admitted Preview", "apply admitted button label")
    require(dialog, "applyAfterAdmission", "server-admitted apply handler")
    require(dialog, "ScratchBirdMutationApplyExecutor.apply", "guarded mutation apply executor")
    require(dialog, "ScratchBirdProbeHistory.recordApply", "apply audit history")
    require(dialog, "ScratchBirdManagementWorkflow.previewIdentity(activeExecutionPlan())", "preview identity field")
    require(dialog, "ScratchBirdManagementWorkflow.applyGateSummary", "apply gate summary")
    require(dialog, "ScratchBirdManagementWorkflow.monitoringDashboardLines", "dashboard status tab")
    require(workflow, "management -> sys.catalog_readable.navigator_tree", "management root dashboard status")
    require(workflow, "management.diagnostics -> sys.catalog_readable.metrics_catalog", "management diagnostics dashboard status")
    require(dialog, "ScratchBirdManagementWorkflow.objectGraphLines", "object graph status tab")
    require(dialog, "ScratchBirdManagementWorkflow.dataEditorLines", "data editor workflow tab")
    require(dialog, "ScratchBirdManagementWorkflow.dataTransferLines", "data transfer workflow tab")
    require(dialog, "ScratchBirdManagementWorkflow.accessibilityLocalizationLines", "accessibility localization proof")
    require(dialog, "ScratchBirdDataEditorContract.Operation.values()", "all data editor operations surfaced")
    require(dialog, "ScratchBirdDataTransferContract.Direction.values()", "all data transfer directions surfaced")
    require(dialog, "ScratchBirdObjectGraphContract.GraphPlan graphPlan", "object graph query contract")
    require(dialog, "managementDataAvailability", "management data availability summary")
    require(dialog, "server-published data surface", "management user-facing data availability wording")
    require(dialog, "ScratchBirdLiveProbe.planForManagementSurface", "management live source probe")
    require(dialog, "managementPresentation", "management display-type presentation")
    require(dialog, "ScratchBirdManagementSurfaceCatalog.find", "management surface lookup")
    require(dialog, "This is a ScratchBird management surface, not a physical schema", "management schema boundary text")
    require(dialog, "UIUtils.runInProgressService(monitor ->", "DBeaver progress service")
    require(dialog, "monitor.beginTask(\"ScratchBird management status refresh\"", "combined refresh monitor")
    require(dialog, "ScratchBirdProbeHistory.recordAuthorizationProbe", "authz audit history")
    require(dialog, "ScratchBirdProbeHistory.recordLiveProbe", "live audit history")
    require(dialog, "ScratchBirdProbeHistory.recordTaskProbe", "task audit history")
    require(dialog, "managementConsoleAgentNameText", "selected agent field")
    require(dialog, "managementConsoleAgentStateText", "selected agent runtime state field")
    require(dialog, "managementConsoleAgentHealthText", "selected agent health field")
    require(dialog, "managementConsoleAgentActivePolicyText", "selected agent active policy field")
    require(dialog, "managementConsoleAgentLastActivityText", "selected agent activity field")
    require(dialog, "refreshManagementConsoleAgentFields", "selected agent field hydration")
    require(dialog, "firstManagementResultRow", "selected agent result row lookup")
    require(dialog, "table.select(0)", "visible result rows auto-selected")
    require(dialog, "Runtime status", "management runtime panel")
    require(dialog, "Policy summary", "management policy summary panel")
    require(dialog, "management-policy-document-selector", "management policy document selector")
    require(dialog, "managementConsolePolicySelector", "management policy selector")
    require(dialog, "managementConsolePolicySourceStatusText", "management policy source status")
    require(dialog, "managementConsoleOverrideSourceStatusText", "management policy override source status")
    require(dialog, "Schedule / override", "management policy schedule panel")
    require(dialog, "managementConsolePolicyNameText", "management policy editor name")
    require(dialog, "managementConsolePolicyBodyText", "management policy body editor")
    require(dialog, "defaultPolicyDocument", "management policy document draft")
    require(dialog, "policySourceStatus", "management policy source status helper")
    require(dialog, "overrideSourceStatus", "management policy override source status helper")
    require(dialog, "policyMutationCommand", "branch-aware management policy command")
    require(dialog, "ALTER MANAGEMENT", "non-agent management policy command")
    require(dialog, "refreshManagementConsolePolicyEditor", "management policy editor hydration")
    require(dialog, "New Draft", "management policy new draft button")
    require(dialog, "Validate Policy", "management policy validate button")
    require(dialog, "Simulate Policy", "management policy simulate button")
    require(dialog, "Set Active", "management policy set active button")
    require(dialog, "Apply Policy", "management policy apply button")
    require(dialog, "ALTER AGENT", "management policy mutation command")
    require(dialog, "case \"ATTACH\"", "management policy attach command")
    require(dialog, "case \"VALIDATE\"", "management policy validate command")
    require(dialog, "case \"SIMULATE\"", "management policy simulate command")
    require(dialog, "case \"APPLY\"", "management policy apply command")
    require(dialog, "Action history", "management action history panel")
    require(dialog, "Evidence and audit", "management evidence audit panel")
    require(dialog, "Admissible actions", "management action panel")
    require(dialog, "tableData.heightHint = 220", "management inventory bounded table height")
    require(dialog, "tableData.minimumHeight = 120", "management inventory table minimum")
    require(dialog, "sash.setWeights(new int[]{34, 66})", "management inventory detail split weights")
    require(dialog, "Rows are displayed in Runtime, Actions, and Evidence tables; policies are opened in the policy document editor.", "management user-facing result status")
    require(dialog, "agentSourceQuery", "agent source query helper")
    require(dialog, "sys.frontend.agent_audit", "agent audit source")

    require(workflow, "preview_sha256", "preview identity")
    require(workflow, "applied_operation_sha256=requires_server_admission", "no unadmitted applied claim")
    require(workflow, "command_sha256", "command hash binding")
    require(workflow, "PENDING_SERVER_ADMISSION", "pending server admission state")
    require(workflow, "REFUSED_CLIENT_GATED", "client refusal state")
    require(workflow, "REFUSED_BY_SERVER", "server refusal state")
    require(workflow, "READY_TO_APPLY", "server-admitted apply-ready state")
    require(workflow, "APPLIED_SERVER_VALIDATED", "server-validated apply state")
    require(workflow, "REFUSED_APPLY", "apply refusal state")
    require(workflow, "VERIFY_APPLY_REFRESHED", "post-apply refresh verify state")
    require_regex(workflow, r"static boolean applyButtonEnabled\([^)]*ScratchBirdAdminExecutor\.ExecutionPlan", "apply enablement is admission aware")
    require(workflow, "ScratchBirdMutationApplyExecutor.applyReadiness", "apply readiness uses mutation executor")
    require(workflow, "RUN_IN_PROGRESS_SERVICE", "long operation progress state")
    require(workflow, "CANCEL_SAFE", "cancel state")
    require(workflow, "TIMEOUT_REFUSAL_VISIBLE", "timeout refusal state")
    require(workflow, "RECONNECT_STATE_VISIBLE", "reconnect state")
    require(workflow, "PARTIAL_FAILURE_AUDITED", "partial failure state")
    require(workflow, "UI_THREAD_SAFE_BACKGROUND", "UI thread state")
    require(workflow, "SAFE_RETRY_BOUNDARY", "safe retry state")
    require(workflow, "DATA_EDITOR_CONTRACT_READY", "data editor ready state")
    require(workflow, "DATA_EDITOR_SERVER_REVALIDATION_REQUIRED", "data editor admission state")
    require(workflow, "DATA_EDITOR_TRANSACTION_AUTHORITY", "data editor transaction state")
    require(workflow, "DATA_EDITOR_TYPE_HANDLING", "data editor type state")
    require(workflow, "DATA_TRANSFER_CONTRACT_READY", "data transfer ready state")
    require(workflow, "DATA_TRANSFER_SERVER_REVALIDATION_REQUIRED", "data transfer admission state")
    require(workflow, "DATA_TRANSFER_ENCODING_BOUNDARY", "data transfer encoding state")
    require(workflow, "DATA_TRANSFER_BATCHING_BOUNDARY", "data transfer batching state")
    require(workflow, "DATA_TRANSFER_RESULT_PARITY", "data transfer result state")
    require(workflow, "ACCESSIBILITY_PROOF_READY", "accessibility proof state")
    require(workflow, "LOCALIZATION_BUNDLE_READY", "localization proof state")

    require(dialog, "AccessibleAdapter", "SWT accessibility import")
    require(dialog, "setAccessibleName", "accessible name helper")
    require(dialog, "ACCESSIBLE_NAME_KEY", "accessible name data")
    require(dialog, "PROOF_DATA_KEY", "proof data key")
    require(dialog, "selected-history-record", "history proof id")
    require(dialog, "selected-task", "task proof id")
    require(ui_manifest, "Bundle-Localization: OSGI-INF/l10n/bundle", "UI bundle localization")
    require(ui_xml, 'name="%command.scratchbird.open.name"', "localized open command")
    require(ui_xml, 'extension point="org.eclipse.ui.editors"', "DBeaver editor extension")
    require(ui_xml, 'id="org.jkiss.dbeaver.ext.scratchbird.ui.managementEditor"', "ScratchBird management editor id")
    require(ui_xml, 'class="org.jkiss.dbeaver.ext.scratchbird.ui.ScratchBirdManagementEditor"', "ScratchBird management editor class")
    require(ui_xml, 'commandId="org.jkiss.dbeaver.core.object.open"', "DBeaver default open command override")
    require(ui_xml, "canDefaultOpen", "ScratchBird default-open guard")
    require(ui_xml, "ScratchBirdOpenHandler", "ScratchBird default-open handler")
    require(ui_xml, 'extension point="org.eclipse.core.runtime.adapters"', "DBeaver navigator adapter extension")
    require(ui_xml, "ScratchBirdNavigatorObjectManagerAdapterFactory", "ScratchBird navigator object manager adapter")
    require(ui_xml, "org.jkiss.dbeaver.ui.navigator.INavigatorObjectManager", "DBeaver navigator object manager type")
    require(navigator_object_manager, "implements IAdapterFactory", "navigator object manager adapter")
    require(navigator_object_manager, "INavigatorObjectManager", "navigator object manager contract")
    require(navigator_object_manager, "openObjectEditor", "navigator open editor interception")
    require(navigator_object_manager, "ScratchBirdManagementEditor.openOrDialog", "navigator open uses embedded editor")
    require(navigator_object_manager, "ScratchBirdNavigatorActionRegistry.Action.OPEN", "navigator open action")
    require(navigator_object_manager, "ScratchBirdSelectionUtils.supportsDefaultOpen", "navigator open guard")
    require(navigator_object_manager, "return FEATURE_OPEN", "navigator open-only feature")
    require(navigator_command_handler, "ScratchBirdManagementEditor.openOrDialog", "context command uses embedded editor")
    require(management_editor, "extends EditorPart", "ScratchBird management editor part")
    require(management_editor, "public static final String EDITOR_ID", "ScratchBird management editor id constant")
    require(management_editor, "page.openEditor", "ScratchBird management opens in workbench editor area")
    require(management_editor, "form.createEmbeddedEditorArea(parent)", "ScratchBird editor embeds management form")
    require(management_editor_input, "implements IEditorInput", "ScratchBird management editor input")
    require(management_editor_input, "getPersistable()", "ScratchBird editor input is not stale workspace state")
    require(selection_utils, "action == ScratchBirdNavigatorActionRegistry.Action.OPEN", "open action default-open policy")
    require(selection_utils, "return supportsDefaultOpen(element);", "open action fallback to default-open guard")
    require(ui_xml, 'label="%menu.scratchbird.label"', "localized ScratchBird menu")
    require(ui_l10n, "command.scratchbird.validateSql.name=Validate ScratchBird v3 SQL", "localized validate command fallback")
    require(ui_l10n, "command.scratchbird.sourceStatus.description=Inspect ScratchBird form/report source-surface status.", "localized source status fallback")

    for dashboard_id, query in (
        ("scratchbird.sessions", 'SELECT COUNT(*) AS "Sessions" FROM sys.sessions'),
        ("scratchbird.transactions", 'SELECT COUNT(*) AS "Transactions" FROM sys.transactions'),
        ("scratchbird.locks", 'SELECT COUNT(*) AS "Locks" FROM sys.locks'),
        ("scratchbird.performance", "SHOW METRICS"),
    ):
        require(report_catalog, dashboard_id, "dashboard contract declaration")
        require_java_string(report_catalog, query, "dashboard contract query")
        require(model_xml, dashboard_id, "registered dashboard declaration")
        require(model_xml, query, "registered dashboard query")
    require(model_xml, 'extension point="org.jkiss.dbeaver.dashboard"', "DBeaver dashboard extension point")
    if "sys.performance" in model_xml:
        raise AssertionError("dashboard declarations must not use stale sys.performance placeholder")

    for token in (
        'ROOT_PATH = "management"',
        'REPORT_BASE_PATH = ROOT_PATH + ".diagnostics"',
        'ROOT_PATH + ".security"',
        'ROOT_PATH + ".parser-and-language"',
        'ROOT_PATH + ".listener-and-manager"',
        'sys.catalog_readable.navigator_tree',
        'sys.parser.dialects',
        'sys.frontend.agents',
        'refusalBehavior',
        'displayType',
        'backingSources',
    ):
        require(management_surface_catalog, token, "management surface catalog")

    for token in (
        "ConsoleDefinition",
        "ConsoleRow",
        'case "management.agents"',
        "Agent Runtime Console",
        "Disabled agents remain visible and selectable",
        "Runtime state, schedule, policy, and action history must come from live server sources",
        "sys.frontend.agents",
        "sys.frontend.agent_policies",
        "sys.frontend.agent_actions",
        "sys.frontend.agent_metric_dependencies",
        "sys.frontend.agent_overrides",
        "sys.frontend.agent_evidence",
        "sys.frontend.agent_audit",
        "management.parser-and-language",
        "management.listener-and-manager",
        "management.file-spaces",
    ):
        require(management_console_catalog, token, "management console catalog")

    manifest_agent_ids = re.findall(r"^SB_AGENT_MANIFEST_ENTRY\(([^,]+),", agent_manifest, re.MULTILINE)
    if len(manifest_agent_ids) != 29:
        raise AssertionError(f"canonical engine agent manifest expected 29 entries, saw {len(manifest_agent_ids)}")
    catalog_agent_ids = re.findall(r'agent\("([^"]+)"', agent_management_catalog)
    if catalog_agent_ids != manifest_agent_ids:
        raise AssertionError(
            "DBeaver agent management catalog drifted from engine manifest: "
            f"engine={manifest_agent_ids!r} ui={catalog_agent_ids!r}"
        )
    require(agent_management_catalog, "enabledByDefault", "agent enabled status")
    require(agent_management_catalog, "disabled until cluster provider", "cluster-disabled status")
    require(agent_management_catalog, "SELECT * FROM sys.frontend.agents WHERE agent_type_id", "agent source query")
    require(agent_management_catalog, "SELECT * FROM sys.frontend.agent_policies WHERE agent_name", "agent policy query")
    require(agent_management_catalog, "SELECT * FROM sys.frontend.agent_actions WHERE agent_name", "agent action query")
    require(agent_management_catalog, "SELECT * FROM sys.frontend.agent_metric_dependencies WHERE agent_name", "agent metric dependency query")
    require(agent_management_catalog, "SELECT * FROM sys.frontend.agent_overrides WHERE target_name", "agent override query")
    require(agent_management_catalog, "SELECT * FROM sys.frontend.agent_evidence WHERE agent_name", "agent evidence query")
    require(agent_management_catalog, "SELECT * FROM sys.frontend.agent_audit", "agent audit query")

    for form_id in (
        "SBDV-FRM-001",
        "SBDV-FRM-601",
        "SBDV-FRM-602",
        "SBDV-FRM-603",
        "SBDV-FRM-604",
        "SBDV-FRM-605",
        "SBDV-FRM-606",
        "SBDV-FRM-607",
        "SBDV-FRM-608",
        "SBDV-FRM-609",
        "SBDV-FRM-610",
        "SBDV-FRM-611",
        "SBDV-FRM-612",
        "SBDV-FRM-110",
    ):
        require(sql_object_editor_catalog, form_id, "SQL object editor form coverage")
    for token in (
        "EditorDefinition",
        "Recursive Namespace Editor",
        "Relational Table Editor",
        "Routine Editor",
        "Domain And Datatype Editor",
        "Security Grant And Ownership Editor",
        "CREATE TABLE",
        "ALTER TABLE",
        "DROP TABLE",
        "CREATE DOMAIN",
        "CREATE FUNCTION",
        "CREATE PROCEDURE",
        "CREATE TRIGGER",
        "GRANT",
        "REVOKE",
        "sys.catalog.object_resolver",
        "sys.catalog_readable.privileges",
        "sys.security.permission_probe",
        "recursive schema paths",
        "exact draft SQL hash",
        "SBLR/UUID outside the engine",
    ):
        require(sql_object_editor_catalog, token, "SQL object editor catalog")

    require(live_probe, "planForManagementSurface", "management surface live probe")
    require(live_probe, "surface.backingSources()", "management surface source enumeration")
    require(live_probe, "commandForSource", "management surface source command builder")
    require(live_probe, "SELECT * FROM ", "management surface sys view probe")
    require(live_probe, 'startsWith("SHOW ")', "management surface SHOW probe")
    require(live_probe, "MAX_SAMPLE_ROWS = 64", "management result sample large enough for canonical agents")

    require(ui_xml, "org.jkiss.dbeaver.ext.scratchbird.ui.validateSql", "SQL validation command")
    require(ui_xml, "org.jkiss.dbeaver.ext.scratchbird.ui.sourceStatus", "source status command")
    require(ui_xml, "ScratchBirdUiPropertyTester", "property tester")
    require(integration_test, "ScratchBirdManagementWorkflow.java", "plugin test workflow source assertion")
    require(integration_test, "Apply Requires Admission", "plugin test apply refusal assertion")
    require(manual_qa, "DBEAVER-MGMT-035-MANUAL-QA-CHECKLIST", "manual QA search key")
    require(manual_qa, "connection wizard", "manual QA connection wizard item")
    require(manual_qa, "Workflow", "manual QA workflow screenshot item")
    require(manual_qa, "Apply Requires Admission", "manual QA apply refusal item")
    require(manual_qa, "accessible labels", "manual QA accessibility item")
    require(manual_qa, "install removal", "manual QA uninstall item")

    for operation in ("INSERT", "UPDATE", "DELETE", "REFRESH"):
        require(data_editor_contract, operation, "data editor operation inventory")
    require(data_editor_contract, "SBDV-DATA-EDITOR", "data editor server admission form")
    require(data_editor_contract, "Autocommit is only a compatibility profile", "data editor autocommit boundary")
    require(data_editor_contract, "Savepoint support is delegated", "data editor savepoint boundary")
    require(data_editor_contract, "ScratchBirdValueProfile", "data editor type profile")
    require(data_editor_contract, "server before mutation", "data editor mutation revalidation")

    for direction in ("IMPORT", "EXPORT"):
        require(data_transfer_contract, direction, "data transfer direction inventory")
    require(data_transfer_contract, "SBDV-DATA-TRANSFER", "data transfer server admission form")
    require(data_transfer_contract, "UTF-8", "data transfer default encoding")
    require(data_transfer_contract, "Language resource hashes", "data transfer language resource boundary")
    require(data_transfer_contract, "ambiguous result requires server recovery/audit proof", "data transfer partial result boundary")

    require(object_graph_contract, "sys.catalog.object_dependencies", "object graph dependency query")
    require(object_graph_contract, "sys.catalog.object_resolver", "object graph search query")
    require(object_graph_contract, "sys.catalog.generated_ddl", "object graph DDL query")
    require(object_graph_contract, "sys.catalog.generated_sbsql", "object graph SBsql query")
    require(object_graph_contract, "EXPLAIN ", "object graph explain query")
    require(object_graph_contract, "Hidden objects must not appear", "object graph authorization filter")

    forbidden_dialog_claims = (
        "Apply completed",
        "Applied successfully",
        "Mutation applied",
        "Verify complete after apply",
    )
    for claim in forbidden_dialog_claims:
        if claim in dialog or claim in workflow:
            raise AssertionError(f"preview-only UI must not claim applied work: {claim}")

    print("dbeaver_management_ui_proof ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
