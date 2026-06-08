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
import org.jkiss.dbeaver.DBException;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3ParseResult;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3Parser;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3Statement;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3StatementFamily;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3StatementKind;
import org.jkiss.dbeaver.model.DBUtils;
import org.jkiss.dbeaver.model.exec.DBCAttributeMetaData;
import org.jkiss.dbeaver.model.exec.DBCException;
import org.jkiss.dbeaver.model.exec.jdbc.JDBCPreparedStatement;
import org.jkiss.dbeaver.model.exec.jdbc.JDBCResultSet;
import org.jkiss.dbeaver.model.exec.jdbc.JDBCSession;
import org.jkiss.dbeaver.model.runtime.DBRProgressMonitor;
import org.jkiss.dbeaver.model.struct.DBSObject;

import java.sql.SQLException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public final class ScratchBirdLiveProbe {

    private static final int MAX_SAMPLE_ROWS = 12;
    private static final int MAX_SAMPLE_COLUMNS = 8;
    private static final int MAX_CELL_LENGTH = 120;
    private static final int STATEMENT_TIMEOUT_SECONDS = 15;

    public enum TaskProbePhase {
        PREVIEW("Preview"),
        VALIDATE("Validate"),
        EXECUTE("Execute");

        @NotNull
        private final String label;

        TaskProbePhase(@NotNull String label) {
            this.label = label;
        }

        @NotNull
        public String label() {
            return label;
        }
    }

    public record ProbePlan(
        @NotNull String label,
        @NotNull String summary,
        @NotNull String authority,
        boolean executable,
        boolean surrogate,
        @NotNull List<String> commands
    ) {
        @NotNull
        public String commandText() {
            if (commands.isEmpty()) {
                return "-";
            }
            return String.join(";\n", commands) + ";";
        }

        @NotNull
        public List<String> summaryLines() {
            return List.of(
                "Label: " + label,
                "Executable: " + executable,
                "Surrogate: " + surrogate,
                "Commands: " + commands.size(),
                "Authority: " + authority,
                "Summary: " + summary);
        }
    }

    public record StatementResult(
        @NotNull String command,
        boolean resultSet,
        long rowCount,
        long updateCount,
        @NotNull List<String> columns,
        @NotNull List<List<String>> sampleRows
    ) {
        @NotNull
        public List<String> previewLines() {
            List<String> lines = new ArrayList<>();
            lines.add("Command: " + command);
            lines.add("Result set: " + resultSet);
            if (resultSet) {
                lines.add("Columns: " + (columns.isEmpty() ? "-" : String.join(", ", columns)));
                lines.add("Rows fetched: " + rowCount);
                if (sampleRows.isEmpty()) {
                    lines.add("Samples: -");
                } else {
                    for (int i = 0; i < sampleRows.size(); i++) {
                        lines.add("Row " + (i + 1) + ": " + String.join(" | ", sampleRows.get(i)));
                    }
                }
            } else {
                lines.add("Update count: " + updateCount);
            }
            return List.copyOf(lines);
        }
    }

    public record ProbeResult(
        @NotNull ProbePlan plan,
        @NotNull ScratchBirdRefusalModel status,
        @NotNull List<StatementResult> statementResults
    ) {
        @NotNull
        public List<String> summaryLines() {
            return List.of(
                "Status: " + status.kind(),
                "Message: " + status.message(),
                "Statements executed: " + statementResults.size(),
                "Probe label: " + plan.label(),
                "Surrogate: " + plan.surrogate());
        }

        @NotNull
        public String previewText() {
            List<String> lines = new ArrayList<>(summaryLines());
            for (StatementResult statementResult : statementResults) {
                lines.add("");
                lines.addAll(statementResult.previewLines());
            }
            return String.join("\n", lines);
        }
    }

    private ScratchBirdLiveProbe() {
    }

    @NotNull
    public static ProbePlan plan(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdFormMode mode,
        @NotNull String targetPath,
        @NotNull ScratchBirdAdminExecutor.ExecutionPlan executionPlan,
        @NotNull List<ScratchBirdTaskDefinition> taskDefinitions,
        @Nullable ScratchBirdDestructivePlan destructivePlan
    ) {
        return switch (mode) {
            case INSPECT, READ_ONLY, REPORT -> fromScript(
                "Live preview execution",
                "Execute the generated read-only preview against the connected ScratchBird server.",
                executionPlan.authority(),
                false,
                executionPlan.commandText());
            case TASK -> taskProbePlan(taskDefinitions, executionPlan.authority());
            case DELETE -> destructiveProbePlan(executionPlan.authority(), destructivePlan);
            case CREATE, ALTER -> surrogateInspectPlan(targetPath, executionPlan.authority());
        };
    }

    @NotNull
    public static ProbePlan planForTask(
        @NotNull ScratchBirdTaskDefinition taskDefinition,
        @NotNull TaskProbePhase phase,
        @NotNull String authority
    ) {
        List<String> candidates = switch (phase) {
            case PREVIEW -> List.of(taskDefinition.precheckCommand(), taskDefinition.validateCommand(), taskDefinition.executeCommand());
            case VALIDATE -> List.of(taskDefinition.validateCommand(), taskDefinition.precheckCommand(), taskDefinition.executeCommand());
            case EXECUTE -> List.of(taskDefinition.executeCommand(), taskDefinition.validateCommand(), taskDefinition.precheckCommand());
        };
        List<String> commands = firstSafeCommands(candidates);
        if (commands.isEmpty()) {
            return new ProbePlan(
                taskDefinition.id() + " - " + phase.label() + " probe",
                "No parser-safe read-only command is available for this task phase.",
                authority + "; task " + taskDefinition.id() + " " + phase.label().toLowerCase(Locale.ENGLISH) + " phase",
                false,
                false,
                List.of());
        }
        return new ProbePlan(
            taskDefinition.id() + " - " + phase.label() + " probe",
            "Execute the selected task's " + phase.label().toLowerCase(Locale.ENGLISH) +
                " command only when it remains parser-safe and read-only.",
            authority + "; task " + taskDefinition.id() + " " + phase.label().toLowerCase(Locale.ENGLISH) + " phase",
            true,
            false,
            commands);
    }

    @NotNull
    public static ProbeResult execute(
        @NotNull DBRProgressMonitor monitor,
        @NotNull DBSObject targetObject,
        @NotNull ProbePlan plan
    ) {
        if (!plan.executable()) {
            return new ProbeResult(
                plan,
                ScratchBirdRefusalModel.clientGated("No safe live server probe is available for this form yet."),
                List.of());
        }
        List<StatementResult> results = new ArrayList<>();
        try (JDBCSession session = DBUtils.openUtilSession(monitor, targetObject, "ScratchBird live server probe")) {
            for (String command : plan.commands()) {
                monitor.subTask(command);
                results.add(executeStatement(session, command));
            }
            return new ProbeResult(
                plan,
                ScratchBirdRefusalModel.admitted("Read-only ScratchBird server probe completed successfully."),
                List.copyOf(results));
        } catch (DBException | RuntimeException e) {
            return new ProbeResult(
                plan,
                classifyFailure(e),
                List.copyOf(results));
        }
    }

    @NotNull
    private static ProbePlan surrogateInspectPlan(@NotNull String targetPath, @NotNull String authority) {
        ScratchBirdFormDefinition inspectForm = ScratchBirdFormRegistry.resolveForPath(
            targetPath,
            ScratchBirdNavigatorActionRegistry.Action.OPEN);
        ScratchBirdAdminExecutor.ExecutionPlan inspectPlan = ScratchBirdAdminExecutor.plan(
            inspectForm,
            ScratchBirdFormMode.INSPECT,
            targetPath);
        return fromScript(
            "Read-only surrogate probe",
            "Use the inspect surface for the selected branch/object to confirm live visibility before create/alter execution.",
            authority,
            true,
            inspectPlan.commandText());
    }

    @NotNull
    private static ProbePlan destructiveProbePlan(
        @NotNull String authority,
        @Nullable ScratchBirdDestructivePlan destructivePlan
    ) {
        if (destructivePlan == null || destructivePlan.dependencyPreview().isEmpty()) {
            return new ProbePlan(
                "Destructive dependency probe",
                "No dependency-preview query is available for this destructive flow yet.",
                authority,
                false,
                true,
                List.of());
        }
        return fromCommands(
            "Destructive dependency probe",
            "Execute dependency-preview queries only; the destructive command itself is never run from the probe lane.",
            authority,
            true,
            destructivePlan.dependencyPreview());
    }

    @NotNull
    private static ProbePlan taskProbePlan(
        @NotNull List<ScratchBirdTaskDefinition> taskDefinitions,
        @NotNull String authority
    ) {
        if (taskDefinitions.isEmpty()) {
            return new ProbePlan(
                "Task validation probe",
                "No task catalog is available for this target.",
                authority,
                false,
                true,
                List.of());
        }
        return planForTask(taskDefinitions.get(0), TaskProbePhase.VALIDATE, authority);
    }

    private static void appendNonCapability(@NotNull List<String> commands, @NotNull String command) {
        if (command.isBlank()) {
            return;
        }
        if (command.toUpperCase(Locale.ENGLISH).contains("SHOW MANAGEMENT CAPABILITIES")) {
            return;
        }
        commands.add(command);
    }

    @NotNull
    private static List<String> firstSafeCommands(@NotNull List<String> candidates) {
        for (String candidate : candidates) {
            if (candidate.isBlank()) {
                continue;
            }
            if (candidate.toUpperCase(Locale.ENGLISH).contains("SHOW MANAGEMENT CAPABILITIES")) {
                continue;
            }
            List<String> commands = extractSafeCommands(candidate);
            if (!commands.isEmpty()) {
                return commands;
            }
        }
        return List.of();
    }

    @NotNull
    private static ProbePlan fromScript(
        @NotNull String label,
        @NotNull String summary,
        @NotNull String authority,
        boolean surrogate,
        @NotNull String script
    ) {
        return fromCommands(label, summary, authority, surrogate, extractSafeCommands(script));
    }

    @NotNull
    private static ProbePlan fromCommands(
        @NotNull String label,
        @NotNull String summary,
        @NotNull String authority,
        boolean surrogate,
        @NotNull List<String> commands
    ) {
        return new ProbePlan(label, summary, authority, !commands.isEmpty(), surrogate, List.copyOf(commands));
    }

    @NotNull
    static List<String> extractSafeCommands(@NotNull String script) {
        ScratchBirdV3ParseResult parseResult = ScratchBirdV3Parser.parse(script);
        if (!parseResult.success()) {
            return List.of();
        }
        List<String> commands = new ArrayList<>();
        for (ScratchBirdV3Statement statement : parseResult.statements()) {
            if (!isSafeForExecution(statement)) {
                return List.of();
            }
            int start = statement.span().start().offset();
            int end = Math.min(statement.span().endOffset(), script.length());
            if (start < 0 || start >= end) {
                return List.of();
            }
            String command = script.substring(start, end).trim();
            while (command.endsWith(";")) {
                command = command.substring(0, command.length() - 1).trim();
            }
            if (!command.isEmpty()) {
                commands.add(command);
            }
        }
        return List.copyOf(commands);
    }

    static boolean isSafeForExecution(@NotNull ScratchBirdV3Statement statement) {
        if (statement.kind() == ScratchBirdV3StatementKind.SELECT ||
            statement.kind() == ScratchBirdV3StatementKind.SHOW ||
            statement.kind() == ScratchBirdV3StatementKind.DESCRIBE ||
            statement.kind() == ScratchBirdV3StatementKind.EXPLAIN) {
            return true;
        }
        if (statement.kind() == ScratchBirdV3StatementKind.WITH) {
            return true;
        }
        if ((statement.kind() == ScratchBirdV3StatementKind.MANAGEMENT_CONTROL ||
            statement.kind() == ScratchBirdV3StatementKind.CLUSTER_CONTROL) &&
            "SHOW".equals(statement.leadingKeyword())) {
            return true;
        }
        return statement.family() == ScratchBirdV3StatementFamily.SESSION &&
            "SHOW".equals(statement.leadingKeyword());
    }

    @NotNull
    private static StatementResult executeStatement(
        @NotNull JDBCSession session,
        @NotNull String command
    ) throws DBCException {
        try (JDBCPreparedStatement statement = session.prepareStatement(command)) {
            statement.setStatementTimeout(STATEMENT_TIMEOUT_SECONDS);
            statement.setLimit(0, MAX_SAMPLE_ROWS);
            statement.setResultsFetchSize(MAX_SAMPLE_ROWS);
            boolean hasResultSet = statement.executeStatement();
            if (!hasResultSet) {
                return new StatementResult(command, false, 0, statement.getUpdateRowCount(), List.of(), List.of());
            }
            try (JDBCResultSet resultSet = statement.openResultSet()) {
                if (resultSet == null) {
                    return new StatementResult(command, true, 0, 0, List.of(), List.of());
                }
                List<? extends DBCAttributeMetaData> attributes = resultSet.getMeta().getAttributes();
                int columnCount = Math.min(attributes.size(), MAX_SAMPLE_COLUMNS);
                List<String> columns = new ArrayList<>(columnCount);
                for (int i = 0; i < columnCount; i++) {
                    columns.add(attributes.get(i).getName());
                }

                List<List<String>> rows = new ArrayList<>();
                long rowCount = 0;
                while (rowCount < MAX_SAMPLE_ROWS && resultSet.nextRow()) {
                    List<String> row = new ArrayList<>(columnCount);
                    for (int i = 0; i < columnCount; i++) {
                        row.add(renderValue(resultSet.getAttributeValue(i)));
                    }
                    rows.add(List.copyOf(row));
                    rowCount++;
                }
                return new StatementResult(command, true, rowCount, 0, List.copyOf(columns), List.copyOf(rows));
            }
        } catch (SQLException e) {
            throw new DBCException("Unable to prepare live probe statement", e);
        }
    }

    @NotNull
    private static String renderValue(@Nullable Object value) {
        if (value == null) {
            return "NULL";
        }
        String text = String.valueOf(value).replace('\n', ' ').replace('\r', ' ');
        if (text.length() <= MAX_CELL_LENGTH) {
            return text;
        }
        return text.substring(0, MAX_CELL_LENGTH - 3) + "...";
    }

    @NotNull
    private static ScratchBirdRefusalModel classifyFailure(@NotNull Throwable throwable) {
        String message = throwable.getMessage();
        if (message == null || message.isBlank()) {
            message = throwable.getClass().getSimpleName();
        }
        String lowered = message.toLowerCase(Locale.ENGLISH);
        if (lowered.contains("permission") ||
            lowered.contains("not allowed") ||
            lowered.contains("superuser") ||
            lowered.contains("authorization")) {
            return ScratchBirdRefusalModel.permissionDenied("Server denied the live probe: " + message);
        }
        if (lowered.contains("unsupported") || lowered.contains("not supported")) {
            return ScratchBirdRefusalModel.unsupported("Server rejected the live probe surface: " + message);
        }
        return ScratchBirdRefusalModel.serverRefused("Server refused the live probe: " + message, "live-server-probe");
    }
}
