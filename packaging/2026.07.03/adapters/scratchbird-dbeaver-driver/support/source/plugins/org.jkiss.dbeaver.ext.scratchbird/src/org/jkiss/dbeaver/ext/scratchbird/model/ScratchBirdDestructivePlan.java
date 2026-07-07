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
import java.util.List;

public record ScratchBirdDestructivePlan(
    @NotNull String confirmationPhrase,
    @NotNull List<String> dependencyPreview,
    @NotNull List<String> dryRunCommands,
    @NotNull List<String> scheduleGuidance,
    @NotNull List<String> rollbackGuidance,
    @NotNull List<String> resultSurfaces
) {

    @NotNull
    public static ScratchBirdDestructivePlan forTarget(
        @NotNull String targetPath,
        @NotNull String generatedCommand
    ) {
        return new ScratchBirdDestructivePlan(
            "DELETE " + targetPath,
            List.of(
                "SELECT object_id, object_type, schema_path, full_path, object_name FROM sys.catalog.object_resolver WHERE full_path = " + literal(targetPath),
                "SELECT object_id, object_type, schema_path, full_path, object_name FROM sys.catalog.object_resolver WHERE schema_path = " + literal(targetPath)),
            List.of(
                ScratchBirdPermissionProbe.serverAuthorizationScript(targetPath),
                generatedCommand),
            List.of(
                "Schedule the destructive action only after the capability probe and dependency preview are clean.",
                "Attach an operator note and evidence bundle before delegating deletion to a scheduled job."),
            List.of(
                "Capture catalog identity, grants, and resolver output before destructive execution.",
                "Collect support-bundle evidence before deleting protected sys, cluster, remote, or emulation objects."),
            List.of(
                "sys.catalog.object_resolver",
                "sys.server_capabilities",
                "support bundle / audit note"));
    }

    @NotNull
    public List<String> summaryLines() {
        List<String> lines = new ArrayList<>();
        lines.add("Confirmation phrase: " + confirmationPhrase);
        lines.add("Dependency preview count: " + dependencyPreview.size());
        lines.add("Dry-run / validate-only steps: " + dryRunCommands.size());
        lines.add("Schedule guidance present: " + !scheduleGuidance.isEmpty());
        lines.add("Rollback guidance present: " + !rollbackGuidance.isEmpty());
        return List.copyOf(lines);
    }

    @NotNull
    private static String literal(@NotNull String value) {
        return "'" + value.replace("'", "''") + "'";
    }
}
