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

import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;

public final class ScratchBirdPermissionProbe {

    private ScratchBirdPermissionProbe() {
    }

    @NotNull
    public static ScratchBirdRefusalModel probe(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdFormMode mode
    ) {
        return probe(form, mode, "");
    }

    @NotNull
    public static ScratchBirdRefusalModel probe(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdFormMode mode,
        @NotNull String targetPath
    ) {
        if (!form.supportsMode(mode)) {
            return ScratchBirdRefusalModel.clientGated(form.id() + " does not support " + mode + " mode.");
        }
        ScratchBirdReportDefinition report = ScratchBirdReportCatalog.findByNavigatorPath(targetPath);
        if (report != null) {
            if (mode != ScratchBirdFormMode.REPORT && mode != ScratchBirdFormMode.INSPECT && mode != ScratchBirdFormMode.READ_ONLY) {
                return ScratchBirdRefusalModel.clientGated("Metric and report nodes are report-only surfaces in DBeaver.");
            }
            return ScratchBirdMetricSourceResolver.sourceStatus(report);
        }
        if (ScratchBirdNamespaceSemantics.isMetricsPath(targetPath)) {
            return switch (mode) {
                case INSPECT, READ_ONLY, REPORT -> ScratchBirdRefusalModel.admitted(
                    "Client-only metrics/report branch is visible in DBeaver; backing sources remain separately gated.");
                default -> ScratchBirdRefusalModel.unsupported(
                    "Metrics branches are client-only report surfaces and do not accept lifecycle or admin task mutations.");
            };
        }
        if (mode == ScratchBirdFormMode.DELETE && !"SBDV-FRM-014".equals(form.id())) {
            return ScratchBirdRefusalModel.clientGated("Destructive actions must route through SBDV-FRM-014.");
        }
        String normalized = normalize(targetPath);
        ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(targetPath);
        if ((mode == ScratchBirdFormMode.CREATE || mode == ScratchBirdFormMode.ALTER || mode == ScratchBirdFormMode.DELETE) &&
            isInternalsPath(normalized)) {
            return ScratchBirdRefusalModel.permissionDenied(
                "Mutating sys.internals is disabled in the client until explicit server-side admission exists.");
        }
        if (mode == ScratchBirdFormMode.TASK && isInternalsPath(normalized)) {
            return ScratchBirdRefusalModel.permissionDenied(
                "Admin tasks against sys.internals are withheld until the server exposes an admitted operator surface.");
        }
        if (!profile.allows(mode)) {
            if (profile.inspectOnly()) {
                return ScratchBirdRefusalModel.permissionDenied(
                    profile.label() + " is an inspect-only surface in DBeaver; lifecycle mutations remain server-protected.");
            }
            return ScratchBirdRefusalModel.unsupported(
                profile.label() + " does not expose " + mode + " mode in the current ScratchBird DBeaver integration.");
        }
        if (mode == ScratchBirdFormMode.TASK && ScratchBirdTaskCatalog.tasksFor(targetPath).isEmpty()) {
            return ScratchBirdRefusalModel.clientGated(
                "No predefined ScratchBird task catalog exists for this target yet; use the object editor or report flow instead.");
        }
        return ScratchBirdRefusalModel.admitted(
            "Branch profile " + profile.id() + " (" + profile.label() + ") admits " + mode + " mode for this target.");
    }

    @NotNull
    public static ScratchBirdLiveProbe.ProbePlan planServerAuthorization(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdFormMode mode,
        @NotNull String targetPath,
        @NotNull ScratchBirdAdminExecutor.ExecutionPlan executionPlan,
        @NotNull List<ScratchBirdTaskDefinition> taskDefinitions,
        ScratchBirdDestructivePlan destructivePlan
    ) {
        ScratchBirdRefusalModel staticProbe = probe(form, mode, targetPath);
        if (!staticProbe.isAdmitted()) {
            return new ScratchBirdLiveProbe.ProbePlan(
                "Server authz probe unavailable",
                "Static client posture blocks server authz probing for this form and mode.",
                executionPlan.authority(),
                false,
                true,
                List.of());
        }
        ScratchBirdLiveProbe.ProbePlan representativePlan = ScratchBirdLiveProbe.plan(
            form,
            mode,
            targetPath,
            executionPlan,
            taskDefinitions,
            destructivePlan);
        boolean mutationMode = isMutationMode(mode);
        List<String> commands = serverAuthorizationCommands(form, mode, targetPath, representativePlan.commands());
        String summary = !mutationMode ?
            "Execute capability inventory plus branch-specific read-only commands against the connected ScratchBird server." :
            "Execute capability inventory, server mutation-admission read probe, and branch-specific read-only surrogate commands against the connected ScratchBird server.";
        return new ScratchBirdLiveProbe.ProbePlan(
            "Server authz probe",
            summary,
            executionPlan.authority() + "; capability inventory via sys.server_capabilities",
            !commands.isEmpty(),
            mutationMode || representativePlan.surrogate(),
            List.copyOf(commands));
    }

    @NotNull
    static String serverAuthorizationScript(@NotNull String targetPath) {
        ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(targetPath);
        ScratchBirdFormDefinition inspectForm = ScratchBirdFormRegistry.require(profile.formId());
        List<String> commands = serverAuthorizationCommands(
            inspectForm,
            ScratchBirdFormMode.INSPECT,
            targetPath,
            inspectRepresentativeCommands(targetPath));
        if (commands.isEmpty()) {
            return capabilityInventoryCommand() + ";";
        }
        return String.join(";\n", commands) + ";";
    }

    private static boolean isInternalsPath(@NotNull String normalizedPath) {
        return normalizedPath.equals("sys.internals") || normalizedPath.startsWith("sys.internals.");
    }

    @NotNull
    static String capabilityInventoryCommand() {
        return "SELECT capability, enabled FROM sys.server_capabilities ORDER BY capability";
    }

    @NotNull
    private static List<String> serverAuthorizationCommands(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdFormMode mode,
        @NotNull String targetPath,
        @NotNull List<String> representativeCommands
    ) {
        LinkedHashSet<String> commands = new LinkedHashSet<>();
        commands.add(capabilityInventoryCommand());
        if (isMutationMode(mode)) {
            commands.add(mutationPermissionProbeCommand(form, mode, targetPath));
        }
        commands.addAll(representativeCommands);
        return List.copyOf(commands);
    }

    private static boolean isMutationMode(@NotNull ScratchBirdFormMode mode) {
        return mode == ScratchBirdFormMode.CREATE ||
            mode == ScratchBirdFormMode.ALTER ||
            mode == ScratchBirdFormMode.DELETE ||
            mode == ScratchBirdFormMode.TASK;
    }

    @NotNull
    private static String mutationPermissionProbeCommand(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdFormMode mode,
        @NotNull String targetPath
    ) {
        return "SELECT admitted, refusal_code, refusal_message FROM sys.security.permission_probe " +
            "WHERE target_path = " + literal(targetPath) +
            " AND form_id = " + literal(form.id()) +
            " AND form_mode = " + literal(mode.name());
    }

    @NotNull
    private static List<String> inspectRepresentativeCommands(@NotNull String targetPath) {
        ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(targetPath);
        ScratchBirdFormDefinition inspectForm = ScratchBirdFormRegistry.require(profile.formId());
        ScratchBirdAdminExecutor.ExecutionPlan inspectPlan = ScratchBirdAdminExecutor.plan(
            inspectForm,
            ScratchBirdFormMode.INSPECT,
            targetPath);
        ScratchBirdLiveProbe.ProbePlan representativePlan = ScratchBirdLiveProbe.plan(
            inspectForm,
            ScratchBirdFormMode.INSPECT,
            targetPath,
            inspectPlan,
            List.of(),
            null);
        return new ArrayList<>(representativePlan.commands());
    }

    @NotNull
    private static String normalize(@NotNull String value) {
        return value.toLowerCase(Locale.ENGLISH);
    }

    @NotNull
    private static String literal(@NotNull String value) {
        return "'" + value.replace("'", "''") + "'";
    }
}
