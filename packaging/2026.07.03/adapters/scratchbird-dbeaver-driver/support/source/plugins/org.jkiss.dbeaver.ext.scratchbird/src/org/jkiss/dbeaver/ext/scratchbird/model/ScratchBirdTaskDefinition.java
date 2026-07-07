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

public record ScratchBirdTaskDefinition(
    @NotNull String id,
    @NotNull String title,
    @NotNull String summary,
    @NotNull String precheckCommand,
    @NotNull String validateCommand,
    @NotNull String executeCommand,
    @NotNull String schedulePreview,
    @NotNull List<String> parameterTemplate,
    @NotNull List<String> resultSurfaces,
    @NotNull String admissionNote,
    boolean destructive
) {

    @NotNull
    public String primaryCommand() {
        if (!executeCommand.isBlank()) {
            return executeCommand;
        }
        if (!validateCommand.isBlank()) {
            return validateCommand;
        }
        return precheckCommand;
    }

    @NotNull
    public List<String> executionModes() {
        List<String> modes = new ArrayList<>();
        if (!precheckCommand.isBlank()) {
            modes.add("preview");
        }
        if (!validateCommand.isBlank()) {
            modes.add("validate-only");
        }
        if (!executeCommand.isBlank()) {
            modes.add("execute-now");
        }
        if (!schedulePreview.isBlank()) {
            modes.add("schedule-as-job");
        }
        return List.copyOf(modes);
    }

    @NotNull
    public List<String> commandMatrix() {
        List<String> lines = new ArrayList<>();
        if (!precheckCommand.isBlank()) {
            lines.add("preview: " + precheckCommand);
        }
        if (!validateCommand.isBlank()) {
            lines.add("validate-only: " + validateCommand);
        }
        if (!executeCommand.isBlank()) {
            lines.add("execute-now: " + executeCommand);
        }
        if (!schedulePreview.isBlank()) {
            lines.add("schedule-as-job: " + schedulePreview);
        }
        return List.copyOf(lines);
    }

    @NotNull
    public List<String> summaryLines() {
        List<String> lines = new ArrayList<>();
        lines.add(id + " - " + title);
        lines.add(summary);
        lines.add("Destructive: " + destructive);
        lines.add("Execution modes: " + String.join(", ", executionModes()));
        lines.add("Admission: " + admissionNote);
        return List.copyOf(lines);
    }
}
