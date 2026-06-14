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

public final class ScratchBirdDataTransferContract {

    public enum Direction {
        IMPORT,
        EXPORT
    }

    public record TransferPlan(
        @NotNull Direction direction,
        @NotNull String targetPath,
        @NotNull String previewCommand,
        @NotNull String authorizationProbe,
        @NotNull List<String> batchingRules,
        @NotNull List<String> encodingRules,
        @NotNull List<String> resultProof
    ) {
        @NotNull
        public List<String> summaryLines() {
            return List.of(
                "Direction: " + direction,
                "Target path: " + targetPath,
                "Preview command: " + previewCommand,
                "Authorization probe: " + authorizationProbe,
                "Batching rules: " + String.join(" | ", batchingRules),
                "Encoding rules: " + String.join(" | ", encodingRules),
                "Result proof: " + String.join(" | ", resultProof));
        }
    }

    private ScratchBirdDataTransferContract() {
    }

    @NotNull
    public static TransferPlan plan(@NotNull Direction direction, @NotNull String targetPath) {
        String preview = direction == Direction.EXPORT
            ? "COPY " + quotePath(targetPath) + " TO STDOUT WITH FORMAT CSV ENCODING 'UTF-8'"
            : "COPY " + quotePath(targetPath) + " FROM STDIN WITH FORMAT CSV ENCODING 'UTF-8'";
        return new TransferPlan(
            direction,
            targetPath,
            preview,
            "SELECT admitted, refusal_code, refusal_message FROM sys.security.permission_probe " +
                "WHERE target_path = " + literal(targetPath) +
                " AND form_id = 'SBDV-DATA-TRANSFER' AND form_mode = " + literal(direction.name()),
            List.of(
                "Default transfer batches are bounded by server-admitted memory and transaction policy.",
                "Batch retry never fabricates commit state; ambiguous result requires server recovery/audit proof.",
                "Cancellation must return a server diagnostic and refresh transaction status."),
            List.of(
                "UTF-8 is the default interchange encoding for text transfer.",
                "Language resource hashes travel as advisory metadata only.",
                "Identifiers are resolved to server-authorized UUIDs before mutation or export visibility decisions."),
            List.of(
                "Import verifies row counts and server diagnostics.",
                "Export verifies result parity against server-visible rows.",
                "Support bundles redact credentials and hidden object names."));
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
