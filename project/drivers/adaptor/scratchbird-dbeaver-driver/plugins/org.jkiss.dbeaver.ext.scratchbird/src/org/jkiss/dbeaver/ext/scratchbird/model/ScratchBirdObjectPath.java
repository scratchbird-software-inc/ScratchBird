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
import org.jkiss.utils.CommonUtils;

import java.util.Arrays;
import java.util.List;

public record ScratchBirdObjectPath(
    @Nullable String databaseUuid,
    @Nullable String objectUuid,
    @Nullable String parentUuid,
    @NotNull String displayPath,
    @NotNull List<String> segments,
    boolean clientOnly
) {
    @NotNull
    public static ScratchBirdObjectPath fromDisplayPath(@NotNull String displayPath, boolean clientOnly) {
        return new ScratchBirdObjectPath(
            null,
            null,
            null,
            displayPath,
            Arrays.stream(displayPath.split("\\.")).filter(segment -> !segment.isEmpty()).toList(),
            clientOnly);
    }

    @NotNull
    public static ScratchBirdObjectPath fromCatalogReference(@NotNull ScratchBirdCatalogObjectReference reference) {
        return new ScratchBirdObjectPath(
            reference.databaseUuid(),
            reference.objectUuid(),
            reference.parentUuid(),
            reference.fullPath(),
            Arrays.stream(reference.fullPath().split("\\.")).filter(segment -> !segment.isEmpty()).toList(),
            reference.clientOnly());
    }

    public boolean hasCatalogIdentity() {
        return !CommonUtils.isEmpty(objectUuid);
    }

    @NotNull
    public String identityStatus() {
        if (clientOnly) {
            return "client-only, no catalog UUID";
        }
        if (hasCatalogIdentity()) {
            return "catalog UUID " + objectUuid + (parentUuid == null ? "" : ", parent " + parentUuid);
        }
        return "server UUID metadata not published by current JDBC metadata surface";
    }
}
