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

public final class ScratchBirdDataEditorContract {

    public enum Operation {
        INSERT,
        UPDATE,
        DELETE,
        REFRESH
    }

    public record EditorPlan(
        @NotNull Operation operation,
        @NotNull String targetPath,
        @NotNull String previewCommand,
        @NotNull String admissionProbeCommand,
        @NotNull List<String> transactionProof,
        @NotNull List<String> typeProof,
        @NotNull List<String> refusalProof
    ) {
        @NotNull
        public List<String> summaryLines() {
            return List.of(
                "Operation: " + operation,
                "Target path: " + targetPath,
                "Preview command: " + previewCommand,
                "Admission probe: " + admissionProbeCommand,
                "Transaction proof: " + String.join(" | ", transactionProof),
                "Type proof: " + String.join(" | ", typeProof),
                "Refusal proof: " + String.join(" | ", refusalProof));
        }
    }

    private ScratchBirdDataEditorContract() {
    }

    @NotNull
    public static EditorPlan plan(@NotNull Operation operation, @NotNull String targetPath) {
        String quotedPath = quotePath(targetPath);
        String preview = switch (operation) {
            case INSERT -> "INSERT INTO " + quotedPath + " (id) VALUES (CAST(? AS UUID))";
            case UPDATE -> "UPDATE " + quotedPath + " SET updated_at = CURRENT_TIMESTAMP WHERE id = CAST(? AS UUID)";
            case DELETE -> "DELETE FROM " + quotedPath + " WHERE id = CAST(? AS UUID)";
            case REFRESH -> "SELECT * FROM " + quotedPath + " FETCH FIRST 200 ROWS ONLY";
        };
        return new EditorPlan(
            operation,
            targetPath,
            preview,
            permissionProbe(targetPath, "DATA_EDITOR_" + operation.name()),
            transactionProof(operation),
            List.of(
                "DBeaver typed values must route through ScratchBirdValueProfile and ScratchBirdValueBinding.",
                "UUID values use CAST(text AS UUID) unless the JDBC driver exposes a native UUID object.",
                "JSONB, VECTOR, RANGE, HASH256, and binary content use ScratchBird value handlers before SQL/SBLR preparation."),
            List.of(
                "Permission refusal must be displayed from server SQLSTATE/native diagnostic mapping.",
                "Generated SBLR/UUID update bundles must be revalidated by the server before mutation.",
                "Data editor refresh must reload from server truth after commit or rollback."));
    }

    @NotNull
    private static List<String> transactionProof(@NotNull Operation operation) {
        if (operation == Operation.REFRESH) {
            return List.of(
                "Refresh executes inside the active ScratchBird session transaction.",
                "Visibility follows server MGA snapshot rules and authorization filtering.");
        }
        return List.of(
            "DBeaver data editor does not own commit or rollback finality.",
            "Mutation executes inside the current server-owned MGA transaction.",
            "Autocommit is only a compatibility profile; server opens the replacement transaction after commit or rollback.",
            "Savepoint support is delegated to the server/JDBC transaction route when exposed.");
    }

    @NotNull
    private static String permissionProbe(@NotNull String targetPath, @NotNull String operation) {
        return "SELECT admitted, refusal_code, refusal_message FROM sys.security.permission_probe " +
            "WHERE target_path = " + literal(targetPath) +
            " AND form_id = 'SBDV-DATA-EDITOR' AND form_mode = " + literal(operation);
    }

    @NotNull
    private static String quotePath(@NotNull String path) {
        StringBuilder builder = new StringBuilder();
        for (String segment : path.split("\\.")) {
            if (segment.isBlank()) {
                continue;
            }
            if (builder.length() > 0) {
                builder.append('.');
            }
            builder.append(segment.matches("[A-Za-z_][A-Za-z0-9_]*") ? segment : '"' + segment.replace("\"", "\"\"") + '"');
        }
        return builder.toString();
    }

    @NotNull
    private static String literal(@NotNull String value) {
        return "'" + value.replace("'", "''") + "'";
    }
}
