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

import java.util.List;

public record ScratchBirdManagementActionEnvelope(
    @NotNull String envelopeId,
    @NotNull String formId,
    @NotNull ScratchBirdFormMode mode,
    @NotNull String targetPath,
    @NotNull String previewHash,
    @NotNull String previewCommand,
    @NotNull String admissionProbeCommand,
    @NotNull String sblrUuidPolicy,
    @NotNull String transactionAuthority,
    @NotNull ScratchBirdSessionScope sessionScope,
    @NotNull ScratchBirdFeatureBoundary featureBoundary,
    @NotNull ScratchBirdNetworkPolicy networkPolicy
) {
    @NotNull
    public static ScratchBirdManagementActionEnvelope forPlan(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdFormMode mode,
        @NotNull String targetPath,
        @NotNull ScratchBirdAdminExecutor.ExecutionPlan executionPlan
    ) {
        ScratchBirdSessionScope scope = ScratchBirdSessionScope.anonymousPreviewScope(targetPath);
        ScratchBirdFeatureBoundary boundary = ScratchBirdFeatureBoundary.forTarget(targetPath, mode);
        String previewHash = previewHashFor(form, mode, targetPath, executionPlan);
        return new ScratchBirdManagementActionEnvelope(
            "sbdv-action:" + previewHash.substring(0, 24),
            form.id(),
            mode,
            targetPath,
            previewHash,
            executionPlan.commandText(),
            admissionProbeCommand(form, mode, targetPath, previewHash, commandHashFor(executionPlan)),
            "client_advisory_only; server_must_revalidate_sblr_uuid_principal_role_group_policy_resource_hash_visibility_and_mga_transaction_scope",
            "MGA_SERVER_OWNED_ALWAYS_ACTIVE_SESSION",
            scope,
            boundary,
            ScratchBirdNetworkPolicy.defaultPolicy());
    }

    @NotNull
    public static String previewHashFor(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdFormMode mode,
        @NotNull String targetPath,
        @NotNull ScratchBirdAdminExecutor.ExecutionPlan executionPlan
    ) {
        ScratchBirdSessionScope scope = ScratchBirdSessionScope.anonymousPreviewScope(targetPath);
        String basis = String.join("|", form.id(), mode.name(), targetPath, executionPlan.commandText(), scope.transactionScopeKey());
        return ScratchBirdSessionScope.sha256(basis);
    }

    @NotNull
    public static String commandHashFor(@NotNull ScratchBirdAdminExecutor.ExecutionPlan executionPlan) {
        return ScratchBirdSessionScope.sha256(executionPlan.commandText());
    }

    public boolean applyAllowedByClientPosture() {
        return featureBoundary.applyAllowed();
    }

    @NotNull
    public List<String> summaryLines() {
        return List.of(
            "Envelope id: " + envelopeId,
            "Form id: " + formId,
            "Mode: " + mode.name(),
            "Target: " + targetPath,
            "Preview hash: " + previewHash,
            "SBLR/UUID policy: " + sblrUuidPolicy,
            "Transaction authority: " + transactionAuthority,
            "Feature boundary: " + featureBoundary.availability() + " / " + featureBoundary.refusalCode(),
            "Client apply allowed: " + applyAllowedByClientPosture(),
            "Admission probe: " + admissionProbeCommand);
    }

    @NotNull
    public List<String> reviewLines() {
        return List.of(
            "Envelope " + envelopeId + " binds form " + formId + " to target " + targetPath + ".",
            "Preview hash " + previewHash + " must match the command submitted for server authorization.",
            "DBeaver-generated SBLR/UUID bundles are never trusted as authority.",
            "Server admission must bind principal, role, group, policy snapshot, UUID visibility, language resource hash, and active MGA transaction context.",
            "The UI may display feature boundary " + featureBoundary.refusalCode() + " before execution; hidden failures are not acceptable.",
            "Network policy denies plugin telemetry, diagnostics upload, marketplace calls, and implicit update checks by default.");
    }

    @NotNull
    private static String admissionProbeCommand(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdFormMode mode,
        @NotNull String targetPath,
        @NotNull String previewHash,
        @NotNull String commandHash
    ) {
        return "SELECT admitted, refusal_code, refusal_message, audit_event_id " +
            "FROM sys.security.permission_probe " +
            "WHERE target_path = " + literal(targetPath) +
            " AND form_id = " + literal(form.id()) +
            " AND form_mode = " + literal(mode.name()) +
            " AND preview_hash = " + literal(previewHash) +
            " AND command_hash = " + literal(commandHash);
    }

    @NotNull
    private static String literal(@NotNull String value) {
        return "'" + value.replace("'", "''") + "'";
    }
}
