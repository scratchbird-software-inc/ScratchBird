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

public record ScratchBirdRefusalModel(
    @NotNull Kind kind,
    @NotNull String message,
    @Nullable String sourceSurface
) {
    public enum Kind {
        ADMITTED,
        SERVER_ADMISSION_REQUIRED,
        PERMISSION_DENIED,
        MISSING_SOURCE,
        UNSUPPORTED,
        SERVER_REFUSED,
        NOT_DISCLOSED,
        CLIENT_GATED
    }

    @NotNull
    public static ScratchBirdRefusalModel admitted() {
        return new ScratchBirdRefusalModel(Kind.ADMITTED, "Server-side capability probe has not refused this action.", null);
    }

    @NotNull
    public static ScratchBirdRefusalModel admitted(@NotNull String message) {
        return new ScratchBirdRefusalModel(Kind.ADMITTED, message, null);
    }

    @NotNull
    public static ScratchBirdRefusalModel clientGated(@NotNull String message) {
        return new ScratchBirdRefusalModel(Kind.CLIENT_GATED, message, null);
    }

    @NotNull
    public static ScratchBirdRefusalModel permissionDenied(@NotNull String message) {
        return new ScratchBirdRefusalModel(Kind.PERMISSION_DENIED, message, null);
    }

    @NotNull
    public static ScratchBirdRefusalModel serverAdmissionRequired(
        @NotNull String message,
        @Nullable String sourceSurface
    ) {
        return new ScratchBirdRefusalModel(Kind.SERVER_ADMISSION_REQUIRED, message, sourceSurface);
    }

    @NotNull
    public static ScratchBirdRefusalModel missingSource(@NotNull String message, @Nullable String sourceSurface) {
        return new ScratchBirdRefusalModel(Kind.MISSING_SOURCE, message, sourceSurface);
    }

    @NotNull
    public static ScratchBirdRefusalModel unsupported(@NotNull String message) {
        return new ScratchBirdRefusalModel(Kind.UNSUPPORTED, message, null);
    }

    @NotNull
    public static ScratchBirdRefusalModel serverRefused(@NotNull String message, @Nullable String sourceSurface) {
        return new ScratchBirdRefusalModel(Kind.SERVER_REFUSED, message, sourceSurface);
    }

    @NotNull
    public static ScratchBirdRefusalModel notDisclosed(@NotNull String message, @Nullable String sourceSurface) {
        return new ScratchBirdRefusalModel(Kind.NOT_DISCLOSED, message, sourceSurface);
    }

    public boolean isAdmitted() {
        return kind == Kind.ADMITTED;
    }

    public boolean allowsServerProbe() {
        return kind == Kind.ADMITTED || kind == Kind.SERVER_ADMISSION_REQUIRED;
    }

    public boolean isDeterministicRefusal() {
        return kind != Kind.ADMITTED && kind != Kind.SERVER_ADMISSION_REQUIRED;
    }

    @NotNull
    public String redactedMessage() {
        return ScratchBirdSecurityRedactor.redactEvidenceText(message);
    }
}
