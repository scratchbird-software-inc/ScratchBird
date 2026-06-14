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

public record ScratchBirdFeatureBoundaryStatus(
    @NotNull Kind kind,
    @NotNull String featureId,
    @NotNull String message,
    @Nullable String sourceSurface,
    @NotNull String diagnosticCode,
    boolean executable
) {
    public enum Kind {
        AVAILABLE,
        REQUIRES_SERVER_ADMISSION,
        POLICY_DENIED,
        UNSUPPORTED,
        UNAVAILABLE,
        ENTERPRISE_ONLY,
        CLOSED_PROVIDER_ONLY,
        SERVER_REFUSED,
        STALE,
        HIDDEN_OR_MISSING
    }

    @NotNull
    public static ScratchBirdFeatureBoundaryStatus available(
        @NotNull String featureId,
        @NotNull String message,
        @Nullable String sourceSurface
    ) {
        return new ScratchBirdFeatureBoundaryStatus(
            Kind.AVAILABLE,
            featureId,
            message,
            sourceSurface,
            "DRIVER.FEATURE_AVAILABLE",
            true);
    }

    @NotNull
    public static ScratchBirdFeatureBoundaryStatus requiresServerAdmission(
        @NotNull String featureId,
        @NotNull String message,
        @Nullable String sourceSurface
    ) {
        return new ScratchBirdFeatureBoundaryStatus(
            Kind.REQUIRES_SERVER_ADMISSION,
            featureId,
            message,
            sourceSurface,
            "DRIVER.SERVER_ADMISSION_REQUIRED",
            true);
    }

    @NotNull
    public static ScratchBirdFeatureBoundaryStatus unavailable(
        @NotNull String featureId,
        @NotNull String message,
        @Nullable String sourceSurface
    ) {
        return new ScratchBirdFeatureBoundaryStatus(
            Kind.UNAVAILABLE,
            featureId,
            message,
            sourceSurface,
            "DRIVER.FEATURE_UNAVAILABLE",
            false);
    }

    @NotNull
    public static ScratchBirdFeatureBoundaryStatus unsupported(
        @NotNull String featureId,
        @NotNull String message
    ) {
        return new ScratchBirdFeatureBoundaryStatus(
            Kind.UNSUPPORTED,
            featureId,
            message,
            null,
            "DRIVER.FEATURE_UNSUPPORTED",
            false);
    }

    @NotNull
    public static ScratchBirdFeatureBoundaryStatus policyDenied(
        @NotNull String featureId,
        @NotNull String message,
        @Nullable String sourceSurface
    ) {
        return new ScratchBirdFeatureBoundaryStatus(
            Kind.POLICY_DENIED,
            featureId,
            message,
            sourceSurface,
            "DRIVER.FEATURE_POLICY_DENIED",
            false);
    }

    @NotNull
    public static ScratchBirdFeatureBoundaryStatus hiddenOrMissing(
        @NotNull String featureId,
        @NotNull String message,
        @Nullable String sourceSurface
    ) {
        return new ScratchBirdFeatureBoundaryStatus(
            Kind.HIDDEN_OR_MISSING,
            featureId,
            message,
            sourceSurface,
            "DRIVER.HIDDEN_OBJECT_NOT_DISCLOSED",
            false);
    }

    @NotNull
    public ScratchBirdRefusalModel toRefusalModel() {
        return switch (kind) {
            case AVAILABLE -> ScratchBirdRefusalModel.admitted(message);
            case REQUIRES_SERVER_ADMISSION -> ScratchBirdRefusalModel.serverAdmissionRequired(message, sourceSurface);
            case POLICY_DENIED -> ScratchBirdRefusalModel.permissionDenied(message);
            case UNSUPPORTED, ENTERPRISE_ONLY, CLOSED_PROVIDER_ONLY -> ScratchBirdRefusalModel.unsupported(message);
            case UNAVAILABLE, STALE -> ScratchBirdRefusalModel.missingSource(message, sourceSurface);
            case SERVER_REFUSED -> ScratchBirdRefusalModel.serverRefused(message, sourceSurface);
            case HIDDEN_OR_MISSING -> ScratchBirdRefusalModel.notDisclosed(message, sourceSurface);
        };
    }

    public boolean isAvailable() {
        return kind == Kind.AVAILABLE;
    }

    public boolean isDeterministicBoundary() {
        return kind != Kind.AVAILABLE;
    }

    @NotNull
    public String summary() {
        return kind + " " + featureId + " [" + diagnosticCode + "]: " +
            ScratchBirdSecurityRedactor.redactEvidenceText(message);
    }
}
