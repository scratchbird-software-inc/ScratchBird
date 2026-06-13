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

public final class ScratchBirdObjectGraphContract {

    public record GraphPlan(
        @NotNull String targetPath,
        @NotNull String dependencyQuery,
        @NotNull String searchQuery,
        @NotNull String ddlPreviewQuery,
        @NotNull String sbsqlPreviewQuery,
        @NotNull String explainQuery,
        @NotNull List<String> visibilityRules
    ) {
        @NotNull
        public List<String> summaryLines() {
            return List.of(
                "Target path: " + targetPath,
                "Dependency query: " + dependencyQuery,
                "Search query: " + searchQuery,
                "DDL preview query: " + ddlPreviewQuery,
                "SBsql preview query: " + sbsqlPreviewQuery,
                "Explain query: " + explainQuery,
                "Visibility rules: " + String.join(" | ", visibilityRules));
        }
    }

    private ScratchBirdObjectGraphContract() {
    }

    @NotNull
    public static GraphPlan plan(@NotNull String targetPath) {
        return new GraphPlan(
            targetPath,
            "SELECT source_object_id, target_object_id, dependency_type FROM sys.catalog.object_dependencies " +
                "WHERE source_path = " + literal(targetPath) + " OR target_path = " + literal(targetPath),
            "SELECT object_id, object_type, full_path, object_name FROM sys.catalog.object_resolver " +
                "WHERE full_path LIKE " + literal(targetPath + "%") + " ORDER BY full_path",
            "SELECT ddl_text FROM sys.catalog.generated_ddl WHERE full_path = " + literal(targetPath),
            "SELECT sbsql_text FROM sys.catalog.generated_sbsql WHERE full_path = " + literal(targetPath),
            "EXPLAIN " + inspectPreview(targetPath),
            List.of(
                "Object graph rows are authorization-filtered by the server.",
                "Generated DDL and generated SBsql are display artifacts; UUID identity remains catalog authority.",
                "Metadata invalidation must refresh from sys.catalog.object_resolver after apply, commit, rollback, or cache epoch change.",
                "Hidden objects must not appear in ER diagrams, search results, dependency views, generated DDL/SBsql, or explain plans."));
    }

    @NotNull
    private static String inspectPreview(@NotNull String targetPath) {
        return "SELECT * FROM sys.catalog.object_resolver WHERE full_path = " + literal(targetPath);
    }

    @NotNull
    private static String literal(@NotNull String value) {
        return "'" + value.replace("'", "''") + "'";
    }
}
