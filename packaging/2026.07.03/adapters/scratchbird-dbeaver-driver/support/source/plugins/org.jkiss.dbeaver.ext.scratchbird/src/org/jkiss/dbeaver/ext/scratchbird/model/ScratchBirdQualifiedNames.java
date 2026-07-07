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
import org.jkiss.dbeaver.model.struct.DBSObject;
import org.jkiss.dbeaver.ext.generic.model.GenericSchema;

final class ScratchBirdQualifiedNames {

    private ScratchBirdQualifiedNames() {
    }

    @NotNull
    static String qualify(
        @NotNull ScratchBirdDataSource dataSource,
        @Nullable GenericSchema schema,
        @NotNull DBSObject object
    ) {
        return schema == null
            ? quoteIdentifier(object.getName())
            : qualifyPath(schema.getName(), object.getName());
    }

    @NotNull
    static String qualifyPath(@Nullable String schemaPath, @NotNull String objectName) {
        StringBuilder builder = new StringBuilder();
        if (schemaPath != null) {
            for (String segment : schemaPath.split("\\.")) {
                if (segment == null || segment.isBlank()) {
                    continue;
                }
                if (builder.length() > 0) {
                    builder.append('.');
                }
                builder.append(quoteIdentifier(segment));
            }
        }
        if (builder.length() > 0) {
            builder.append('.');
        }
        builder.append(quoteIdentifier(objectName));
        return builder.toString();
    }

    @NotNull
    static String qualifyAuthorityPath(@Nullable String authorityPath) {
        StringBuilder builder = new StringBuilder();
        if (authorityPath != null) {
            for (String segment : authorityPath.split("\\.")) {
                if (segment == null || segment.isBlank()) {
                    continue;
                }
                if (builder.length() > 0) {
                    builder.append('.');
                }
                builder.append(quoteIdentifier(segment));
            }
        }
        return builder.toString();
    }

    @NotNull
    static String joinPath(@Nullable String parentPath, @Nullable String objectName) {
        StringBuilder builder = new StringBuilder();
        if (parentPath != null && !parentPath.isBlank()) {
            builder.append(parentPath.trim());
        }
        if (objectName != null && !objectName.isBlank()) {
            if (builder.length() > 0) {
                builder.append('.');
            }
            builder.append(objectName.trim());
        }
        return builder.toString();
    }

    @NotNull
    static String parentPath(@Nullable String authorityPath) {
        if (authorityPath == null) {
            return "";
        }
        String trimmed = authorityPath.trim();
        int separator = trimmed.lastIndexOf('.');
        return separator < 0 ? "" : trimmed.substring(0, separator);
    }

    @NotNull
    private static String quoteIdentifier(@Nullable String identifier) {
        if (identifier == null) {
            return "";
        }
        String trimmed = identifier.trim();
        if (trimmed.isEmpty()) {
            return "";
        }
        if (trimmed.matches("[a-z_][a-z0-9_$]*")) {
            return trimmed;
        }
        return "\"" + trimmed.replace("\"", "\"\"") + "\"";
    }
}
