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
package org.jkiss.dbeaver.ext.scratchbird.model;

import org.jkiss.code.NotNull;
import org.jkiss.code.Nullable;

import java.util.List;
import java.util.UUID;

public record ScratchBirdActionAdmission(
    @NotNull String requestUuid,
    @NotNull String actionId,
    @NotNull String formId,
    @NotNull ScratchBirdFormMode mode,
    @NotNull String targetPath,
    @Nullable String targetUuid,
    @NotNull String previewHash,
    @NotNull String commandHash,
    @NotNull String referencedUuidHash,
    @NotNull String authorizationFingerprint,
    @NotNull String transactionContextId,
    @NotNull Decision decision,
    @NotNull String diagnosticCode,
    @NotNull String message
) {
    public enum Decision {
        PENDING_SERVER_ADMISSION,
        SERVER_ADMITTED,
        SERVER_REFUSED,
        FALLBACK_TO_SERVER_TEXT,
        CLIENT_REFUSED,
        FEATURE_UNAVAILABLE
    }

    @NotNull
    public static ScratchBirdActionAdmission fromPlan(@NotNull ScratchBirdAdminExecutor.ExecutionPlan plan) {
        Decision decision = plan.executable() ? Decision.PENDING_SERVER_ADMISSION : Decision.CLIENT_REFUSED;
        String diagnostic = plan.executable() ?
            "DRIVER.SERVER_ADMISSION_REQUIRED" :
            "DRIVER.ACTION_CLIENT_REFUSED";
        String message = plan.executable() ?
            "Preview is advisory. Execution requires server-side admission, authorization, UUID validation, and transaction-context validation." :
            "Client posture refuses this action before any server mutation can be attempted.";
        return new ScratchBirdActionAdmission(
            UUID.randomUUID().toString(),
            plan.form().id() + ":" + plan.mode().name(),
            plan.form().id(),
            plan.mode(),
            plan.targetPath(),
            null,
            ScratchBirdSecurityRedactor.hashForAudit(plan.commandText()),
            ScratchBirdSecurityRedactor.hashForAudit(plan.commandText()),
            ScratchBirdSecurityRedactor.hashForAudit(""),
            "",
            "",
            decision,
            diagnostic,
            message);
    }

    @NotNull
    public static ScratchBirdActionAdmission fromPlan(
        @NotNull ScratchBirdAdminExecutor.ExecutionPlan plan,
        @NotNull ScratchBirdAuthorizationContext context,
        @Nullable String targetUuid,
        @NotNull String referencedUuidSummary
    ) {
        ScratchBirdActionAdmission base = fromPlan(plan);
        return new ScratchBirdActionAdmission(
            base.requestUuid(),
            base.actionId(),
            base.formId(),
            base.mode(),
            base.targetPath(),
            targetUuid,
            base.previewHash(),
            base.commandHash(),
            ScratchBirdSecurityRedactor.hashForAudit(referencedUuidSummary),
            context.fingerprint(),
            context.transactionContextId(),
            context.serverAdmitted() ? base.decision() : Decision.CLIENT_REFUSED,
            context.serverAdmitted() ? base.diagnosticCode() : "DRIVER.SESSION_NOT_ADMITTED",
            context.serverAdmitted() ? base.message() : "Server-admitted session context is required before this action can be prepared.");
    }

    @NotNull
    public ScratchBirdFeatureBoundaryStatus featureBoundaryStatus() {
        return switch (decision) {
            case PENDING_SERVER_ADMISSION -> ScratchBirdFeatureBoundaryStatus.requiresServerAdmission(
                actionId,
                message,
                "sys.security.permission_probe");
            case SERVER_ADMITTED -> ScratchBirdFeatureBoundaryStatus.available(actionId, message, "server-admission");
            case SERVER_REFUSED -> new ScratchBirdFeatureBoundaryStatus(
                ScratchBirdFeatureBoundaryStatus.Kind.SERVER_REFUSED,
                actionId,
                message,
                "server-admission",
                diagnosticCode,
                false);
            case FALLBACK_TO_SERVER_TEXT -> ScratchBirdFeatureBoundaryStatus.requiresServerAdmission(
                actionId,
                "SBLR/UUID pass-through fell back to server-side SBsql text prepare under policy.",
                "server-text-prepare");
            case CLIENT_REFUSED -> ScratchBirdFeatureBoundaryStatus.policyDenied(actionId, message, "client-posture");
            case FEATURE_UNAVAILABLE -> ScratchBirdFeatureBoundaryStatus.unavailable(actionId, message, null);
        };
    }

    public boolean previewMatchesAppliedCommand(@NotNull String appliedCommandText) {
        return commandHash.equals(ScratchBirdSecurityRedactor.hashForAudit(appliedCommandText));
    }

    @NotNull
    public List<String> evidenceLines() {
        return List.of(
            "Request UUID: " + requestUuid,
            "Action: " + actionId,
            "Mode: " + mode,
            "Target: " + ScratchBirdSecurityRedactor.redactEvidenceText(targetPath),
            "Target UUID: " + (targetUuid == null || targetUuid.isBlank() ? "-" : targetUuid),
            "Preview hash: " + previewHash,
            "Command hash: " + commandHash,
            "Referenced UUID hash: " + referencedUuidHash,
            "Authorization fingerprint: " + (authorizationFingerprint.isBlank() ? "-" : authorizationFingerprint),
            "Transaction context: " + (transactionContextId.isBlank() ? "-" : transactionContextId),
            "Decision: " + decision,
            "Diagnostic: " + diagnosticCode,
            "Message: " + ScratchBirdSecurityRedactor.redactEvidenceText(message));
    }
}
