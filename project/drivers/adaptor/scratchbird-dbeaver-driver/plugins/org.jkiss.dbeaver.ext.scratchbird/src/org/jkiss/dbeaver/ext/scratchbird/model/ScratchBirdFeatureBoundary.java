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
import java.util.Locale;

public record ScratchBirdFeatureBoundary(
    @NotNull String featureId,
    @NotNull Availability availability,
    @NotNull String uiState,
    @NotNull String refusalCode,
    boolean previewAllowed,
    boolean applyAllowed,
    boolean auditRequired,
    @NotNull List<String> proofLines
) {
    public enum Availability {
        AVAILABLE,
        SERVER_AUTHORIZATION_REQUIRED,
        POLICY_DENIED,
        UNSUPPORTED,
        CLOSED_PROVIDER_REQUIRED,
        ENTERPRISE_ONLY
    }

    @NotNull
    public static ScratchBirdFeatureBoundary forTarget(
        @NotNull String targetPath,
        @NotNull ScratchBirdFormMode mode
    ) {
        String normalized = targetPath.toLowerCase(Locale.ENGLISH);
        if (normalized.startsWith("cluster.")) {
            return closedProvider("cluster-provider", "Cluster provider is not included in the public DBeaver plugin release.");
        }
        if (normalized.startsWith("sys.cluster") || normalized.contains(".cluster.")) {
            return closedProvider("cluster-provider", "Cluster inspection is read-only unless a server-side cluster provider admits the operation.");
        }
        if (normalized.contains("enterprise")) {
            return enterpriseOnly("enterprise-management", "This surface requires an Enterprise DBeaver/ScratchBird evidence lane before it can be exposed as apply-capable.");
        }
        if (normalized.startsWith("sys.internals")) {
            return policyDenied("sys-internals", "sys.internals is server-protected and DBeaver may only display exact server refusals.");
        }
        if (normalized.startsWith("metrics") && mode != ScratchBirdFormMode.REPORT &&
            mode != ScratchBirdFormMode.INSPECT && mode != ScratchBirdFormMode.READ_ONLY) {
            return unsupported("metrics-mutation", "Metrics branches are report surfaces and do not accept DBeaver lifecycle mutations.");
        }
        boolean mutation = mode == ScratchBirdFormMode.CREATE ||
            mode == ScratchBirdFormMode.ALTER ||
            mode == ScratchBirdFormMode.DELETE ||
            mode == ScratchBirdFormMode.TASK;
        if (mutation) {
            return new ScratchBirdFeatureBoundary(
                "server-admitted-management-action",
                Availability.SERVER_AUTHORIZATION_REQUIRED,
                "preview-only until server authorization probe and live apply route admit the action",
                "SBDV-FEATURE-SERVER-AUTHZ-REQUIRED",
                true,
                false,
                true,
                List.of(
                    "DBeaver generated previews are advisory.",
                    "SBLR/UUID bundles are not trusted until the ScratchBird server revalidates principal, role, group, resource hash, UUID visibility, and active MGA transaction scope.",
                    "A UI apply button may become enabled only after the admitted server route is available for the selected feature."));
        }
        return new ScratchBirdFeatureBoundary(
            "server-authorized-read-surface",
            Availability.AVAILABLE,
            "read-only surface available subject to server result visibility",
            "SBDV-FEATURE-AVAILABLE",
            true,
            true,
            false,
            List.of(
                "DBeaver may issue read-only probes.",
                "The server still filters rows, UUIDs, object paths, and diagnostics by the active authorization context."));
    }

    @NotNull
    public static ScratchBirdFeatureBoundary closedProvider(@NotNull String featureId, @NotNull String reason) {
        return new ScratchBirdFeatureBoundary(
            featureId,
            Availability.CLOSED_PROVIDER_REQUIRED,
            "visible refusal; closed provider required for apply-capable behavior",
            "SBDV-FEATURE-CLOSED-PROVIDER",
            true,
            false,
            true,
            List.of(reason, "Public DBeaver plugin must not imply that unavailable provider behavior is installed."));
    }

    @NotNull
    public static ScratchBirdFeatureBoundary enterpriseOnly(@NotNull String featureId, @NotNull String reason) {
        return new ScratchBirdFeatureBoundary(
            featureId,
            Availability.ENTERPRISE_ONLY,
            "visible refusal; Enterprise evidence lane required",
            "SBDV-FEATURE-ENTERPRISE-ONLY",
            true,
            false,
            true,
            List.of(reason, "Community Edition release claims remain limited to stock DBeaver CE behavior."));
    }

    @NotNull
    public static ScratchBirdFeatureBoundary policyDenied(@NotNull String featureId, @NotNull String reason) {
        return new ScratchBirdFeatureBoundary(
            featureId,
            Availability.POLICY_DENIED,
            "visible server-policy refusal",
            "SBDV-FEATURE-POLICY-DENIED",
            true,
            false,
            true,
            List.of(reason, "Policy-denied actions must not be hidden as generic UI failures."));
    }

    @NotNull
    public static ScratchBirdFeatureBoundary unsupported(@NotNull String featureId, @NotNull String reason) {
        return new ScratchBirdFeatureBoundary(
            featureId,
            Availability.UNSUPPORTED,
            "visible unsupported-feature refusal",
            "SBDV-FEATURE-UNSUPPORTED",
            true,
            false,
            false,
            List.of(reason));
    }

    @NotNull
    public List<String> summaryLines() {
        return List.of(
            "Feature id: " + featureId,
            "Availability: " + availability,
            "UI state: " + uiState,
            "Refusal code: " + refusalCode,
            "Preview allowed: " + previewAllowed,
            "Apply allowed: " + applyAllowed,
            "Audit required: " + auditRequired,
            "Proof: " + String.join(" | ", proofLines));
    }
}
