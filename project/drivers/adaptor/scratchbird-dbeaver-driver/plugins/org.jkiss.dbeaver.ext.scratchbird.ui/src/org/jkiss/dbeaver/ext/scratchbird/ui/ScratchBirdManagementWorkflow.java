// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * DBeaver - Universal Database Manager
 * Copyright (C) 2010-2026 DBeaver Corp and others
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package org.jkiss.dbeaver.ext.scratchbird.ui;

import org.jkiss.code.NotNull;
import org.jkiss.code.Nullable;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdAdminExecutor;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdDataEditorContract;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdDataTransferContract;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdDestructivePlan;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdFormDefinition;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdFormMode;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdLiveProbe;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdManagementActionEnvelope;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdMutationApplyExecutor;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdObjectGraphContract;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdProbeHistory;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdRefusalModel;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdReportDefinition;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdTaskDefinition;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HexFormat;
import java.util.List;

final class ScratchBirdManagementWorkflow {

    static final List<String> WORKPLAN_SLICES = List.of(
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
        "DBEAVER-MGMT-045");

    static final String PROOF_DATA_KEY = "org.jkiss.dbeaver.ext.scratchbird.ui.proofId";
    static final String ACCESSIBLE_NAME_KEY = "org.jkiss.dbeaver.ext.scratchbird.ui.accessibleName";

    private ScratchBirdManagementWorkflow() {
    }

    @NotNull
    static String previewHash(@NotNull String commandText) {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            return HexFormat.of().formatHex(digest.digest(commandText.getBytes(StandardCharsets.UTF_8)));
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException("SHA-256 digest is required by Java", e);
        }
    }

    @NotNull
    static String previewIdentity(@NotNull ScratchBirdAdminExecutor.ExecutionPlan plan) {
        return "preview_sha256=" + previewHash(plan.commandText()) +
            "\nmode=" + plan.mode().name() +
            "\ntarget=" + plan.targetPath() +
            "\nform=" + plan.form().id() +
            "\ncommand_sha256=" + ScratchBirdManagementActionEnvelope.commandHashFor(plan) +
            "\napplied_operation_sha256=requires_server_admission";
    }

    @NotNull
    static String workflowStatus(
        @NotNull ScratchBirdAdminExecutor.ExecutionPlan plan,
        @NotNull ScratchBirdRefusalModel permission,
        @Nullable ScratchBirdLiveProbe.ProbeResult authzResult,
        @Nullable ScratchBirdLiveProbe.ProbeResult liveResult,
        @Nullable ScratchBirdLiveProbe.ProbeResult taskResult
    ) {
        if (permission.isDeterministicRefusal()) {
            return "REFUSED_CLIENT_GATED: " + permission.kind() + ": " + permission.message();
        }
        if (isMutationMode(plan.mode()) && authzResult == null) {
            return "PENDING_SERVER_ADMISSION: validate and refresh may run, but apply remains disabled for preview_sha256=" +
                previewHash(plan.commandText()) + ".";
        }
        if (authzResult != null && authzResult.status().isDeterministicRefusal()) {
            return "REFUSED_BY_SERVER: " + authzResult.status().kind() + ": " + authzResult.status().message();
        }
        if (taskResult != null && taskResult.status().isDeterministicRefusal()) {
            return "TASK_REFUSED: " + taskResult.status().kind() + ": " + taskResult.status().message();
        }
        if (liveResult != null && liveResult.status().isDeterministicRefusal()) {
            return "REFRESH_REFUSED: " + liveResult.status().kind() + ": " + liveResult.status().message();
        }
        if (isMutationMode(plan.mode())) {
            return "SERVER_STATUS_CAPTURED: authorization evidence exists; apply is enabled only when sys.security.permission_probe admits the exact preview and command hash.";
        }
        return "READ_ONLY_STATUS_CAPTURED: inspect/report refresh can verify live server state without mutation apply.";
    }

    @NotNull
    static String validationSummary(
        @NotNull List<String> diagnostics,
        @NotNull List<String> statementInventory,
        @NotNull List<String> formHints
    ) {
        return "LOCAL_VALIDATION_READY: Java v3 parser diagnostics are advisory and do not grant apply authority." +
            "\nStatements: " + summarize(statementInventory) +
            "\nDiagnostics: " + summarize(diagnostics) +
            "\nForm hints: " + summarize(formHints);
    }

    @NotNull
    static String applyGateSummary(
        @NotNull ScratchBirdAdminExecutor.ExecutionPlan plan,
        @NotNull ScratchBirdRefusalModel permission,
        @Nullable ScratchBirdLiveProbe.ProbeResult authzResult,
        @Nullable ScratchBirdMutationApplyExecutor.ApplyResult applyResult,
        @NotNull String previewHash,
        @NotNull String commandHash
    ) {
        if (applyResult != null) {
            if (applyResult.applied()) {
                return "APPLIED_SERVER_VALIDATED: preview_sha256=" + previewHash +
                    " command_sha256=" + commandHash +
                    " applied_operation_sha256=" + applyResult.appliedOperationHash() +
                    ". Refresh/verify must read server truth after apply.";
            }
            return "REFUSED_APPLY: " + applyResult.status().kind() + ": " + applyResult.status().message();
        }
        if (!isMutationMode(plan.mode())) {
            return "NOT_APPLICABLE: inspect, report, and read-only flows have no mutation apply step.";
        }
        if (permission.isDeterministicRefusal()) {
            return "REFUSED_CLIENT_GATED: " + permission.kind() + ": " + permission.message();
        }
        if (!plan.executable()) {
            return "REFUSED_NO_EXECUTABLE_PREVIEW: this form has no executable mutation preview.";
        }
        if (authzResult == null) {
            return "PENDING_SERVER_ADMISSION: apply is disabled until the server returns explicit admission for preview_sha256=" +
                previewHash + " command_sha256=" + commandHash + ".";
        }
        if (authzResult.status().isDeterministicRefusal()) {
            return "REFUSED_BY_SERVER: " + authzResult.status().kind() + ": " + authzResult.status().message();
        }
        ScratchBirdRefusalModel readiness = ScratchBirdMutationApplyExecutor.applyReadiness(
            plan,
            authzResult,
            previewHash,
            commandHash);
        if (!readiness.isAdmitted()) {
            return "PENDING_SERVER_ADMISSION: " + readiness.kind() + ": " + readiness.message();
        }
        return "READY_TO_APPLY: server permission probe admitted preview_sha256=" + previewHash +
            " command_sha256=" + commandHash + "; execution still goes through server-revalidated JDBC/SBsql.";
    }

    static boolean applyButtonEnabled(
        @NotNull ScratchBirdAdminExecutor.ExecutionPlan plan,
        @NotNull ScratchBirdRefusalModel permission,
        @Nullable ScratchBirdLiveProbe.ProbeResult authzResult,
        @Nullable ScratchBirdMutationApplyExecutor.ApplyResult applyResult,
        @NotNull String previewHash,
        @NotNull String commandHash
    ) {
        if (applyResult != null && applyResult.applied()) {
            return false;
        }
        if (permission.isDeterministicRefusal()) {
            return false;
        }
        return ScratchBirdMutationApplyExecutor.applyReadiness(plan, authzResult, previewHash, commandHash).isAdmitted();
    }

    @NotNull
    static String applyButtonLabel(boolean readyToApply) {
        return readyToApply ? "Apply Admitted Preview" : "Apply Requires Admission";
    }

    @NotNull
    static String refreshStatusSummary(
        @NotNull ScratchBirdLiveProbe.ProbePlan authzPlan,
        @NotNull ScratchBirdLiveProbe.ProbePlan livePlan,
        @Nullable ScratchBirdLiveProbe.ProbeResult authzResult,
        @Nullable ScratchBirdLiveProbe.ProbeResult liveResult
    ) {
        return "Authz plan: " + probeStatus(authzPlan, authzResult) +
            "\nLive plan: " + probeStatus(livePlan, liveResult);
    }

    @NotNull
    static String verifyStatusSummary(
        @NotNull ScratchBirdAdminExecutor.ExecutionPlan plan,
        @Nullable ScratchBirdLiveProbe.ProbeResult authzResult,
        @Nullable ScratchBirdLiveProbe.ProbeResult liveResult,
        @Nullable ScratchBirdMutationApplyExecutor.ApplyResult applyResult
    ) {
        String previewHash = previewHash(plan.commandText());
        if (applyResult != null && applyResult.applied()) {
            if (liveResult != null && liveResult.status().isAdmitted()) {
                return "VERIFY_APPLY_REFRESHED: applied_operation_sha256=" + applyResult.appliedOperationHash() +
                    " has post-apply server refresh evidence.";
            }
            return "VERIFY_PENDING_POST_APPLY_REFRESH: applied_operation_sha256=" + applyResult.appliedOperationHash() +
                " requires server refresh before verify can close.";
        }
        if (isMutationMode(plan.mode())) {
            return "VERIFY_PENDING_NO_APPLY: preview_sha256=" + previewHash +
                " has no applied operation hash; verify checks only server admission and refreshed visibility.";
        }
        if (liveResult == null && authzResult == null) {
            return "VERIFY_PENDING_REFRESH: preview_sha256=" + previewHash + " has not been refreshed against the server.";
        }
        return "VERIFY_READ_ONLY_REFRESH: preview_sha256=" + previewHash +
            " has server refresh evidence; no mutation apply is claimed.";
    }

    @NotNull
    static String refusalSummary(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdRefusalModel permission,
        @Nullable ScratchBirdReportDefinition report
    ) {
        if (permission.isDeterministicRefusal()) {
            return permission.kind() + ": " + permission.message();
        }
        if (report != null && report.futureGated()) {
            return "MISSING_SOURCE: Future-gated report source is visible only as a feature boundary.";
        }
        return "NO_REFUSAL_RECORDED: current static posture admits inspection; server execution can still refuse.";
    }

    @NotNull
    static String rollbackSummary(@Nullable ScratchBirdDestructivePlan destructivePlan) {
        if (destructivePlan == null) {
            return "NO_APPLY_NO_ROLLBACK: no mutation has been applied from this dialog.";
        }
        return String.join("\n", destructivePlan.rollbackGuidance());
    }

    @NotNull
    static String longOperationSummary() {
        return "RUN_IN_PROGRESS_SERVICE: authz, live, and task refreshes run through DBeaver progress service." +
            "\nCANCEL_SAFE: cancellation exits without marking apply or verify complete." +
            "\nTIMEOUT_REFUSAL_VISIBLE: statement timeout or reconnect failure remains visible as SERVER_REFUSED or PERMISSION_DENIED." +
            "\nRECONNECT_STATE_VISIBLE: reconnect failures keep the latest admission, refresh, task, and history states separate." +
            "\nPARTIAL_FAILURE_AUDITED: partial probe results are retained with their server diagnostic instead of being promoted as success." +
            "\nUI_THREAD_SAFE_BACKGROUND: long probes are launched through the progress service and not from direct button-thread mutation." +
            "\nSAFE_RETRY_BOUNDARY: retry is limited to read-only refresh/probe commands.";
    }

    @NotNull
    static String auditSummary(
        @NotNull String probeScopeKey,
        @NotNull String targetPath
    ) {
        return "Scope: " + targetPath +
            "\nScope key: " + probeScopeKey +
            "\nLocal history store: " + ScratchBirdProbeHistory.storeLocationText() +
            "\nAudit posture: preview, authz, live, task, and apply evidence are retained locally; server audit remains authoritative.";
    }

    @NotNull
    static String featureBoundarySummary(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdRefusalModel permission
    ) {
        return "Feature boundary source: " + form.id() +
            "\nAvailability: " + permission.kind() +
            "\nRefusal text: " + permission.message() +
            "\nRelease claim: disabled or refused surfaces are not presented as completed management features.";
    }

    @NotNull
    static List<String> monitoringDashboardLines() {
        return List.of(
            "scratchbird.sessions -> SELECT COUNT(*) AS Sessions FROM sys.sessions",
            "scratchbird.transactions -> SELECT COUNT(*) AS Transactions FROM sys.transactions",
            "scratchbird.locks -> SELECT COUNT(*) AS Locks FROM sys.locks",
            "scratchbird.performance -> SHOW METRICS",
            "Dashboard refresh failures remain server refusals; no placeholder value is promoted as current.");
    }

    @NotNull
    static List<String> dataEditorLines(@NotNull String targetPath) {
        return List.of(
            "DATA_EDITOR_CONTRACT_READY: insert, update, delete, and refresh previews are generated for " + targetPath + ".",
            "DATA_EDITOR_SERVER_REVALIDATION_REQUIRED: every generated SBsql/SBLR update route must pass sys.security.permission_probe before mutation.",
            "DATA_EDITOR_TRANSACTION_AUTHORITY: commit, rollback, autocommit, savepoint, and snapshot finality remain server/JDBC transaction authority.",
            "DATA_EDITOR_TYPE_HANDLING: ScratchBirdValueProfile and ScratchBirdValueBinding select UUID, JSONB, VECTOR, RANGE, HASH256, binary, and structured handlers.",
            "DATA_EDITOR_REFUSAL_VISIBLE: permission, type, stale-row, conflict, and refresh errors stay visible and never become an applied claim.");
    }

    @NotNull
    static List<String> dataTransferLines(@NotNull String targetPath) {
        return List.of(
            "DATA_TRANSFER_CONTRACT_READY: import and export previews are generated for " + targetPath + ".",
            "DATA_TRANSFER_SERVER_REVALIDATION_REQUIRED: transfer admission, visibility, and mutation are server-authorized through sys.security.permission_probe.",
            "DATA_TRANSFER_ENCODING_BOUNDARY: UTF-8 is the default interchange encoding and language resource hashes are advisory metadata.",
            "DATA_TRANSFER_BATCHING_BOUNDARY: batching, cancellation, retry, and ambiguous partial results require server audit/recovery proof.",
            "DATA_TRANSFER_RESULT_PARITY: import row counts and export result sets must verify against server-visible rows.");
    }

    @NotNull
    static List<String> objectGraphLines(@NotNull String targetPath) {
        ScratchBirdObjectGraphContract.GraphPlan graphPlan = ScratchBirdObjectGraphContract.plan(targetPath);
        return List.of(
            "Object search is server-authorized for visible objects under " + targetPath + ".",
            "Dependency query: " + graphPlan.dependencyQuery(),
            "Search query: " + graphPlan.searchQuery(),
            "Generated DDL query: " + graphPlan.ddlPreviewQuery(),
            "Generated SBsql query: " + graphPlan.sbsqlPreviewQuery(),
            "Explain query: " + graphPlan.explainQuery(),
            "Generated DDL and generated SBsql remain previews until live refresh confirms object identity.",
            "Explain and plan surfaces require server admission and must not disclose hidden objects.",
            "Metadata invalidation refreshes from server truth before verify; stale or deleted targets remain deterministic refusals.");
    }

    @NotNull
    static List<String> accessibilityLocalizationLines() {
        return List.of(
            "ACCESSIBILITY_PROOF_READY: dialog fields, task controls, history controls, and footer buttons carry accessible names and proof ids.",
            "KEYBOARD_NAVIGATION_READY: tab folder, buttons, read-only fields, task selector, and history selector use native SWT traversal.",
            "LOCALIZATION_BUNDLE_READY: plugin command/menu strings resolve through OSGI-INF/l10n/bundle with canonical English fallback.",
            "DIAGNOSTIC_TEXT_READY: refusal, validation, and server-diagnostic text is screen-reader friendly plain text.");
    }

    @NotNull
    static List<String> contractSummaryLines(@NotNull String targetPath) {
        return List.of(
            "DBEAVER-MGMT-039 -> " + ScratchBirdDataEditorContract.plan(
                ScratchBirdDataEditorContract.Operation.INSERT,
                targetPath).admissionProbeCommand(),
            "DBEAVER-MGMT-041 -> " + ScratchBirdDataTransferContract.plan(
                ScratchBirdDataTransferContract.Direction.EXPORT,
                targetPath).authorizationProbe(),
            "DBEAVER-MGMT-042 -> " + ScratchBirdObjectGraphContract.plan(targetPath).searchQuery());
    }

    @NotNull
    static List<String> manualQaChecklist() {
        return List.of(
            "connection wizard",
            "navigator context actions",
            "SQL editor validation and completion",
            "management workflow tab",
            "authz/live/task refusal display",
            "dashboard refresh refusal display",
            "keyboard traversal and accessible labels",
            "screenshot packet with no applied claim");
    }

    @NotNull
    static List<String> workplanSlices() {
        return WORKPLAN_SLICES;
    }

    private static boolean isMutationMode(@NotNull ScratchBirdFormMode mode) {
        return mode == ScratchBirdFormMode.CREATE ||
            mode == ScratchBirdFormMode.ALTER ||
            mode == ScratchBirdFormMode.DELETE ||
            mode == ScratchBirdFormMode.TASK;
    }

    @NotNull
    private static String probeStatus(
        @NotNull ScratchBirdLiveProbe.ProbePlan plan,
        @Nullable ScratchBirdLiveProbe.ProbeResult result
    ) {
        if (!plan.executable()) {
            return plan.label() + " unavailable";
        }
        if (result == null) {
            return plan.label() + " pending";
        }
        return result.status().kind() + ": " + result.status().message();
    }

    @NotNull
    private static String summarize(@NotNull List<String> values) {
        if (values.isEmpty()) {
            return "-";
        }
        int limit = Math.min(values.size(), 3);
        String summary = String.join(" | ", values.subList(0, limit));
        if (values.size() > limit) {
            return summary + " | ...";
        }
        return summary;
    }
}
