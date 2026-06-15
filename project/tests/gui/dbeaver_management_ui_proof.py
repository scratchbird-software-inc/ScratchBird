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
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
DBEAVER_ROOT = REPO_ROOT / "project/drivers/adaptor/scratchbird-dbeaver-driver"
UI_ROOT = DBEAVER_ROOT / "plugins/org.jkiss.dbeaver.ext.scratchbird.ui"
MODEL_ROOT = DBEAVER_ROOT / "plugins/org.jkiss.dbeaver.ext.scratchbird"
TEST_ROOT = DBEAVER_ROOT / "test/org.jkiss.dbeaver.ext.scratchbird.test"


def read(path: Path) -> str:
    if not path.exists():
        raise AssertionError(f"missing required proof input: {path.relative_to(REPO_ROOT)}")
    return path.read_text(encoding="utf-8")


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
    require_order(dialog, "createDataEditorContractTab(tabs)", "createDataTransferContractTab(tabs)", "contract tab order")
    require_order(dialog, "createDataTransferContractTab(tabs)", "createExecutionTab(tabs, plan)", "contract tabs before execution")
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
    require(dialog, "ScratchBirdManagementWorkflow.previewIdentity(plan)", "preview identity field")
    require(dialog, "ScratchBirdManagementWorkflow.applyGateSummary", "apply gate summary")
    require(dialog, "ScratchBirdManagementWorkflow.monitoringDashboardLines", "dashboard status tab")
    require(dialog, "ScratchBirdManagementWorkflow.objectGraphLines", "object graph status tab")
    require(dialog, "ScratchBirdManagementWorkflow.dataEditorLines", "data editor workflow tab")
    require(dialog, "ScratchBirdManagementWorkflow.dataTransferLines", "data transfer workflow tab")
    require(dialog, "ScratchBirdManagementWorkflow.accessibilityLocalizationLines", "accessibility localization proof")
    require(dialog, "ScratchBirdDataEditorContract.Operation.values()", "all data editor operations surfaced")
    require(dialog, "ScratchBirdDataTransferContract.Direction.values()", "all data transfer directions surfaced")
    require(dialog, "ScratchBirdObjectGraphContract.GraphPlan graphPlan", "object graph query contract")
    require(dialog, "UIUtils.runInProgressService(monitor ->", "DBeaver progress service")
    require(dialog, "monitor.beginTask(\"ScratchBird management status refresh\"", "combined refresh monitor")
    require(dialog, "ScratchBirdProbeHistory.recordAuthorizationProbe", "authz audit history")
    require(dialog, "ScratchBirdProbeHistory.recordLiveProbe", "live audit history")
    require(dialog, "ScratchBirdProbeHistory.recordTaskProbe", "task audit history")

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
    if 'extension point="org.jkiss.dbeaver.dashboard"' in model_xml:
        raise AssertionError("dashboard contract must not register live dashboards before server admission")
    if "sys.performance" in model_xml:
        raise AssertionError("dashboard declarations must not use stale sys.performance placeholder")

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
