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
 * You may obtain one at
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
import org.jkiss.dbeaver.model.DBUtils;
import org.jkiss.dbeaver.model.exec.jdbc.JDBCPreparedStatement;
import org.jkiss.dbeaver.model.exec.jdbc.JDBCSession;
import org.jkiss.dbeaver.model.runtime.DBRProgressMonitor;
import org.jkiss.dbeaver.model.struct.DBSObject;

import java.sql.SQLException;
import java.util.List;

public final class ScratchBirdMutationApplyExecutor {

    private static final int STATEMENT_TIMEOUT_SECONDS = 30;

    public enum ApplyDecision {
        APPLIED,
        REFUSED
    }

    public record ApplyResult(
        @NotNull ApplyDecision decision,
        @NotNull ScratchBirdRefusalModel status,
        @NotNull String previewHash,
        @NotNull String commandHash,
        @NotNull String appliedOperationHash,
        boolean resultSet,
        long updateCount,
        @NotNull String commandText
    ) {
        public boolean applied() {
            return decision == ApplyDecision.APPLIED;
        }

        @NotNull
        public List<String> summaryLines() {
            return List.of(
                "Decision: " + decision,
                "Status: " + status.kind() + ": " + status.redactedMessage(),
                "Preview hash: " + previewHash,
                "Command hash: " + commandHash,
                "Applied operation hash: " + appliedOperationHash,
                "Result set: " + resultSet,
                "Update count: " + updateCount);
        }

        @NotNull
        public String previewText() {
            return String.join("\n", summaryLines()) +
                "\nCommand: " + ScratchBirdSecurityRedactor.redactEvidenceText(commandText);
        }
    }

    private ScratchBirdMutationApplyExecutor() {
    }

    @NotNull
    public static ApplyResult refuse(
        @NotNull ScratchBirdAdminExecutor.ExecutionPlan plan,
        @NotNull ScratchBirdRefusalModel status,
        @NotNull String previewHash,
        @NotNull String commandHash
    ) {
        return new ApplyResult(
            ApplyDecision.REFUSED,
            status,
            previewHash,
            commandHash,
            "none",
            false,
            0,
            plan.commandText());
    }

    @NotNull
    public static ApplyResult apply(
        @NotNull DBRProgressMonitor monitor,
        @NotNull DBSObject targetObject,
        @NotNull ScratchBirdAdminExecutor.ExecutionPlan plan,
        @Nullable ScratchBirdLiveProbe.ProbeResult authzResult,
        @NotNull String previewHash,
        @NotNull String commandHash
    ) {
        ScratchBirdRefusalModel readiness = applyReadiness(plan, authzResult, previewHash, commandHash);
        if (!readiness.isAdmitted()) {
            return refuse(plan, readiness, previewHash, commandHash);
        }
        try (JDBCSession session = DBUtils.openUtilSession(monitor, targetObject, "ScratchBird admitted management apply")) {
            monitor.subTask("ScratchBird admitted management apply");
            try (JDBCPreparedStatement statement = session.prepareStatement(plan.commandText())) {
                statement.setStatementTimeout(STATEMENT_TIMEOUT_SECONDS);
                boolean resultSet = statement.executeStatement();
                long updateCount = resultSet ? 0 : statement.getUpdateRowCount();
                return new ApplyResult(
                    ApplyDecision.APPLIED,
                    ScratchBirdRefusalModel.admitted("ScratchBird server accepted the admitted management command through the JDBC/SBsql route."),
                    previewHash,
                    commandHash,
                    commandHash,
                    resultSet,
                    updateCount,
                    plan.commandText());
            }
        } catch (SQLException e) {
            return refusedByException(plan, previewHash, commandHash, e);
        } catch (DBException | RuntimeException e) {
            return refusedByException(plan, previewHash, commandHash, e);
        }
    }

    @NotNull
    public static ScratchBirdRefusalModel applyReadiness(
        @NotNull ScratchBirdAdminExecutor.ExecutionPlan plan,
        @Nullable ScratchBirdLiveProbe.ProbeResult authzResult,
        @NotNull String previewHash,
        @NotNull String commandHash
    ) {
        if (!isMutationMode(plan.mode())) {
            return ScratchBirdRefusalModel.clientGated("Inspect, report, and read-only flows have no mutation apply step.");
        }
        if (!plan.executable()) {
            return ScratchBirdRefusalModel.clientGated("This form has no executable mutation preview.");
        }
        return ScratchBirdLiveProbe.mutationAdmissionStatus(authzResult, previewHash, commandHash);
    }

    private static boolean isMutationMode(@NotNull ScratchBirdFormMode mode) {
        return mode == ScratchBirdFormMode.CREATE ||
            mode == ScratchBirdFormMode.ALTER ||
            mode == ScratchBirdFormMode.DELETE ||
            mode == ScratchBirdFormMode.TASK;
    }

    @NotNull
    private static ApplyResult refusedByException(
        @NotNull ScratchBirdAdminExecutor.ExecutionPlan plan,
        @NotNull String previewHash,
        @NotNull String commandHash,
        @NotNull Throwable throwable
    ) {
        String message = throwable.getMessage();
        if (message == null || message.isBlank()) {
            message = throwable.getClass().getSimpleName();
        }
        return refuse(
            plan,
            ScratchBirdRefusalModel.serverRefused(
                "Server refused admitted management apply: " + ScratchBirdSecurityRedactor.redactEvidenceText(message),
                "jdbc-sbsql-apply"),
            previewHash,
            commandHash);
    }
}
