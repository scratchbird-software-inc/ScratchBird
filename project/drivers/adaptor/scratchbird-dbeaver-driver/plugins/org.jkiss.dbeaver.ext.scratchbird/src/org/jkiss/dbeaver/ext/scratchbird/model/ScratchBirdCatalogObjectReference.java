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

public record ScratchBirdCatalogObjectReference(
    @Nullable String databaseUuid,
    @Nullable String objectUuid,
    @Nullable String parentUuid,
    @NotNull String objectType,
    @NotNull String fullPath,
    @NotNull String objectName,
    boolean catalogBacked,
    boolean clientOnly
) {

    @NotNull
    public static ScratchBirdCatalogObjectReference schema(
        @Nullable String databaseUuid,
        @Nullable String objectUuid,
        @Nullable String parentUuid,
        @NotNull String fullPath,
        @NotNull String objectName
    ) {
        return new ScratchBirdCatalogObjectReference(
            databaseUuid,
            objectUuid,
            parentUuid,
            "SCHEMA",
            fullPath,
            objectName,
            true,
            false);
    }

    @NotNull
    public static ScratchBirdCatalogObjectReference syntheticSchema(@NotNull String fullPath) {
        return schema(null, null, null, fullPath, leafName(fullPath));
    }

    @NotNull
    public static ScratchBirdCatalogObjectReference clientOnly(@NotNull String fullPath, @NotNull String objectType) {
        return new ScratchBirdCatalogObjectReference(
            null,
            null,
            null,
            objectType,
            fullPath,
            leafName(fullPath),
            false,
            true);
    }

    @NotNull
    public ScratchBirdCatalogObjectReference withParentUuid(@Nullable String resolvedParentUuid) {
        return new ScratchBirdCatalogObjectReference(
            databaseUuid,
            objectUuid,
            resolvedParentUuid,
            objectType,
            fullPath,
            objectName,
            catalogBacked,
            clientOnly);
    }

    public boolean hasCatalogIdentity() {
        return CommonUtils.isNotEmpty(objectUuid);
    }

    @NotNull
    private static String leafName(@NotNull String fullPath) {
        int separator = fullPath.lastIndexOf('.');
        return separator < 0 ? fullPath : fullPath.substring(separator + 1);
    }
}
