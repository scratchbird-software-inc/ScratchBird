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
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3Completion;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3Diagnostic;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3ParseResult;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3Parser;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3SourceSpan;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3Statement;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3StatementFamily;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3StatementKind;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3Token;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3TokenType;

import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

public final class ScratchBirdValidationBridge {

    private static final List<String> NON_RELATIONAL_SQL_MARKERS = List.of(
        "DOCUMENT",
        "PAYLOAD",
        "JSON",
        "JSONB",
        "VECTOR",
        "GRAPH",
        "SEARCH",
        "HYBRID",
        "KEY VALUE",
        "KEY_VALUE",
        "KEY-VALUE");

    private static final Set<String> ROOT_SEGMENTS = Set.of(
        "sys",
        "users",
        "cluster",
        "emulated",
        "remote",
        "data",
        ScratchBirdNamespaceSemantics.METRICS_ROOT
    );

    private ScratchBirdValidationBridge() {
    }

    @NotNull
    public static List<String> diagnosticsFor(@NotNull String sql) {
        if (sql.isBlank()) {
            return List.of("No SQL text supplied.");
        }
        ScratchBirdV3ParseResult result = ScratchBirdV3Parser.parse(sql);
        if (!result.diagnostics().isEmpty()) {
            return result.diagnostics().stream()
                .map(ScratchBirdV3Diagnostic::format)
                .toList();
        }
        return List.of(
            "ScratchBird Java v3 parser accepted " + result.statements().size() +
                " statement(s); execution remains server-authoritative."
        );
    }

    @NotNull
    public static List<String> statementSummaryFor(@NotNull String sql) {
        if (sql.isBlank()) {
            return List.of("No SQL text supplied.");
        }
        ScratchBirdV3ParseResult result = ScratchBirdV3Parser.parse(sql);
        if (result.statements().isEmpty()) {
            return List.of("No parsed statements are available for the current SQL.");
        }
        List<String> summaries = new ArrayList<>();
        for (int index = 0; index < result.statements().size(); index++) {
            ScratchBirdV3Statement statement = result.statements().get(index);
            summaries.add("Statement " + (index + 1) + ": " + statement.surface()
                + " [" + statement.kind() + ", " + statement.family() + ", "
                + (statement.complete() ? "terminated" : "unterminated") + "]");
        }
        return summaries;
    }

    @NotNull
    public static List<String> lintHintsFor(@NotNull String sql, int offset) {
        return lintHintsFor(sql, offset, null);
    }

    @NotNull
    public static List<String> lintHintsFor(
        @NotNull String sql,
        int offset,
        @Nullable String targetPath
    ) {
        if (sql.isBlank()) {
            return List.of("No SQL text supplied.");
        }
        ScratchBirdV3ParseResult result = ScratchBirdV3Parser.parse(sql);
        if (result.statements().isEmpty()) {
            return List.of("Resolve parser errors before advisory lint hints are considered.");
        }

        Set<String> hints = new LinkedHashSet<>();
        for (int index = 0; index < result.statements().size(); index++) {
            ScratchBirdV3Statement statement = result.statements().get(index);
            int statementNumber = index + 1;

            if (!statement.complete()) {
                hints.add(advisory(
                    "INFO",
                    "PRS_JV3_L002",
                    statement.span(),
                    "Statement " + statementNumber + " is not terminated with ';'; parser completions remain fragment-oriented until the statement is closed."));
            }
            if (statement.family() == ScratchBirdV3StatementFamily.MANAGEMENT ||
                statement.family() == ScratchBirdV3StatementFamily.DCL ||
                statement.family() == ScratchBirdV3StatementFamily.CONNECTION) {
                hints.add(advisory(
                    "INFO",
                    "PRS_JV3_L003",
                    statement.span(),
                    "Statement " + statementNumber + " targets a server-controlled surface; parser acceptance does not imply authorization."));
            }
            if (statement.family() == ScratchBirdV3StatementFamily.DDL) {
                hints.add(advisory(
                    "INFO",
                    "PRS_JV3_L004",
                    statement.span(),
                    "Statement " + statementNumber + " is DDL; prefer fully qualified schema and object paths to align with ScratchBird's UUID-root navigator."));
            }
            if (statement.family() == ScratchBirdV3StatementFamily.NOSQL) {
                hints.add(advisory(
                    "INFO",
                    "PRS_JV3_L005",
                    statement.span(),
                    "Statement " + statementNumber + " uses canonical v3 bridge syntax; keep removed engine-prefixed compatibility aliases out of new SQL."));
            }
            if (statement.family() == ScratchBirdV3StatementFamily.PSQL && !statement.complete()) {
                hints.add(advisory(
                    "INFO",
                    "PRS_JV3_L006",
                    statement.span(),
                    "Statement " + statementNumber + " is a PSQL fragment; keep BEGIN/END blocks and delimiters balanced while editing."));
            }
            String currentSchemaAliasHint = currentSchemaAliasHint(statement, statementNumber);
            if (currentSchemaAliasHint != null) {
                hints.add(currentSchemaAliasHint);
            }

            addSemanticLintHints(hints, statement, statementNumber, targetPath);
        }
        addBatchSemanticLintHints(hints, result.statements());

        List<ScratchBirdV3Completion> completions = ScratchBirdSqlPromptPlanner.completionCandidates(sql, offset);
        if (!completions.isEmpty()) {
            hints.add("INFO PRS_JV3_L010 at cursor - Current parser proposals: " + completionPreview(completions) + ".");
        }
        return hints.isEmpty()
            ? List.of("No additional advisory lint hints for the current SQL.")
            : List.copyOf(hints);
    }

    @NotNull
    public static List<String> contextHintsFor(@NotNull String sql, int offset) {
        return contextHintsFor(sql, offset, null);
    }

    @NotNull
    public static List<String> contextHintsFor(
        @NotNull String sql,
        int offset,
        @Nullable String targetPath
    ) {
        if (sql.isBlank()) {
            return List.of("No SQL text supplied.");
        }
        ScratchBirdV3ParseResult result = ScratchBirdV3Parser.parse(sql);
        if (result.statements().isEmpty()) {
            return List.of("Resolve parser errors before context hints are considered.");
        }

        Set<String> hints = new LinkedHashSet<>();
        if (targetPath != null && !targetPath.isBlank()) {
            hints.add(targetPathHint(targetPath));
        }
        for (int index = 0; index < result.statements().size(); index++) {
            ScratchBirdV3Statement statement = result.statements().get(index);
            int statementNumber = index + 1;
            addPathHints(hints, statement, statementNumber);
            addObjectFamilyContextHints(hints, statement, statementNumber, targetPath);
            String derivedScopeHint = derivedScopeHint(statement, statementNumber);
            if (derivedScopeHint != null) {
                hints.add(derivedScopeHint);
            }
            String surfaceHint = surfaceHint(statement, statementNumber);
            if (surfaceHint != null) {
                hints.add(surfaceHint);
            }
            String authzHint = authzHint(statement, statementNumber);
            if (authzHint != null) {
                hints.add(authzHint);
            }
        }
        return hints.isEmpty() ? List.of("No additional context hints for the current SQL.") : List.copyOf(hints);
    }

    @NotNull
    public static List<String> serverProbeHintsFor(@NotNull String sql, int offset) {
        return serverProbeHintsFor(sql, offset, null);
    }

    @NotNull
    public static List<String> serverProbeHintsFor(
        @NotNull String sql,
        int offset,
        @Nullable String targetPath
    ) {
        if (sql.isBlank()) {
            return List.of("No SQL text supplied.");
        }
        ScratchBirdV3ParseResult result = ScratchBirdV3Parser.parse(sql);
        if (result.statements().isEmpty()) {
            return List.of("Resolve parser errors before server-backed probe hints are considered.");
        }

        Set<String> hints = new LinkedHashSet<>();
        for (int index = 0; index < result.statements().size(); index++) {
            ScratchBirdV3Statement statement = result.statements().get(index);
            int statementNumber = index + 1;
            for (ProbeHintRoute route : probeHintRoutes(statement, targetPath, sql)) {
                ScratchBirdLiveProbe.ProbePlan authzPlan = ScratchBirdPermissionProbe.planServerAuthorization(
                    route.form(),
                    route.mode(),
                    route.path(),
                    route.executionPlan(),
                    route.taskDefinitions(),
                    route.destructivePlan());
                hints.add(serverAuthorizationHint(statement, statementNumber, route, authzPlan));

                ScratchBirdLiveProbe.ProbePlan livePlan = ScratchBirdLiveProbe.plan(
                    route.form(),
                    route.mode(),
                    route.path(),
                    route.executionPlan(),
                    route.taskDefinitions(),
                    route.destructivePlan());
                hints.add(liveProbeHint(statement, statementNumber, route, livePlan));
            }
        }
        return hints.isEmpty()
            ? List.of("No additional server-backed probe hints for the current SQL.")
            : List.copyOf(hints);
    }

    @NotNull
    public static List<String> formHintsFor(@NotNull String sql, int offset) {
        return formHintsFor(sql, offset, null);
    }

    @NotNull
    public static List<String> formHintsFor(
        @NotNull String sql,
        int offset,
        @Nullable String targetPath
    ) {
        if (sql.isBlank()) {
            return List.of("No SQL text supplied.");
        }
        ScratchBirdV3ParseResult result = ScratchBirdV3Parser.parse(sql);
        if (result.statements().isEmpty()) {
            return List.of("Resolve parser errors before object-aware form hints are considered.");
        }

        Set<String> hints = new LinkedHashSet<>();
        for (int index = 0; index < result.statements().size(); index++) {
            ScratchBirdV3Statement statement = result.statements().get(index);
            int statementNumber = index + 1;
            for (ProbeHintRoute route : probeHintRoutes(statement, targetPath, sql)) {
                ScratchBirdRefusalModel permission = ScratchBirdPermissionProbe.probe(
                    route.form(),
                    route.mode(),
                    route.path());
                ScratchBirdLiveProbe.ProbePlan authzPlan = ScratchBirdPermissionProbe.planServerAuthorization(
                    route.form(),
                    route.mode(),
                    route.path(),
                    route.executionPlan(),
                    route.taskDefinitions(),
                    route.destructivePlan());
                ScratchBirdLiveProbe.ProbePlan livePlan = ScratchBirdLiveProbe.plan(
                    route.form(),
                    route.mode(),
                    route.path(),
                    route.executionPlan(),
                    route.taskDefinitions(),
                    route.destructivePlan());
                List<ScratchBirdFormPanelCatalog.Panel> panels = ScratchBirdFormPanelCatalog.panelsFor(
                    route.form(),
                    route.mode(),
                    route.path(),
                    permission,
                    route.executionPlan(),
                    authzPlan,
                    livePlan,
                    route.taskDefinitions(),
                    route.destructivePlan(),
                    null);
                hints.add(formRouteHint(statement, statementNumber, route));
                hints.add(formSurfaceHint(statement, statementNumber, route, panels));
                hints.add(formProfileHint(statement, statementNumber, route));
                String objectRouteHint = objectRouteHint(statement, statementNumber, route);
                if (objectRouteHint != null) {
                    hints.add(objectRouteHint);
                }
                String objectModelHint = objectModelHint(statement, statementNumber, route);
                if (objectModelHint != null) {
                    hints.add(objectModelHint);
                }
                String liveObjectEvidenceHint = liveObjectEvidenceHint(statement, statementNumber, route, authzPlan, livePlan);
                if (liveObjectEvidenceHint != null) {
                    hints.add(liveObjectEvidenceHint);
                }
            }
        }
        return hints.isEmpty()
            ? List.of("No additional object-aware form hints for the current SQL.")
            : List.copyOf(hints);
    }

    @NotNull
    public static List<String> completionHintsFor(@NotNull String sql, int offset) {
        if (sql.isBlank()) {
            return List.of("No SQL text supplied.");
        }
        int safeOffset = Math.max(0, Math.min(offset, sql.length()));
        List<ScratchBirdV3Completion> completions = ScratchBirdV3Parser.completionsAt(sql, safeOffset);
        if (completions.isEmpty()) {
            return List.of("No parser completions for the current context.");
        }
        return completions.stream()
            .map(completion -> completion.label() + " - " + completion.detail())
            .toList();
    }

    @NotNull
    private static String advisory(
        @NotNull String severity,
        @NotNull String code,
        @NotNull ScratchBirdV3SourceSpan span,
        @NotNull String message
    ) {
        return severity + " " + code + location(span) + " - " + message;
    }

    @NotNull
    private static String location(@NotNull ScratchBirdV3SourceSpan span) {
        return " at " + span.start().line() + ":" + span.start().column();
    }

    @NotNull
    private static String completionPreview(@NotNull List<ScratchBirdV3Completion> completions) {
        return completions.stream()
            .limit(3)
            .map(ScratchBirdV3Completion::label)
            .reduce((left, right) -> left + ", " + right)
            .orElse("-");
    }

    private static void addSemanticLintHints(
        @NotNull Set<String> hints,
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @Nullable String targetPath
    ) {
        ScratchBirdFormMode requestedMode = requestedMode(statement);
        List<String> referencedPaths = referencedPaths(statement);
        List<String> dclObjectPaths = dclObjectPaths(statement);
        List<String> dclPrincipalPaths = dclPrincipalPaths(statement);
        for (String path : referencedPaths) {
            String dmlPathMutationHint = dmlPathMutationHint(statement, statementNumber, path);
            if (dmlPathMutationHint != null) {
                hints.add(dmlPathMutationHint);
            }
            String dmlCollectionPathHint = dmlCollectionPathHint(statement, statementNumber, path);
            if (dmlCollectionPathHint != null) {
                hints.add(dmlCollectionPathHint);
            }
            String branchMutationHint = branchMutationHint(statement, statementNumber, path, requestedMode);
            if (branchMutationHint != null) {
                hints.add(branchMutationHint);
            }
            String domainCollectionPathHint = domainCollectionPathHint(statement, statementNumber, path, requestedMode);
            if (domainCollectionPathHint != null) {
                hints.add(domainCollectionPathHint);
            }
            String namespaceCollectionPathHint = namespaceCollectionPathHint(statement, statementNumber, path, requestedMode);
            if (namespaceCollectionPathHint != null) {
                hints.add(namespaceCollectionPathHint);
            }
            String specialSurfacePathHint = specialSurfaceCollectionPathHint(statement, statementNumber, path, requestedMode);
            if (specialSurfacePathHint != null) {
                hints.add(specialSurfacePathHint);
            }
            String pathObjectFamilyHint = pathObjectFamilyMismatchHint(statement, statementNumber, path);
            if (pathObjectFamilyHint != null) {
                hints.add(pathObjectFamilyHint);
            }
            String domainPlacementHint = domainPlacementHint(statement, statementNumber, path);
            if (domainPlacementHint != null) {
                hints.add(domainPlacementHint);
            }
        }
        for (String path : dclObjectPaths) {
            String dclCollectionPathHint = dclCollectionPathHint(statement, statementNumber, path);
            if (dclCollectionPathHint != null) {
                hints.add(dclCollectionPathHint);
            }
            String dclBroadSurfacePathHint = dclBroadSurfacePathHint(statement, statementNumber, path);
            if (dclBroadSurfacePathHint != null) {
                hints.add(dclBroadSurfacePathHint);
            }
        }
        for (String path : dclPrincipalPaths) {
            String dclBroadPrincipalPathHint = dclBroadPrincipalPathHint(statement, statementNumber, path);
            if (dclBroadPrincipalPathHint != null) {
                hints.add(dclBroadPrincipalPathHint);
            }
        }
        String rootedLifecycleHint = rootedLifecycleHint(statement, statementNumber, referencedPaths, targetPath, requestedMode);
        if (rootedLifecycleHint != null) {
            hints.add(rootedLifecycleHint);
        }
        String unrootedDmlHint = unrootedDmlMutationHint(statement, statementNumber, referencedPaths, targetPath);
        if (unrootedDmlHint != null) {
            hints.add(unrootedDmlHint);
        }
        String unrootedDclHint = unrootedDclObjectHint(statement, statementNumber, dclObjectPaths, targetPath);
        if (unrootedDclHint != null) {
            hints.add(unrootedDclHint);
        }
        if (targetPath != null && !targetPath.isBlank()) {
            String dmlTargetMutationHint = dmlTargetMutationHint(statement, statementNumber, targetPath);
            if (dmlTargetMutationHint != null) {
                hints.add(dmlTargetMutationHint);
            }
            String dmlCollectionTargetHint = dmlCollectionTargetHint(statement, statementNumber, targetPath);
            if (dmlCollectionTargetHint != null) {
                hints.add(dmlCollectionTargetHint);
            }
            String dclCollectionTargetHint = dclCollectionTargetHint(statement, statementNumber, targetPath);
            if (dclCollectionTargetHint != null) {
                hints.add(dclCollectionTargetHint);
            }
            String dclBroadSurfaceTargetHint = dclBroadSurfaceTargetHint(statement, statementNumber, targetPath);
            if (dclBroadSurfaceTargetHint != null) {
                hints.add(dclBroadSurfaceTargetHint);
            }
            String dclBroadPrincipalTargetHint = dclBroadPrincipalTargetHint(statement, statementNumber, targetPath);
            if (dclBroadPrincipalTargetHint != null) {
                hints.add(dclBroadPrincipalTargetHint);
            }
            String targetPolicyHint = targetPolicyHint(statement, statementNumber, targetPath, requestedMode);
            if (targetPolicyHint != null) {
                hints.add(targetPolicyHint);
            }
            String domainCollectionTargetHint = domainCollectionTargetHint(statement, statementNumber, targetPath, requestedMode);
            if (domainCollectionTargetHint != null) {
                hints.add(domainCollectionTargetHint);
            }
            String namespaceCollectionTargetHint = namespaceCollectionTargetHint(statement, statementNumber, targetPath, requestedMode);
            if (namespaceCollectionTargetHint != null) {
                hints.add(namespaceCollectionTargetHint);
            }
            String parentObjectTargetHint = parentObjectTargetHint(statement, statementNumber, targetPath);
            if (parentObjectTargetHint != null) {
                hints.add(parentObjectTargetHint);
            }
            String targetSpecialSurfaceHint = specialSurfaceCollectionTargetHint(statement, statementNumber, targetPath, requestedMode);
            if (targetSpecialSurfaceHint != null) {
                hints.add(targetSpecialSurfaceHint);
            }
            String targetObjectFamilyHint = targetObjectFamilyMismatchHint(statement, statementNumber, targetPath, referencedPaths);
            if (targetObjectFamilyHint != null) {
                hints.add(targetObjectFamilyHint);
            }
            List<String> targetComparisonPaths = isSecurityDclObjectReview(statement) ? dclObjectPaths : referencedPaths;
            String derivedTargetScopeHint = derivedTargetScopeHint(statement, statementNumber, targetPath, targetComparisonPaths);
            if (derivedTargetScopeHint != null) {
                hints.add(derivedTargetScopeHint);
            }
            String targetMismatchHint = targetMismatchHint(statement, statementNumber, targetPath, targetComparisonPaths);
            if (targetMismatchHint != null) {
                hints.add(targetMismatchHint);
            }
            String targetDomainPlacementHint = targetDomainPlacementHint(statement, statementNumber, targetPath);
            if (targetDomainPlacementHint != null) {
                hints.add(targetDomainPlacementHint);
            }
        }
    }

    private static void addBatchSemanticLintHints(
        @NotNull Set<String> hints,
        @NotNull List<ScratchBirdV3Statement> statements
    ) {
        ScratchBirdV3Statement firstMetadataMutation = null;
        ScratchBirdV3Statement firstSchemaMutation = null;
        ScratchBirdV3Statement firstSecurityMutation = null;
        ScratchBirdV3Statement firstTransactionControl = null;
        ScratchBirdV3Statement openTransactionMetadataMutation = null;
        ScratchBirdV3Statement firstUncommittedMetadataRead = null;
        int firstMetadataMutationNumber = 0;
        int firstSchemaMutationNumber = 0;
        int firstSecurityMutationNumber = 0;
        int firstTransactionControlNumber = 0;
        int openTransactionMetadataMutationNumber = 0;
        int firstUncommittedMetadataReadNumber = 0;
        int firstUncommittedMetadataReadMutationNumber = 0;
        boolean explicitTransactionOpen = false;

        for (int index = 0; index < statements.size(); index++) {
            ScratchBirdV3Statement statement = statements.get(index);
            int statementNumber = index + 1;
            if (firstTransactionControl == null && isTransactionControl(statement)) {
                firstTransactionControl = statement;
                firstTransactionControlNumber = statementNumber;
            }
            if (isTransactionStart(statement)) {
                explicitTransactionOpen = true;
                continue;
            }
            if (isTransactionEnd(statement)) {
                explicitTransactionOpen = false;
                openTransactionMetadataMutation = null;
                openTransactionMetadataMutationNumber = 0;
            }
            if (firstMetadataMutation == null && isMetadataPublicationMutation(statement)) {
                firstMetadataMutation = statement;
                firstMetadataMutationNumber = statementNumber;
            }
            if (firstSchemaMutation == null && isSchemaPublicationMutation(statement)) {
                firstSchemaMutation = statement;
                firstSchemaMutationNumber = statementNumber;
            }
            if (firstSecurityMutation == null && isSecurityPublicationMutation(statement)) {
                firstSecurityMutation = statement;
                firstSecurityMutationNumber = statementNumber;
            }
            if (explicitTransactionOpen
                && openTransactionMetadataMutation != null
                && firstUncommittedMetadataRead == null
                && isMetadataReadStatement(statement)) {
                firstUncommittedMetadataRead = statement;
                firstUncommittedMetadataReadNumber = statementNumber;
                firstUncommittedMetadataReadMutationNumber = openTransactionMetadataMutationNumber;
            }
            if (explicitTransactionOpen
                && openTransactionMetadataMutation == null
                && isMetadataPublicationMutation(statement)) {
                openTransactionMetadataMutation = statement;
                openTransactionMetadataMutationNumber = statementNumber;
            }
        }

        if (firstMetadataMutation != null && firstTransactionControl != null) {
            hints.add(metadataTransactionPublicationHint(
                firstMetadataMutation,
                firstMetadataMutationNumber,
                firstTransactionControlNumber,
                firstTransactionControl.kind()));
        }
        if (firstSchemaMutation != null && firstSecurityMutation != null) {
            hints.add(metadataDualAnchorHint(
                firstSchemaMutation,
                firstSchemaMutationNumber,
                firstSecurityMutationNumber));
        }
        if (firstUncommittedMetadataRead != null) {
            hints.add(metadataReadAfterUncommittedMutationHint(
                firstUncommittedMetadataRead,
                firstUncommittedMetadataReadNumber,
                firstUncommittedMetadataReadMutationNumber));
        }
    }

    private static void addPathHints(
        @NotNull Set<String> hints,
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber
    ) {
        int pathCount = 0;
        for (String path : referencedPaths(statement)) {
            hints.add(pathHint(statement, statementNumber, path));
            pathCount++;
            if (pathCount >= 2) {
                break;
            }
        }
    }

    @NotNull
    private static List<String> referencedPaths(@NotNull ScratchBirdV3Statement statement) {
        LinkedHashSet<String> paths = new LinkedHashSet<>();
        List<ScratchBirdV3Token> tokens = statement.tokens();
        for (int index = 0; index < tokens.size(); index++) {
            PathScan path = pathAt(tokens, index);
            if (path == null) {
                continue;
            }

            if (path.rooted()) {
                paths.add(path.path());
            }
            index = path.endIndex();
        }
        return List.copyOf(paths);
    }

    @NotNull
    private static List<String> dclPrincipalPaths(@NotNull ScratchBirdV3Statement statement) {
        if (!isDclGrantOrRevoke(statement)) {
            return List.of();
        }
        LinkedHashSet<String> paths = new LinkedHashSet<>();
        List<ScratchBirdV3Token> tokens = statement.tokens();
        String principalMarker = statement.kind() == ScratchBirdV3StatementKind.REVOKE ? "FROM" : "TO";
        boolean principalClause = false;
        for (int index = 0; index < tokens.size(); index++) {
            ScratchBirdV3Token token = tokens.get(index);
            if (token.textEquals(principalMarker)) {
                principalClause = true;
                continue;
            }
            if (!principalClause) {
                continue;
            }
            if (isDclPrincipalClauseBoundary(token)) {
                principalClause = false;
                continue;
            }
            PathScan path = pathAt(tokens, index);
            if (path == null) {
                continue;
            }
            if (path.rooted()) {
                paths.add(path.path());
            }
            index = path.endIndex();
        }
        return List.copyOf(paths);
    }

    @NotNull
    private static List<String> dclObjectPaths(@NotNull ScratchBirdV3Statement statement) {
        if (!isSecurityDclObjectReview(statement)) {
            return List.of();
        }
        LinkedHashSet<String> paths = new LinkedHashSet<>();
        List<ScratchBirdV3Token> tokens = statement.tokens();
        boolean objectClause = false;
        for (int index = 0; index < tokens.size(); index++) {
            ScratchBirdV3Token token = tokens.get(index);
            if (token.textEquals("ON")) {
                objectClause = true;
                continue;
            }
            if (!objectClause) {
                continue;
            }
            if (isDclObjectClauseBoundary(token)) {
                objectClause = false;
                continue;
            }
            PathScan path = pathAt(tokens, index);
            if (path == null) {
                continue;
            }
            if (path.rooted()) {
                paths.add(path.path());
            }
            index = path.endIndex();
        }
        return List.copyOf(paths);
    }

    @Nullable
    private static PathScan pathAt(
        @NotNull List<ScratchBirdV3Token> tokens,
        int index
    ) {
        ScratchBirdV3Token token = tokens.get(index);
        if (!token.isIdentifierLike()) {
            return null;
        }

        int cursor = index;
        boolean dotted = false;
        StringBuilder path = new StringBuilder(token.text());
        while (cursor + 2 < tokens.size()
            && tokens.get(cursor + 1).type() == ScratchBirdV3TokenType.DOT
            && tokens.get(cursor + 2).isIdentifierLike()) {
            path.append('.').append(tokens.get(cursor + 2).text());
            cursor += 2;
            dotted = true;
        }

        String normalized = normalizePath(path.toString());
        boolean rooted = (dotted || ROOT_SEGMENTS.contains(normalized))
            && ROOT_SEGMENTS.contains(ScratchBirdNamespaceSemantics.getRootSegment(normalized));
        return new PathScan(normalized, cursor, rooted);
    }

    private static boolean isDclObjectClauseBoundary(@NotNull ScratchBirdV3Token token) {
        return token.type() == ScratchBirdV3TokenType.SEMICOLON
            || token.textEquals("TO")
            || token.textEquals("FROM")
            || token.textEquals("WITH")
            || token.textEquals("GRANTED")
            || token.textEquals("BY")
            || token.textEquals("CASCADE")
            || token.textEquals("RESTRICT");
    }

    private static boolean isDclPrincipalClauseBoundary(@NotNull ScratchBirdV3Token token) {
        return token.type() == ScratchBirdV3TokenType.SEMICOLON
            || token.textEquals("WITH")
            || token.textEquals("GRANTED")
            || token.textEquals("BY")
            || token.textEquals("CASCADE")
            || token.textEquals("RESTRICT");
    }

    @NotNull
    private static String targetPathHint(@NotNull String targetPath) {
        ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(targetPath);
        ScratchBirdFormDefinition form = ScratchBirdFormRegistry.require(profile.formId());
        String authzNote = profile.alterAllowed() || profile.deleteAllowed() || profile.taskAllowed()
            ? "Run Authz Probe is available in the management dialog for server-backed confirmation."
            : "Use Source Status or Properties for inspect-only review.";
        return "INFO PRS_JV3_H001 for target " + normalizePath(targetPath) + " - Selected target routes to "
            + form.id() + " (" + form.name() + ") via " + profile.id() + " and is a "
            + profile.mutationSummary() + ". " + authzNote;
    }

    @NotNull
    private static String pathHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String path
    ) {
        ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(path);
        ScratchBirdFormDefinition form = ScratchBirdFormRegistry.require(profile.formId());
        return advisory(
            "INFO",
            "PRS_JV3_H010",
            statement.span(),
            "Statement " + statementNumber + " references " + path + "; branch route is "
                + form.id() + " (" + form.name() + ") via " + profile.id() + " with "
                + profile.mutationSummary() + ".");
    }

    private static void addObjectFamilyContextHints(
        @NotNull Set<String> hints,
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @Nullable String targetPath
    ) {
        if (targetPath != null && !targetPath.isBlank()) {
            String targetHint = objectFamilyContextHint(statement, statementNumber, normalizePath(targetPath));
            if (targetHint != null) {
                hints.add(targetHint);
            }
        }
        int pathCount = 0;
        for (String referencedPath : referencedPaths(statement)) {
            String hint = objectFamilyContextHint(statement, statementNumber, referencedPath);
            if (hint != null) {
                hints.add(hint);
            }
            pathCount++;
            if (pathCount >= 2) {
                break;
            }
        }
    }

    @Nullable
    private static String objectFamilyContextHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String path
    ) {
        String family = objectFamily(statement);
        if (family == null) {
            return null;
        }
        ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(path);
        String descriptor = profileAdmissionDescriptor(profile, family);
        if (descriptor == null || descriptor.equals(family)) {
            return null;
        }
        return advisory(
            "INFO",
            "PRS_JV3_H032",
            statement.span(),
            "Statement " + statementNumber + " targets " + familyLabel(family) + " on " + path
                + "; " + profile.id() + " admits that route through " + descriptor
                + " children, so keep review and form routing on this ScratchBird surface.");
    }

    @Nullable
    private static String derivedScopeHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber
    ) {
        String derivedPath = derivedProbePath(statement);
        if (derivedPath == null) {
            return null;
        }
        for (String referencedPath : referencedPaths(statement)) {
            if (isWithinPath(referencedPath, derivedPath) || isWithinPath(derivedPath, referencedPath)) {
                return null;
            }
        }
        ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(derivedPath);
        ScratchBirdFormDefinition form = ScratchBirdFormRegistry.require(profile.formId());
        return advisory(
            "INFO",
            "PRS_JV3_H031",
            statement.span(),
            "Statement " + statementNumber + " also has canonical review scope " + derivedPath
                + " via " + profile.id() + " and " + form.id() + " (" + form.name()
                + "); use that branch for parser-backed context, authz probes, and live review state.");
    }

    @Nullable
    private static String surfaceHint(@NotNull ScratchBirdV3Statement statement, int statementNumber) {
        String surface = statement.surface().toUpperCase(Locale.ROOT);
        boolean ddlStatement = statement.family() == ScratchBirdV3StatementFamily.DDL;
        if (ddlStatement && surface.contains("DOMAIN")) {
            return routedSurfaceHint(statement, statementNumber, "PRS_JV3_H011", "SBDV-FRM-611",
                "domains and datatype definitions");
        }
        if (ddlStatement && surface.contains("TRIGGER")) {
            return routedSurfaceHint(statement, statementNumber, "PRS_JV3_H012", "SBDV-FRM-610", "trigger lifecycle");
        }
        if (ddlStatement && (surface.contains("PROCEDURE") || surface.contains("FUNCTION"))) {
            return routedSurfaceHint(statement, statementNumber, "PRS_JV3_H013", "SBDV-FRM-609", "routine lifecycle");
        }
        if (ddlStatement && (surface.contains("SEQUENCE") || surface.contains("GENERATOR"))) {
            return routedSurfaceHint(statement, statementNumber, "PRS_JV3_H014", "SBDV-FRM-608", "sequence lifecycle");
        }
        if (ddlStatement && surface.contains("INDEX")) {
            return routedSurfaceHint(statement, statementNumber, "PRS_JV3_H015", "SBDV-FRM-607", "index lifecycle");
        }
        if (ddlStatement && (surface.contains("CONSTRAINT") || surface.contains("FOREIGN KEY") || surface.contains("UNIQUE KEY"))) {
            return routedSurfaceHint(statement, statementNumber, "PRS_JV3_H016", "SBDV-FRM-606", "constraint lifecycle");
        }
        if (ddlStatement && surface.contains("TABLE")) {
            return advisory(
                "INFO",
                "PRS_JV3_H017",
                statement.span(),
                "Statement " + statementNumber + " targets the table editor family; use SBDV-FRM-601 for relational tables or SBDV-FRM-602 for non-relational tables once model shape is known.");
        }
        if (ddlStatement && surface.contains("VIEW")) {
            return advisory(
                "INFO",
                "PRS_JV3_H018",
                statement.span(),
                "Statement " + statementNumber + " targets the view editor family; use SBDV-FRM-603 for relational views or SBDV-FRM-604 for non-relational views once model shape is known.");
        }
        if (statement.kind() == ScratchBirdV3StatementKind.GRANT || statement.kind() == ScratchBirdV3StatementKind.REVOKE) {
            return routedSurfaceHint(statement, statementNumber, "PRS_JV3_H019", "SBDV-FRM-110",
                "grant, role, and ownership review");
        }
        if (statement.kind() == ScratchBirdV3StatementKind.SET && containsKeyword(statement, "SCHEMA")) {
            return routedSurfaceHint(statement, statementNumber, "PRS_JV3_H020", "SBDV-FRM-013",
                "schema/session default review");
        }
        if (statement.kind() == ScratchBirdV3StatementKind.SHOW && referencesCurrentSchema(statement)) {
            return routedSurfaceHint(statement, statementNumber, "PRS_JV3_H020", "SBDV-FRM-013",
                "schema/session default review");
        }
        if (surface.startsWith("SHOW MANAGEMENT")) {
            if (containsKeyword(statement, "LISTENERS") || containsKeyword(statement, "PARSER") || containsKeyword(statement, "METRICS")) {
                return routedSurfaceHint(statement, statementNumber, "PRS_JV3_H021", "SBDV-FRM-106",
                    "monitoring and metrics review");
            }
            if (containsKeyword(statement, "MANAGER") || containsKeyword(statement, "SERVERS")
                || containsKeyword(statement, "INSTRUCTIONS") || containsKeyword(statement, "DRIFT")) {
                return routedSurfaceHint(statement, statementNumber, "PRS_JV3_H022", "SBDV-FRM-101",
                    "control-plane configuration review");
            }
        }
        if (surface.startsWith("SHOW CLUSTER")
            || statement.kind() == ScratchBirdV3StatementKind.CLUSTER_CONTROL) {
            return routedSurfaceHint(statement, statementNumber, "PRS_JV3_H023", "SBDV-FRM-104",
                "cluster control review");
        }
        if (statement.kind() == ScratchBirdV3StatementKind.SHOW || statement.kind() == ScratchBirdV3StatementKind.DESCRIBE) {
            if (containsKeyword(statement, "SCHEMA") || containsKeyword(statement, "TABLES") || containsKeyword(statement, "VIEWS")) {
                return routedSurfaceHint(statement, statementNumber, "PRS_JV3_H024", "SBDV-FRM-107",
                    "catalog and schema inspection");
            }
        }
        return null;
    }

    @Nullable
    private static String authzHint(@NotNull ScratchBirdV3Statement statement, int statementNumber) {
        if (statement.family() != ScratchBirdV3StatementFamily.MANAGEMENT
            && statement.family() != ScratchBirdV3StatementFamily.DCL
            && statement.family() != ScratchBirdV3StatementFamily.CONNECTION) {
            return null;
        }
        return advisory(
            "INFO",
            "PRS_JV3_H030",
            statement.span(),
            "Statement " + statementNumber + " uses a server-controlled surface; use Run Authz Probe in the ScratchBird management dialog before relying on mutation or administrative follow-up.");
    }

    @NotNull
    private static String routedSurfaceHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String code,
        @NotNull String formId,
        @NotNull String purpose
    ) {
        ScratchBirdFormDefinition form = ScratchBirdFormRegistry.require(formId);
        return advisory(
            "INFO",
            code,
            statement.span(),
            "Statement " + statementNumber + " maps to " + form.id() + " (" + form.name() + ") for "
                + purpose + ".");
    }

    private static boolean containsKeyword(@NotNull ScratchBirdV3Statement statement, @NotNull String keyword) {
        return statement.tokens().stream().anyMatch(token -> token.textEquals(keyword));
    }

    private static boolean referencesCurrentSchema(@NotNull ScratchBirdV3Statement statement) {
        List<ScratchBirdV3Token> tokens = statement.tokens();
        if (tokens.size() > 1 && "CURRENT_SCHEMA".equals(tokens.get(1).upperText())) {
            return true;
        }
        return tokens.size() > 2
            && "CURRENT".equals(tokens.get(1).upperText())
            && "SCHEMA".equals(tokens.get(2).upperText());
    }

    @NotNull
    private static String normalizePath(@NotNull String value) {
        return value.toLowerCase(Locale.ROOT);
    }

    private static String currentSchemaAliasHint(@NotNull ScratchBirdV3Statement statement, int statementNumber) {
        if (statement.kind() != ScratchBirdV3StatementKind.SET && statement.kind() != ScratchBirdV3StatementKind.SHOW) {
            return null;
        }
        List<ScratchBirdV3Token> tokens = statement.tokens();
        if (tokens.size() > 1 && "CURRENT_SCHEMA".equals(tokens.get(1).upperText())) {
            return advisory(
                "WARNING",
                "PRS_JV3_L001",
                tokens.get(1).span(),
                "Statement " + statementNumber + " uses CURRENT_SCHEMA; prefer "
                    + statement.leadingKeyword().toUpperCase(Locale.ROOT) + " SCHEMA as the canonical v3 spelling.");
        }
        if (tokens.size() > 2 &&
            "CURRENT".equals(tokens.get(1).upperText()) &&
            "SCHEMA".equals(tokens.get(2).upperText())) {
            return advisory(
                "WARNING",
                "PRS_JV3_L001",
                tokens.get(1).span(),
                "Statement " + statementNumber + " uses CURRENT SCHEMA; prefer "
                    + statement.leadingKeyword().toUpperCase(Locale.ROOT) + " SCHEMA as the canonical v3 spelling.");
        }
        return null;
    }

    @Nullable
    private static ScratchBirdFormMode requestedMode(@NotNull ScratchBirdV3Statement statement) {
        String leadingKeyword = statement.leadingKeyword().toUpperCase(Locale.ROOT);
        return switch (statement.kind()) {
            case CREATE, RECREATE, DECLARE -> ScratchBirdFormMode.CREATE;
            case ALTER, COMMENT, SECURITY_LABEL, GRANT, REVOKE -> ScratchBirdFormMode.ALTER;
            case DROP, TRUNCATE -> ScratchBirdFormMode.DELETE;
            case EXECUTE_JOB, CANCEL_JOB, VALIDATE, SWEEP, MANAGEMENT_CONTROL, CLUSTER_CONTROL,
                    CUBE_CONTROL, SERVICE_CHANNEL, ADMIN_CONTROL, MIGRATION_CONTROL, UDR_CONTROL,
                    EXTENSION_CONTROL -> "SHOW".equals(leadingKeyword) ? null : ScratchBirdFormMode.TASK;
            default -> null;
        };
    }

    @Nullable
    private static ScratchBirdFormMode reviewMode(@NotNull ScratchBirdV3Statement statement) {
        ScratchBirdFormMode requestedMode = requestedMode(statement);
        if (requestedMode != null) {
            return requestedMode;
        }
        String leadingKeyword = statement.leadingKeyword().toUpperCase(Locale.ROOT);
        if ("SHOW".equals(leadingKeyword)
            || "DESCRIBE".equals(leadingKeyword)
            || "EXPLAIN".equals(leadingKeyword)
            || "SELECT".equals(leadingKeyword)
            || "WITH".equals(leadingKeyword)) {
            return ScratchBirdFormMode.INSPECT;
        }
        if ("SET".equals(leadingKeyword) && containsKeyword(statement, "SCHEMA")) {
            return ScratchBirdFormMode.INSPECT;
        }
        return switch (statement.kind()) {
            case SELECT, SHOW, DESCRIBE, EXPLAIN, WITH -> ScratchBirdFormMode.INSPECT;
            case SET -> containsKeyword(statement, "SCHEMA") ? ScratchBirdFormMode.INSPECT : null;
            default -> null;
        };
    }

    @NotNull
    private static List<ProbeHintRoute> probeHintRoutes(
        @NotNull ScratchBirdV3Statement statement,
        @Nullable String targetPath,
        @NotNull String sql
    ) {
        ScratchBirdFormMode mode = reviewMode(statement);
        if (mode == null) {
            return List.of();
        }

        LinkedHashSet<String> candidates = new LinkedHashSet<>();
        if (targetPath != null && !targetPath.isBlank()) {
            candidates.add(normalizePath(targetPath));
        }
        int pathCount = 0;
        for (String path : referencedPaths(statement)) {
            candidates.add(path);
            pathCount++;
            if (pathCount >= 2) {
                break;
            }
        }
        String derivedPath = derivedProbePath(statement);
        if (derivedPath != null) {
            candidates.add(derivedPath);
        }

        List<ProbeHintRoute> routes = new ArrayList<>();
        LinkedHashSet<String> routeKeys = new LinkedHashSet<>();
        ProbeHintRoute objectRoute = objectProbeHintRoute(statement, mode, sql);
        if (objectRoute != null && routeKeys.add(routeKey(objectRoute))) {
            routes.add(objectRoute);
        }
        for (String candidate : candidates) {
            ProbeHintRoute route = probeHintRoute(candidate, mode);
            if (route != null && routeKeys.add(routeKey(route))) {
                routes.add(route);
            }
        }
        return List.copyOf(routes);
    }

    @Nullable
    private static ProbeHintRoute probeHintRoute(
        @NotNull String candidatePath,
        @NotNull ScratchBirdFormMode mode
    ) {
        ScratchBirdNavigatorActionRegistry.Action action = switch (mode) {
            case DELETE -> ScratchBirdNavigatorActionRegistry.Action.DELETE;
            case TASK -> ScratchBirdNavigatorActionRegistry.Action.TASKS;
            default -> ScratchBirdNavigatorActionRegistry.Action.OPEN;
        };
        ScratchBirdFormDefinition form = ScratchBirdFormRegistry.resolveForPath(candidatePath, action);
        if (!form.supportsMode(mode)) {
            if (mode == ScratchBirdFormMode.INSPECT && form.supportsMode(ScratchBirdFormMode.READ_ONLY)) {
                mode = ScratchBirdFormMode.READ_ONLY;
            } else {
                return null;
            }
        }
        ScratchBirdAdminExecutor.ExecutionPlan executionPlan = ScratchBirdAdminExecutor.plan(form, mode, candidatePath);
        List<ScratchBirdTaskDefinition> taskDefinitions = mode == ScratchBirdFormMode.TASK
            ? ScratchBirdTaskCatalog.tasksFor(candidatePath)
            : List.of();
        ScratchBirdDestructivePlan destructivePlan = mode == ScratchBirdFormMode.DELETE
            ? ScratchBirdDestructivePlan.forTarget(candidatePath, executionPlan.commandText())
            : null;
        return new ProbeHintRoute(candidatePath, form, mode, executionPlan, taskDefinitions, destructivePlan, false);
    }

    @Nullable
    private static ProbeHintRoute objectProbeHintRoute(
        @NotNull ScratchBirdV3Statement statement,
        @NotNull ScratchBirdFormMode mode,
        @NotNull String sql
    ) {
        if (statement.family() != ScratchBirdV3StatementFamily.DDL) {
            return null;
        }
        String formId = objectFormId(statement, sql);
        if (formId == null) {
            return null;
        }
        String objectPath = firstReferencedPath(statement);
        if (objectPath == null || objectPath.isBlank()) {
            return null;
        }
        ScratchBirdFormDefinition form = ScratchBirdFormRegistry.require(formId);
        if (!form.supportsMode(mode)) {
            return null;
        }
        String statementText = statementText(sql, statement);
        String normalizedPath = normalizePath(objectPath);
        ScratchBirdAdminExecutor.ExecutionPlan executionPlan = new ScratchBirdAdminExecutor.ExecutionPlan(
            form,
            mode,
            normalizedPath,
            statementText,
            true,
            mode == ScratchBirdFormMode.DELETE,
            "parser-derived object route preview; execution remains server-authoritative until run against ScratchBird");
        ScratchBirdDestructivePlan destructivePlan = mode == ScratchBirdFormMode.DELETE
            ? ScratchBirdDestructivePlan.forTarget(normalizedPath, statementText)
            : null;
        return new ProbeHintRoute(normalizedPath, form, mode, executionPlan, List.of(), destructivePlan, true);
    }

    @Nullable
    private static String derivedProbePath(@NotNull ScratchBirdV3Statement statement) {
        String surface = statement.surface().toUpperCase(Locale.ROOT);
        if (statement.family() == ScratchBirdV3StatementFamily.DDL) {
            String ddlObject = ddlObjectSurface(statement);
            if ("DOMAIN".equals(ddlObject) || "TYPE".equals(ddlObject)) {
                return "sys.domains";
            }
            if ("JOB".equals(ddlObject) || "MIGRATION JOB".equals(ddlObject)) {
                return "sys.jobs";
            }
            if ("USER".equals(ddlObject)
                || "ROLE".equals(ddlObject)
                || "GROUP".equals(ddlObject)
                || "POLICY".equals(ddlObject)
                || "TOKEN".equals(ddlObject)
                || "CONNECTION RULE".equals(ddlObject)) {
                return "sys.security";
            }
        }
        if (surface.startsWith("SHOW MANAGEMENT")) {
            if (containsKeyword(statement, "LISTENERS") || containsKeyword(statement, "PARSER") || containsKeyword(statement, "METRICS")) {
                return "sys.monitoring";
            }
            if (containsKeyword(statement, "MANAGER") || containsKeyword(statement, "SERVERS")
                || containsKeyword(statement, "INSTRUCTIONS") || containsKeyword(statement, "DRIFT")) {
                return "sys.config";
            }
        }
        if (surface.startsWith("SHOW CLUSTER") || statement.kind() == ScratchBirdV3StatementKind.CLUSTER_CONTROL) {
            return "sys.cluster";
        }
        if (statement.kind() == ScratchBirdV3StatementKind.GRANT || statement.kind() == ScratchBirdV3StatementKind.REVOKE) {
            return "sys.security";
        }
        if ((statement.kind() == ScratchBirdV3StatementKind.SHOW || statement.kind() == ScratchBirdV3StatementKind.DESCRIBE)
            && (containsKeyword(statement, "SCHEMA") || containsKeyword(statement, "TABLES") || containsKeyword(statement, "VIEWS"))) {
            return "sys.catalog";
        }
        return null;
    }

    @NotNull
    private static String ddlObjectSurface(@NotNull ScratchBirdV3Statement statement) {
        String surface = statement.surface().toUpperCase(Locale.ROOT);
        for (String prefix : List.of("CREATE ", "ALTER ", "DROP ", "RECREATE ")) {
            if (surface.startsWith(prefix)) {
                return surface.substring(prefix.length());
            }
        }
        return surface;
    }

    @NotNull
    private static String serverAuthorizationHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull ProbeHintRoute route,
        @NotNull ScratchBirdLiveProbe.ProbePlan authzPlan
    ) {
        String objectPreview = objectPreviewSuffix(route);
        return advisory(
            "INFO",
            "PRS_JV3_P001",
            statement.span(),
            "Statement " + statementNumber + " has a server authz probe route for " + route.path()
                + " via " + route.form().id() + " (" + route.form().name() + ") in "
                + route.mode().name().toLowerCase(Locale.ROOT) + " mode. "
                + authzPlan.summary() + " Preview: " + commandPreview(authzPlan.commands()) + "."
                + objectPreview);
    }

    @NotNull
    private static String liveProbeHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull ProbeHintRoute route,
        @NotNull ScratchBirdLiveProbe.ProbePlan livePlan
    ) {
        String objectPreview = objectPreviewSuffix(route);
        return advisory(
            "INFO",
            "PRS_JV3_P002",
            statement.span(),
            "Statement " + statementNumber + " has a live probe route for " + route.path()
                + " via " + route.form().id() + " (" + route.form().name() + ") in "
                + route.mode().name().toLowerCase(Locale.ROOT) + " mode. "
                + livePlan.summary() + " Preview: " + commandPreview(livePlan.commands()) + "."
                + objectPreview);
    }

    @NotNull
    private static String commandPreview(@NotNull List<String> commands) {
        if (commands.isEmpty()) {
            return "-";
        }
        return commands.stream()
            .limit(2)
            .map(String::trim)
            .map(command -> command.replace('\n', ' ').replace('\r', ' '))
            .map(command -> command.length() > 120 ? command.substring(0, 117) + "..." : command)
            .reduce((left, right) -> left + " | " + right)
            .map(preview -> commands.size() > 2 ? preview + " | ..." : preview)
            .orElse("-");
    }

    @NotNull
    private static String objectPreviewSuffix(@NotNull ProbeHintRoute route) {
        if (!route.objectDerived()) {
            return "";
        }
        return " Parser-derived object SQL: " + shortenedPreview(route.executionPlan().commandText()) + ".";
    }

    @NotNull
    private static String shortenedPreview(@NotNull String command) {
        String preview = command.trim().replace('\n', ' ').replace('\r', ' ');
        if (preview.length() > 120) {
            return preview.substring(0, 117) + "...";
        }
        return preview;
    }

    @NotNull
    private static String formRouteHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull ProbeHintRoute route
    ) {
        return advisory(
            "INFO",
            "PRS_JV3_F001",
            statement.span(),
            "Statement " + statementNumber + " routes to " + route.form().id() + " (" + route.form().name() + ") for "
                + route.path() + " in " + route.mode().name().toLowerCase(Locale.ROOT) + " mode. Must fields: "
                + fieldPreview(route.form().mustFields(), 4) + ". Should fields: "
                + fieldPreview(route.form().shouldFields(), 3) + ".");
    }

    @NotNull
    private static String formSurfaceHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull ProbeHintRoute route,
        @NotNull List<ScratchBirdFormPanelCatalog.Panel> panels
    ) {
        return advisory(
            "INFO",
            "PRS_JV3_F002",
            statement.span(),
            "Statement " + statementNumber + " exposes child routes " + childFormPreview(route.form().childForms())
                + " and panel surfaces " + panelPreview(panels) + " for "
                + route.form().id() + ".");
    }

    @NotNull
    private static String formProfileHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull ProbeHintRoute route
    ) {
        ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(route.path());
        return advisory(
            "INFO",
            "PRS_JV3_F003",
            statement.span(),
            "Statement " + statementNumber + " aligns with branch profile " + profile.id()
                + " (" + profile.label() + ") as a " + profile.mutationSummary()
                + ". Expected children: " + fieldPreview(profile.expectedChildren(), 3)
                + ". Focus fields: " + fieldPreview(profile.focusFields(), 3) + ".");
    }

    @Nullable
    private static String objectRouteHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull ProbeHintRoute route
    ) {
        if (!route.objectDerived()) {
            return null;
        }
        return advisory(
            "INFO",
            "PRS_JV3_F004",
            statement.span(),
            "Statement " + statementNumber + " also has an exact object-editor route to "
                + route.form().id() + " (" + route.form().name() + ") for "
                + route.path() + "; preview commands use the current SQL text instead of a generic branch template.");
    }

    @Nullable
    private static String objectModelHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull ProbeHintRoute route
    ) {
        if (!route.objectDerived()) {
            return null;
        }
        ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(route.path());
        return advisory(
            "INFO",
            "PRS_JV3_F005",
            statement.span(),
            "Statement " + statementNumber + " parser-derived object model is "
                + objectModelFamily(route.form()) + " at " + route.path()
                + " with parent path " + parentPath(route.path()) + ", leaf "
                + leafName(route.path()) + ", form route " + route.form().summary()
                + ", branch profile " + profile.id()
                + ", and catalog identity source sys.catalog.object_resolver; UUID and parent UUID truth remain server authoritative.");
    }

    @Nullable
    private static String liveObjectEvidenceHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull ProbeHintRoute route,
        @NotNull ScratchBirdLiveProbe.ProbePlan authzPlan,
        @NotNull ScratchBirdLiveProbe.ProbePlan livePlan
    ) {
        if (!route.objectDerived()) {
            return null;
        }
        return advisory(
            "INFO",
            "PRS_JV3_F006",
            statement.span(),
            "Statement " + statementNumber + " live object evidence stays read-only for "
                + route.path() + ": authz plan " + authzPlan.label() + " previews "
                + commandPreview(authzPlan.commands()) + "; live plan " + livePlan.label()
                + " previews " + commandPreview(livePlan.commands())
                + ". Mutation SQL remains a parser-derived preview until server authorization and execution confirm it.");
    }

    @NotNull
    private static String objectModelFamily(@NotNull ScratchBirdFormDefinition form) {
        return switch (form.id()) {
            case "SBDV-FRM-601" -> "relational table";
            case "SBDV-FRM-602" -> "non-relational table";
            case "SBDV-FRM-603" -> "relational view";
            case "SBDV-FRM-604" -> "non-relational view";
            case "SBDV-FRM-605" -> "column or field";
            case "SBDV-FRM-606" -> "constraint";
            case "SBDV-FRM-607" -> "index";
            case "SBDV-FRM-608" -> "sequence";
            case "SBDV-FRM-609" -> "routine";
            case "SBDV-FRM-610" -> "trigger";
            case "SBDV-FRM-611" -> "domain or datatype definition";
            case "SBDV-FRM-612" -> "payload model";
            default -> form.scope();
        };
    }

    @NotNull
    private static String parentPath(@NotNull String path) {
        int separator = normalizePath(path).lastIndexOf('.');
        return separator < 0 ? "-" : normalizePath(path).substring(0, separator);
    }

    @NotNull
    private static String leafName(@NotNull String path) {
        String normalized = normalizePath(path);
        int separator = normalized.lastIndexOf('.');
        return separator < 0 ? normalized : normalized.substring(separator + 1);
    }

    @NotNull
    private static String fieldPreview(@NotNull List<String> fields, int limit) {
        if (fields.isEmpty()) {
            return "-";
        }
        return fields.stream()
            .limit(limit)
            .reduce((left, right) -> left + ", " + right)
            .map(preview -> fields.size() > limit ? preview + ", ..." : preview)
            .orElse("-");
    }

    @NotNull
    private static String pathPreview(@NotNull List<String> paths, int limit) {
        if (paths.isEmpty()) {
            return "-";
        }
        return paths.stream()
            .limit(limit)
            .reduce((left, right) -> left + ", " + right)
            .map(preview -> paths.size() > limit ? preview + ", ..." : preview)
            .orElse("-");
    }

    @NotNull
    private static String childFormPreview(@NotNull List<String> childFormIds) {
        if (childFormIds.isEmpty()) {
            return "-";
        }
        return childFormIds.stream()
            .limit(3)
            .map(childFormId -> {
                ScratchBirdFormDefinition child = ScratchBirdFormRegistry.find(childFormId);
                return child == null ? childFormId : child.id() + " (" + child.name() + ")";
            })
            .reduce((left, right) -> left + " | " + right)
            .map(preview -> childFormIds.size() > 3 ? preview + " | ..." : preview)
            .orElse("-");
    }

    @NotNull
    private static String panelPreview(@NotNull List<ScratchBirdFormPanelCatalog.Panel> panels) {
        if (panels.isEmpty()) {
            return "-";
        }
        return panels.stream()
            .limit(3)
            .map(panel -> panel.id() + " (" + panel.tabLabel() + ")")
            .reduce((left, right) -> left + " | " + right)
            .map(preview -> panels.size() > 3 ? preview + " | ..." : preview)
            .orElse("-");
    }

    @Nullable
    private static String branchMutationHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String path,
        @Nullable ScratchBirdFormMode requestedMode
    ) {
        if (requestedMode == null) {
            return null;
        }
        if (ScratchBirdNamespaceSemantics.isMetricsPath(path)) {
            return advisory(
                "WARNING",
                "PRS_JV3_L020",
                statement.span(),
                "Statement " + statementNumber + " references " + path + ", but the metrics branch is client-only in DBeaver; route through report forms instead of direct lifecycle SQL.");
        }
        ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(path);
        if (profile.allows(requestedMode)) {
            return null;
        }
        ScratchBirdFormDefinition form = ScratchBirdFormRegistry.require(profile.formId());
        return advisory(
            "WARNING",
            "PRS_JV3_L021",
            statement.span(),
            "Statement " + statementNumber + " requests " + requestedMode.name().toLowerCase(Locale.ROOT) + " semantics on "
                + path + ", but " + profile.id() + " is a " + profile.mutationSummary() + "; use "
                + form.id() + " (" + form.name() + ") for the admitted route.");
    }

    @Nullable
    private static String domainCollectionPathHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String path,
        @Nullable ScratchBirdFormMode requestedMode
    ) {
        if (requestedMode == null || requestedMode == ScratchBirdFormMode.CREATE || !isDomainSurface(statement)) {
            return null;
        }
        if (!"sys.domains".equals(normalizePath(path))) {
            return null;
        }
        return advisory(
            "WARNING",
            "PRS_JV3_L037",
            statement.span(),
            "Statement " + statementNumber + " targets domain lifecycle on " + normalizePath(path)
                + ", but that path is the domain hub collection surface itself; retarget to a concrete domain path before relying on lifecycle review or execution.");
    }

    @Nullable
    private static String namespaceCollectionPathHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String path,
        @Nullable ScratchBirdFormMode requestedMode
    ) {
        if (requestedMode == null || requestedMode == ScratchBirdFormMode.CREATE) {
            return null;
        }
        String family = objectFamily(statement);
        if (family == null) {
            return null;
        }
        String surfaceLabel = namespaceCollectionLabel(path);
        if (surfaceLabel == null) {
            return null;
        }
        return advisory(
            "WARNING",
            "PRS_JV3_L035",
            statement.span(),
            "Statement " + statementNumber + " targets " + familyLabel(family) + " on " + path
                + ", but that path is the " + surfaceLabel
                + " collection surface itself; retarget to a concrete child object path before relying on lifecycle review or execution.");
    }

    @Nullable
    private static String specialSurfaceCollectionPathHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String path,
        @Nullable ScratchBirdFormMode requestedMode
    ) {
        if (requestedMode == null || requestedMode == ScratchBirdFormMode.CREATE) {
            return null;
        }
        String family = objectFamily(statement);
        if (family == null) {
            return null;
        }
        String surfaceLabel = specialSurfaceCollectionLabel(path);
        if (surfaceLabel == null) {
            return null;
        }
        return advisory(
            "WARNING",
            "PRS_JV3_L032",
            statement.span(),
            "Statement " + statementNumber + " targets " + familyLabel(family) + " on " + path
                + ", but that path is the " + surfaceLabel + " collection surface itself; retarget to a concrete child object path before relying on lifecycle review or execution.");
    }

    @Nullable
    private static String pathObjectFamilyMismatchHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String path
    ) {
        String family = objectFamily(statement);
        if (family == null) {
            return null;
        }
        ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(path);
        if (profileAdmitsObjectFamily(profile, family)) {
            return null;
        }
        return advisory(
            "WARNING",
            "PRS_JV3_L031",
            statement.span(),
            "Statement " + statementNumber + " targets " + familyLabel(family) + " on " + path
                + ", but " + profile.id() + " expects " + fieldPreview(profile.expectedChildren(), 4)
                + "; move this lifecycle SQL to a ScratchBird branch that admits "
                + familyLabel(family) + " children.");
    }

    @Nullable
    private static String domainPlacementHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String path
    ) {
        if (statement.family() != ScratchBirdV3StatementFamily.DDL
            || !statement.surface().toUpperCase(Locale.ROOT).contains("DOMAIN")
            || isWithinPath(path, "sys.domains")) {
            return null;
        }
        return advisory(
            "WARNING",
            "PRS_JV3_L022",
            statement.span(),
                "Statement " + statementNumber + " places a domain surface under " + path
                    + "; canonical ScratchBird domains live under sys.domains.");
    }

    @Nullable
    private static String dmlPathMutationHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String path
    ) {
        if (!isDmlMutation(statement)) {
            return null;
        }
        String surfaceLabel = protectedDmlSurfaceLabel(path);
        if (surfaceLabel == null) {
            return null;
        }
        ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(path);
        ScratchBirdFormDefinition form = ScratchBirdFormRegistry.require(profile.formId());
        return advisory(
            "WARNING",
            "PRS_JV3_L039",
            statement.span(),
            "Statement " + statementNumber + " uses DML mutation against " + normalizePath(path)
                + ", which is a " + surfaceLabel + " routed through " + profile.id()
                + " and " + form.id() + " (" + form.name()
                + "); use the admitted form, task, or server-authoritative procedure instead of relying on raw DML review.");
    }

    @Nullable
    private static String dmlTargetMutationHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String targetPath
    ) {
        if (!isDmlMutation(statement)) {
            return null;
        }
        String surfaceLabel = protectedDmlSurfaceLabel(targetPath);
        if (surfaceLabel == null) {
            return null;
        }
        ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(targetPath);
        ScratchBirdFormDefinition form = ScratchBirdFormRegistry.require(profile.formId());
        return advisory(
            "WARNING",
            "PRS_JV3_L040",
            statement.span(),
            "Statement " + statementNumber + " is being reviewed as DML mutation from selected target "
                + normalizePath(targetPath) + ", which is a " + surfaceLabel + " routed through "
                + profile.id() + " and " + form.id() + " (" + form.name()
                + "); retarget to a concrete mutable data object or use the admitted management form/task flow.");
    }

    @Nullable
    private static String dmlCollectionPathHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String path
    ) {
        if (!isDmlMutation(statement)) {
            return null;
        }
        String surfaceLabel = dmlCollectionSurfaceLabel(path);
        if (surfaceLabel == null) {
            return null;
        }
        return advisory(
            "WARNING",
            "PRS_JV3_L041",
            statement.span(),
            "Statement " + statementNumber + " uses DML mutation against " + normalizePath(path)
                + ", but that path is the " + surfaceLabel
                + "; retarget to a concrete mutable child object path before relying on DML review or execution.");
    }

    @Nullable
    private static String dmlCollectionTargetHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String targetPath
    ) {
        if (!isDmlMutation(statement)) {
            return null;
        }
        String normalizedTarget = normalizePath(targetPath);
        String surfaceLabel = dmlCollectionSurfaceLabel(normalizedTarget);
        if (surfaceLabel == null) {
            return null;
        }
        String objectPath = firstReferencedPath(statement);
        if (objectPath != null
            && isWithinPath(objectPath, normalizedTarget)
            && !normalizedTarget.equals(normalizePath(objectPath))) {
            return null;
        }
        return advisory(
            "WARNING",
            "PRS_JV3_L042",
            statement.span(),
            "Statement " + statementNumber + " is being reviewed as DML mutation from selected target "
                + normalizedTarget + ", but that selected target is the " + surfaceLabel
                + "; select a concrete mutable child object or fully qualify the DML target before relying on form, authz-probe, or live-probe results.");
    }

    @Nullable
    private static String unrootedDmlMutationHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull List<String> referencedPaths,
        @Nullable String targetPath
    ) {
        if (!isDmlMutation(statement) || !referencedPaths.isEmpty()) {
            return null;
        }
        if (targetPath != null && !targetPath.isBlank()) {
            return null;
        }
        return advisory(
            "WARNING",
            "PRS_JV3_L043",
            statement.span(),
            "Statement " + statementNumber
                + " does not declare a rooted ScratchBird DML target and no selected object context is available; fully qualify the mutation target under a concrete data.*, users.*, cluster.*, remote.*, or emulated.* object path before relying on review or execution.");
    }

    @Nullable
    private static String unrootedDclObjectHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull List<String> referencedPaths,
        @Nullable String targetPath
    ) {
        if (!isSecurityDclObjectReview(statement) || !referencedPaths.isEmpty()) {
            return null;
        }
        if (targetPath != null && !targetPath.isBlank()) {
            return null;
        }
        return advisory(
            "WARNING",
            "PRS_JV3_L044",
            statement.span(),
            "Statement " + statementNumber
                + " does not declare a rooted ScratchBird securable object and no selected object context is available; fully qualify the ON target under a concrete data.*, users.*, cluster.*, remote.*, emulated.*, or sys.* object path before relying on security review. Keep principal and grant posture review anchored on sys.security.");
    }

    @Nullable
    private static String dclCollectionPathHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String path
    ) {
        if (!isSecurityDclObjectReview(statement)) {
            return null;
        }
        String surfaceLabel = dclCollectionSurfaceLabel(path);
        if (surfaceLabel == null) {
            return null;
        }
        return advisory(
            "WARNING",
            "PRS_JV3_L045",
            statement.span(),
            "Statement " + statementNumber + " grants or revokes object privileges on " + normalizePath(path)
                + ", but that path is the " + surfaceLabel
                + "; retarget to a concrete securable child object before relying on security review or execution.");
    }

    @Nullable
    private static String dclCollectionTargetHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String targetPath
    ) {
        if (!isSecurityDclObjectReview(statement)) {
            return null;
        }
        String normalizedTarget = normalizePath(targetPath);
        String surfaceLabel = dclCollectionSurfaceLabel(normalizedTarget);
        if (surfaceLabel == null) {
            return null;
        }
        String objectPath = firstDclObjectPath(statement);
        if (objectPath != null
            && isWithinPath(objectPath, normalizedTarget)
            && !normalizedTarget.equals(normalizePath(objectPath))) {
            return null;
        }
        return advisory(
            "WARNING",
            "PRS_JV3_L046",
            statement.span(),
            "Statement " + statementNumber + " is being reviewed for object privileges from selected target "
                + normalizedTarget + ", but that selected target is the " + surfaceLabel
                + "; select a concrete securable child object or fully qualify the ON target before relying on security review, authz-probe, or live-probe results.");
    }

    @Nullable
    private static String dclBroadSurfacePathHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String path
    ) {
        if (!isSecurityDclObjectReview(statement)) {
            return null;
        }
        String surfaceLabel = dclBroadSecurableSurfaceLabel(path);
        if (surfaceLabel == null) {
            return null;
        }
        return advisory(
            "WARNING",
            "PRS_JV3_L047",
            statement.span(),
            "Statement " + statementNumber + " grants or revokes object privileges on " + normalizePath(path)
                + ", but that path is the " + surfaceLabel
                + "; route broad grant posture through sys.security and SBDV-FRM-110, and retarget object privileges to a concrete securable child.");
    }

    @Nullable
    private static String dclBroadSurfaceTargetHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String targetPath
    ) {
        if (!isSecurityDclObjectReview(statement)) {
            return null;
        }
        String normalizedTarget = normalizePath(targetPath);
        String surfaceLabel = dclBroadSecurableSurfaceLabel(normalizedTarget);
        if (surfaceLabel == null) {
            return null;
        }
        String objectPath = firstDclObjectPath(statement);
        if (objectPath != null
            && isWithinPath(objectPath, normalizedTarget)
            && !normalizedTarget.equals(normalizePath(objectPath))) {
            return null;
        }
        return advisory(
            "WARNING",
            "PRS_JV3_L048",
            statement.span(),
            "Statement " + statementNumber + " is being reviewed for object privileges from selected target "
                + normalizedTarget + ", but that selected target is the " + surfaceLabel
                + "; select a concrete securable child or fully qualify the ON target before relying on security review, authz-probe, or live-probe results.");
    }

    @Nullable
    private static String dclBroadPrincipalPathHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String path
    ) {
        if (!isDclGrantOrRevoke(statement)) {
            return null;
        }
        String surfaceLabel = dclBroadPrincipalSurfaceLabel(path);
        if (surfaceLabel == null) {
            return null;
        }
        return advisory(
            "WARNING",
            "PRS_JV3_L049",
            statement.span(),
            "Statement " + statementNumber + " grants or revokes privileges for " + normalizePath(path)
                + ", but that principal path is the " + surfaceLabel
                + "; route principal selection through sys.security and SBDV-FRM-110, and name a concrete user, role, group, or policy principal before relying on security review.");
    }

    @Nullable
    private static String dclBroadPrincipalTargetHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String targetPath
    ) {
        if (!isDclGrantOrRevoke(statement)) {
            return null;
        }
        String normalizedTarget = normalizePath(targetPath);
        String surfaceLabel = dclBroadPrincipalSurfaceLabel(normalizedTarget);
        if (surfaceLabel == null) {
            return null;
        }
        String principalPath = firstDclPrincipalPath(statement);
        if (principalPath != null
            && isWithinPath(principalPath, normalizedTarget)
            && !normalizedTarget.equals(normalizePath(principalPath))) {
            return null;
        }
        return advisory(
            "WARNING",
            "PRS_JV3_L050",
            statement.span(),
            "Statement " + statementNumber + " is being reviewed for grant principal posture from selected target "
                + normalizedTarget + ", but that selected target is the " + surfaceLabel
                + "; select a concrete user, role, group, or policy principal, or keep broad grant posture review on sys.security and SBDV-FRM-110.");
    }

    @NotNull
    private static String metadataTransactionPublicationHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        int transactionStatementNumber,
        @NotNull ScratchBirdV3StatementKind transactionKind
    ) {
        return advisory(
            "INFO",
            "PRS_JV3_L051",
            statement.span(),
            "Statement " + statementNumber + " changes ScratchBird schema or security metadata in a batch that also contains "
                + transactionKind.name().toLowerCase(Locale.ROOT).replace('_', ' ')
                + " transaction control at statement " + transactionStatementNumber
                + "; schema/security publication is MGA commit-bound, parser-assist deltas are committed-only, and DBeaver form/probe hints remain pre-execution advisory until the server transaction reaches commit or rollback.");
    }

    @NotNull
    private static String metadataDualAnchorHint(
        @NotNull ScratchBirdV3Statement statement,
        int schemaStatementNumber,
        int securityStatementNumber
    ) {
        return advisory(
            "WARNING",
            "PRS_JV3_L052",
            statement.span(),
            "Statements " + schemaStatementNumber + " and " + securityStatementNumber
                + " mix schema and security metadata mutation in one review batch; ScratchBird must advance schema epoch and security policy epoch consistently at commit, while rollback or failed autocommit must not publish parser deltas. Review the object route plus sys.security/SBDV-FRM-110 before execution.");
    }

    @NotNull
    private static String metadataReadAfterUncommittedMutationHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        int mutationStatementNumber
    ) {
        return advisory(
            "WARNING",
            "PRS_JV3_L053",
            statement.span(),
            "Statement " + statementNumber + " reads catalog or parser-assist metadata after uncommitted metadata mutation at statement "
                + mutationStatementNumber
                + "; parser normalization starts from the committed catalog baseline, transaction-local overlays must remain session-local, and DBeaver review should rely on live server results until commit or rollback publishes the next parser-assist delta.");
    }

    @Nullable
    private static String rootedLifecycleHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull List<String> referencedPaths,
        @Nullable String targetPath,
        @Nullable ScratchBirdFormMode requestedMode
    ) {
        if (requestedMode == null
            || !requiresRootedLifecyclePath(statement)
            || !referencedPaths.isEmpty()) {
            return null;
        }
        String targetNote = targetPath == null || targetPath.isBlank()
            ? ""
            : " Selected target: " + normalizePath(targetPath) + ".";
        return advisory(
            "WARNING",
            "PRS_JV3_L027",
            statement.span(),
            "Statement " + statementNumber + " does not declare a rooted ScratchBird path for this lifecycle surface; fully qualify it under data.*, users.*, cluster.*, remote.*, emulated.*, or sys.domains before execution."
                + targetNote);
    }

    private static boolean requiresRootedLifecyclePath(@NotNull ScratchBirdV3Statement statement) {
        if (statement.family() != ScratchBirdV3StatementFamily.DDL) {
            return false;
        }
        String surface = statement.surface().toUpperCase(Locale.ROOT);
        return surface.contains("TABLE")
            || surface.contains("VIEW")
            || surface.contains("DOMAIN")
            || surface.contains("SEQUENCE")
            || surface.contains("GENERATOR")
            || surface.contains("PROCEDURE")
            || surface.contains("FUNCTION")
            || surface.contains("TRIGGER")
            || surface.contains("INDEX")
            || surface.contains("CONSTRAINT")
            || surface.contains("SCHEMA");
    }

    @Nullable
    private static String targetPolicyHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String targetPath,
        @Nullable ScratchBirdFormMode requestedMode
    ) {
        if (requestedMode == null) {
            return null;
        }
        if (ScratchBirdNamespaceSemantics.isMetricsPath(targetPath)) {
            return advisory(
                "WARNING",
                "PRS_JV3_L023",
                statement.span(),
                "Statement " + statementNumber + " is being reviewed from " + normalizePath(targetPath)
                    + ", which is a client-only metrics surface; use report and source-status flows instead of direct lifecycle SQL.");
        }
        ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(targetPath);
        if (profile.allows(requestedMode)) {
            return null;
        }
        ScratchBirdFormDefinition form = ScratchBirdFormRegistry.require(profile.formId());
        return advisory(
            "WARNING",
            "PRS_JV3_L024",
            statement.span(),
            "Statement " + statementNumber + " requests " + requestedMode.name().toLowerCase(Locale.ROOT)
                + " semantics while the selected target " + normalizePath(targetPath) + " routes to "
                + profile.id() + " as a " + profile.mutationSummary() + "; review or retarget via "
                + form.id() + " (" + form.name() + ").");
    }

    @Nullable
    private static String domainCollectionTargetHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String targetPath,
        @Nullable ScratchBirdFormMode requestedMode
    ) {
        if (requestedMode == null || requestedMode == ScratchBirdFormMode.CREATE || !isDomainSurface(statement)) {
            return null;
        }
        String normalizedTarget = normalizePath(targetPath);
        if (!"sys.domains".equals(normalizedTarget)) {
            return null;
        }
        String objectPath = firstReferencedPath(statement);
        if (objectPath != null
            && isWithinPath(objectPath, normalizedTarget)
            && !normalizedTarget.equals(normalizePath(objectPath))) {
            return null;
        }
        return advisory(
            "WARNING",
            "PRS_JV3_L038",
            statement.span(),
            "Statement " + statementNumber + " is being reviewed from " + normalizedTarget
                + ", but that selected target is the domain hub collection surface; retarget to the concrete domain before relying on form, authz-probe, or live-probe results.");
    }

    @Nullable
    private static String namespaceCollectionTargetHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String targetPath,
        @Nullable ScratchBirdFormMode requestedMode
    ) {
        if (requestedMode == null || requestedMode == ScratchBirdFormMode.CREATE) {
            return null;
        }
        String family = objectFamily(statement);
        if (family == null) {
            return null;
        }
        String surfaceLabel = namespaceCollectionLabel(targetPath);
        if (surfaceLabel == null) {
            return null;
        }
        String objectPath = firstReferencedPath(statement);
        if (objectPath != null && isWithinPath(objectPath, normalizePath(targetPath))) {
            return null;
        }
        return advisory(
            "WARNING",
            "PRS_JV3_L036",
            statement.span(),
            "Statement " + statementNumber + " is being reviewed from " + normalizePath(targetPath)
                + " as " + familyLabel(family) + ", but that selected target is the "
                + surfaceLabel + " collection surface; retarget to the concrete child object before relying on form, authz-probe, or live-probe results.");
    }

    @Nullable
    private static String parentObjectTargetHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String targetPath
    ) {
        if (statement.family() != ScratchBirdV3StatementFamily.DDL) {
            return null;
        }
        String objectPath = firstReferencedPath(statement);
        if (objectPath == null || objectPath.isBlank()) {
            return null;
        }
        String normalizedTarget = normalizePath(targetPath);
        String normalizedObject = normalizePath(objectPath);
        if (normalizedTarget.equals(normalizedObject)
            || !isWithinPath(normalizedObject, normalizedTarget)) {
            return null;
        }
        return advisory(
            "INFO",
            "PRS_JV3_L034",
            statement.span(),
            "Statement " + statementNumber + " names concrete ScratchBird object " + normalizedObject
                + ", but the selected target is parent surface " + normalizedTarget
                + "; retarget object review to " + normalizedObject
                + " before relying on object-editor, authz-probe, or live-probe results.");
    }

    @Nullable
    private static String specialSurfaceCollectionTargetHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String targetPath,
        @Nullable ScratchBirdFormMode requestedMode
    ) {
        if (requestedMode == null || requestedMode == ScratchBirdFormMode.CREATE) {
            return null;
        }
        String family = objectFamily(statement);
        if (family == null) {
            return null;
        }
        String surfaceLabel = specialSurfaceCollectionLabel(targetPath);
        if (surfaceLabel == null) {
            return null;
        }
        return advisory(
            "WARNING",
            "PRS_JV3_L033",
            statement.span(),
            "Statement " + statementNumber + " is being reviewed from " + normalizePath(targetPath)
                + " as " + familyLabel(family) + ", but that selected target is the "
                + surfaceLabel + " collection surface; retarget to the concrete child object before relying on form, authz-probe, or live-probe results.");
    }

    @Nullable
    private static String targetObjectFamilyMismatchHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String targetPath,
        @NotNull List<String> referencedPaths
    ) {
        String family = objectFamily(statement);
        if (family == null) {
            return null;
        }
        String normalizedTarget = normalizePath(targetPath);
        for (String referencedPath : referencedPaths) {
            if (isWithinPath(referencedPath, normalizedTarget) || isWithinPath(normalizedTarget, referencedPath)) {
                return null;
            }
        }
        ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(targetPath);
        if (profileAdmitsObjectFamily(profile, family)) {
            return null;
        }
        return advisory(
            "WARNING",
            "PRS_JV3_L030",
            statement.span(),
            "Statement " + statementNumber + " is being reviewed from " + normalizedTarget
                + " as " + familyLabel(family) + ", but " + profile.id() + " expects "
                + fieldPreview(profile.expectedChildren(), 4)
                + "; retarget the selected ScratchBird surface before relying on lifecycle review or execution.");
    }

    @Nullable
    private static String derivedTargetScopeHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String targetPath,
        @NotNull List<String> referencedPaths
    ) {
        String derivedPath = derivedProbePath(statement);
        if (derivedPath == null) {
            return null;
        }
        String normalizedTarget = normalizePath(targetPath);
        if (isWithinPath(derivedPath, normalizedTarget) || isWithinPath(normalizedTarget, derivedPath)) {
            return null;
        }
        ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(derivedPath);
        ScratchBirdFormDefinition form = ScratchBirdFormRegistry.require(profile.formId());
        if (referencedPaths.isEmpty()) {
            return advisory(
                "WARNING",
                "PRS_JV3_L028",
                statement.span(),
                "Statement " + statementNumber + " has canonical ScratchBird review scope " + derivedPath
                    + " via " + profile.id() + " and " + form.id() + " (" + form.name()
                    + "), but the selected target is " + normalizedTarget
                    + "; retarget before relying on form, authz-probe, or live-probe results.");
        }
        return advisory(
            "INFO",
            "PRS_JV3_L029",
            statement.span(),
            "Statement " + statementNumber + " references " + pathPreview(referencedPaths, 2)
                + " but also has canonical ScratchBird review scope " + derivedPath + " via "
                + profile.id() + " and " + form.id() + " (" + form.name()
                + "); keep object qualification in SQL, but review administrative authz/probe state from "
                + derivedPath + " instead of only " + normalizedTarget + ".");
    }

    @Nullable
    private static String targetMismatchHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String targetPath,
        @NotNull List<String> referencedPaths
    ) {
        if (referencedPaths.isEmpty()) {
            return null;
        }
        String normalizedTarget = normalizePath(targetPath);
        for (String referencedPath : referencedPaths) {
            if (isWithinPath(referencedPath, normalizedTarget) || isWithinPath(normalizedTarget, referencedPath)) {
                continue;
            }
            return advisory(
                "INFO",
                "PRS_JV3_L025",
                statement.span(),
                "Statement " + statementNumber + " references " + referencedPath + " while the selected target is "
                    + normalizedTarget + "; retarget the form or fully qualify the intended branch before execution.");
        }
        return null;
    }

    @Nullable
    private static String targetDomainPlacementHint(
        @NotNull ScratchBirdV3Statement statement,
        int statementNumber,
        @NotNull String targetPath
    ) {
        if (statement.family() != ScratchBirdV3StatementFamily.DDL
            || !statement.surface().toUpperCase(Locale.ROOT).contains("DOMAIN")
            || isWithinPath(targetPath, "sys.domains")) {
            return null;
        }
        return advisory(
            "WARNING",
            "PRS_JV3_L026",
            statement.span(),
            "Statement " + statementNumber + " is a domain lifecycle command, but the selected target "
                + normalizePath(targetPath) + " is outside sys.domains; retarget the canonical domain branch before execution.");
    }

    private static boolean isWithinPath(@NotNull String candidate, @NotNull String scope) {
        String normalizedCandidate = normalizePath(candidate);
        String normalizedScope = normalizePath(scope);
        return normalizedCandidate.equals(normalizedScope)
            || normalizedCandidate.startsWith(normalizedScope + ".");
    }

    @Nullable
    private static String namespaceCollectionLabel(@NotNull String path) {
        String normalizedPath = normalizePath(path);
        ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(normalizedPath);
        if ("SBBP-DATA-NAMESPACE".equals(profile.id())
            && ScratchBirdNamespaceSemantics.getPathDepth(normalizedPath) == 2) {
            return "data namespace";
        }
        return null;
    }

    @Nullable
    private static String specialSurfaceCollectionLabel(@NotNull String path) {
        String normalizedPath = normalizePath(path);
        ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(normalizedPath);
        if ("SBBP-USER-SCRATCH".equals(profile.id()) && normalizedPath.endsWith(".scratch")) {
            return "scratch workspace";
        }
        if ("SBBP-GROUP-PUBLIC".equals(profile.id()) && normalizedPath.endsWith(".public")) {
            return "group public";
        }
        return null;
    }

    private static boolean isDomainSurface(@NotNull ScratchBirdV3Statement statement) {
        return statement.family() == ScratchBirdV3StatementFamily.DDL
            && statement.surface().toUpperCase(Locale.ROOT).contains("DOMAIN");
    }

    private static boolean isDmlMutation(@NotNull ScratchBirdV3Statement statement) {
        return switch (statement.kind()) {
            case INSERT, UPDATE, UPDATE_OR_INSERT, DELETE, MERGE, COPY -> true;
            default -> false;
        };
    }

    private static boolean isSecurityDclObjectReview(@NotNull ScratchBirdV3Statement statement) {
        return isDclGrantOrRevoke(statement)
            && containsKeyword(statement, "ON");
    }

    private static boolean isDclGrantOrRevoke(@NotNull ScratchBirdV3Statement statement) {
        return statement.kind() == ScratchBirdV3StatementKind.GRANT
            || statement.kind() == ScratchBirdV3StatementKind.REVOKE;
    }

    private static boolean isTransactionControl(@NotNull ScratchBirdV3Statement statement) {
        return statement.family() == ScratchBirdV3StatementFamily.TRANSACTION;
    }

    private static boolean isTransactionStart(@NotNull ScratchBirdV3Statement statement) {
        return statement.kind() == ScratchBirdV3StatementKind.BEGIN
            || statement.kind() == ScratchBirdV3StatementKind.START;
    }

    private static boolean isTransactionEnd(@NotNull ScratchBirdV3Statement statement) {
        return statement.kind() == ScratchBirdV3StatementKind.COMMIT
            || statement.kind() == ScratchBirdV3StatementKind.ROLLBACK;
    }

    private static boolean isMetadataPublicationMutation(@NotNull ScratchBirdV3Statement statement) {
        return isSchemaPublicationMutation(statement) || isSecurityPublicationMutation(statement);
    }

    private static boolean isSchemaPublicationMutation(@NotNull ScratchBirdV3Statement statement) {
        return statement.family() == ScratchBirdV3StatementFamily.DDL && !isSecurityPublicationMutation(statement);
    }

    private static boolean isSecurityPublicationMutation(@NotNull ScratchBirdV3Statement statement) {
        if (isDclGrantOrRevoke(statement) || statement.kind() == ScratchBirdV3StatementKind.SECURITY_LABEL) {
            return true;
        }
        if (statement.family() != ScratchBirdV3StatementFamily.DDL) {
            return false;
        }
        String ddlObject = ddlObjectSurface(statement);
        return "USER".equals(ddlObject)
            || "ROLE".equals(ddlObject)
            || "GROUP".equals(ddlObject)
            || "POLICY".equals(ddlObject)
            || "TOKEN".equals(ddlObject)
            || "CONNECTION RULE".equals(ddlObject);
    }

    private static boolean isMetadataReadStatement(@NotNull ScratchBirdV3Statement statement) {
        if (statement.kind() == ScratchBirdV3StatementKind.SHOW
            || statement.kind() == ScratchBirdV3StatementKind.DESCRIBE) {
            return true;
        }
        if (statement.kind() != ScratchBirdV3StatementKind.SELECT
            && statement.kind() != ScratchBirdV3StatementKind.WITH) {
            return false;
        }
        for (String path : referencedPaths(statement)) {
            if (isWithinPath(path, "sys.catalog")
                || isWithinPath(path, "sys.security")
                || isWithinPath(path, "sys.domains")) {
                return true;
            }
        }
        return false;
    }

    @Nullable
    private static String protectedDmlSurfaceLabel(@NotNull String path) {
        String normalizedPath = normalizePath(path);
        if (ScratchBirdNamespaceSemantics.isMetricsPath(normalizedPath)) {
            return "client-only metrics surface";
        }
        if (normalizedPath.equals("sys") || normalizedPath.startsWith("sys.")) {
            ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(normalizedPath);
            if (profile.inspectOnly()) {
                return "inspect-only system surface";
            }
            if (!profile.createAllowed() && !profile.alterAllowed() && !profile.deleteAllowed()) {
                return "non-mutating system control surface";
            }
            return "server-controlled system lifecycle surface";
        }
        if (normalizedPath.equals("cluster") || normalizedPath.startsWith("cluster.")) {
            return "cluster control surface";
        }
        if (normalizedPath.equals("emulated") || normalizedPath.startsWith("emulated.")) {
            return "emulation control surface";
        }
        if (normalizedPath.equals("remote") || normalizedPath.startsWith("remote.")) {
            return "remote connector control surface";
        }
        return null;
    }

    @Nullable
    private static String dmlCollectionSurfaceLabel(@NotNull String path) {
        String normalizedPath = normalizePath(path);
        if ("data".equals(normalizedPath)) {
            return "data root collection surface";
        }
        String namespaceLabel = namespaceCollectionLabel(normalizedPath);
        if (namespaceLabel != null) {
            return namespaceLabel + " collection surface";
        }
        String specialSurfaceLabel = specialSurfaceCollectionLabel(normalizedPath);
        if (specialSurfaceLabel != null) {
            return specialSurfaceLabel + " collection surface";
        }
        return null;
    }

    @Nullable
    private static String dclCollectionSurfaceLabel(@NotNull String path) {
        String normalizedPath = normalizePath(path);
        if ("sys.domains".equals(normalizedPath)) {
            return "domain hub securable collection surface";
        }
        String dmlSurface = dmlCollectionSurfaceLabel(normalizedPath);
        if (dmlSurface != null) {
            return dmlSurface.replace("collection surface", "securable collection surface");
        }
        return null;
    }

    @Nullable
    private static String dclBroadSecurableSurfaceLabel(@NotNull String path) {
        String normalizedPath = normalizePath(path);
        if (ScratchBirdNamespaceSemantics.isMetricsPath(normalizedPath)) {
            return "client-only metrics report surface";
        }
        if ("sys".equals(normalizedPath)) {
            return "system root management surface";
        }
        if ("users".equals(normalizedPath)) {
            return "principal root collection surface";
        }
        if ("cluster".equals(normalizedPath)) {
            return "cluster root management surface";
        }
        if ("emulated".equals(normalizedPath)) {
            return "emulated-engine root collection surface";
        }
        if ("remote".equals(normalizedPath)) {
            return "remote connector root collection surface";
        }
        if (normalizedPath.equals("users.public")
            || normalizedPath.equals("users.home")
            || normalizedPath.equals("users.groups")) {
            return "principal collection surface";
        }
        if ((normalizedPath.startsWith("sys.") && ScratchBirdNamespaceSemantics.getPathDepth(normalizedPath) == 2
                && !"sys.domains".equals(normalizedPath))
            || (normalizedPath.startsWith("cluster.") && ScratchBirdNamespaceSemantics.getPathDepth(normalizedPath) == 2)
            || normalizedPath.equals("remote.links")
            || normalizedPath.equals("remote.fdw")
            || (normalizedPath.startsWith("emulated.") && ScratchBirdNamespaceSemantics.getPathDepth(normalizedPath) == 2)) {
            return "administrative collection surface";
        }
        return null;
    }

    @Nullable
    private static String dclBroadPrincipalSurfaceLabel(@NotNull String path) {
        String normalizedPath = normalizePath(path);
        if ("users".equals(normalizedPath)) {
            return "principal root surface";
        }
        if ("users.public".equals(normalizedPath)) {
            return "public principal collection surface";
        }
        if ("users.home".equals(normalizedPath)) {
            return "home principal collection surface";
        }
        if ("users.groups".equals(normalizedPath)) {
            return "group principal collection surface";
        }
        if ("sys.security".equals(normalizedPath)) {
            return "security administration collection surface";
        }
        return null;
    }

    @Nullable
    private static String objectFamily(@NotNull ScratchBirdV3Statement statement) {
        String surface = statement.surface().toUpperCase(Locale.ROOT);
        if (surface.contains("DOMAIN") || surface.contains("SCHEMA")) {
            return null;
        }
        if (surface.contains("TRIGGER") || surface.contains("INDEX")
            || surface.contains("CONSTRAINT") || surface.contains("FOREIGN KEY")
            || surface.contains("UNIQUE KEY")) {
            return "table-affiliated object";
        }
        if (surface.contains("PROCEDURE") || surface.contains("FUNCTION")) {
            return "routine";
        }
        if (surface.contains("SEQUENCE") || surface.contains("GENERATOR")) {
            return "sequence";
        }
        if (surface.contains("VIEW")) {
            return "view";
        }
        if (surface.contains("TABLE")) {
            return "table";
        }
        return null;
    }

    @NotNull
    private static String familyLabel(@NotNull String family) {
        return family;
    }

    private static boolean profileAdmitsObjectFamily(
        @NotNull ScratchBirdBranchProfile profile,
        @NotNull String family
    ) {
        return profileAdmissionDescriptor(profile, family) != null;
    }

    @Nullable
    private static String profileAdmissionDescriptor(
        @NotNull ScratchBirdBranchProfile profile,
        @NotNull String family
    ) {
        List<String> expectedChildren = profile.expectedChildren().stream()
            .map(value -> value.toLowerCase(Locale.ROOT))
            .toList();
        return switch (family) {
            case "table" -> firstMatchingDescriptor(expectedChildren,
                "table", "workspace object", "published object", "managed object", "reference-native object");
            case "view" -> firstMatchingDescriptor(expectedChildren,
                "view", "workspace object", "published object", "managed object", "reference-native object");
            case "sequence" -> firstMatchingDescriptor(expectedChildren,
                "sequence", "workspace object", "published object", "managed object", "reference-native object");
            case "routine" -> firstMatchingDescriptor(expectedChildren,
                "routine", "workspace object", "published object", "managed object", "reference-native object");
            case "table-affiliated object" -> firstMatchingDescriptor(expectedChildren,
                "table", "workspace object", "published object", "managed object", "reference-native object");
            default -> null;
        };
    }

    @Nullable
    private static String firstMatchingDescriptor(
        @NotNull List<String> expectedChildren,
        @NotNull String... candidates
    ) {
        for (String candidate : candidates) {
            for (String expectedChild : expectedChildren) {
                if (expectedChild.contains(candidate)) {
                    return candidate;
                }
            }
        }
        return null;
    }

    @Nullable
    private static String objectFormId(
        @NotNull ScratchBirdV3Statement statement,
        @NotNull String sql
    ) {
        String surface = statement.surface().toUpperCase(Locale.ROOT);
        if (surface.contains("DOMAIN")) {
            return "SBDV-FRM-611";
        }
        if (surface.contains("TRIGGER")) {
            return "SBDV-FRM-610";
        }
        if (surface.contains("PROCEDURE") || surface.contains("FUNCTION")) {
            return "SBDV-FRM-609";
        }
        if (surface.contains("SEQUENCE") || surface.contains("GENERATOR")) {
            return "SBDV-FRM-608";
        }
        if (surface.contains("INDEX")) {
            return "SBDV-FRM-607";
        }
        if (surface.contains("CONSTRAINT") || surface.contains("FOREIGN KEY") || surface.contains("UNIQUE KEY")) {
            return "SBDV-FRM-606";
        }
        if (surface.contains("TABLE")) {
            return isLikelyNonRelationalSql(sql, statement) ? "SBDV-FRM-602" : "SBDV-FRM-601";
        }
        if (surface.contains("VIEW")) {
            return isLikelyNonRelationalSql(sql, statement) ? "SBDV-FRM-604" : "SBDV-FRM-603";
        }
        return null;
    }

    private static boolean isLikelyNonRelationalSql(
        @NotNull String sql,
        @NotNull ScratchBirdV3Statement statement
    ) {
        String statementText = statementText(sql, statement).toUpperCase(Locale.ROOT);
        for (String marker : NON_RELATIONAL_SQL_MARKERS) {
            if (statementText.contains(marker)) {
                return true;
            }
        }
        return false;
    }

    @Nullable
    private static String firstReferencedPath(@NotNull ScratchBirdV3Statement statement) {
        List<String> paths = referencedPaths(statement);
        return paths.isEmpty() ? null : paths.getFirst();
    }

    @Nullable
    private static String firstDclObjectPath(@NotNull ScratchBirdV3Statement statement) {
        List<String> paths = dclObjectPaths(statement);
        return paths.isEmpty() ? null : paths.getFirst();
    }

    @Nullable
    private static String firstDclPrincipalPath(@NotNull ScratchBirdV3Statement statement) {
        List<String> paths = dclPrincipalPaths(statement);
        return paths.isEmpty() ? null : paths.getFirst();
    }

    @NotNull
    private static String statementText(
        @NotNull String sql,
        @NotNull ScratchBirdV3Statement statement
    ) {
        int start = Math.max(0, Math.min(statement.span().start().offset(), sql.length()));
        int end = Math.max(start, Math.min(statement.span().endOffset(), sql.length()));
        String text = sql.substring(start, end).trim();
        if (text.isEmpty()) {
            text = statement.surface();
        }
        if (statement.complete() && !text.endsWith(";")) {
            return text + ";";
        }
        return text;
    }

    @NotNull
    private static String routeKey(@NotNull ProbeHintRoute route) {
        return route.form().id() + "|" + route.path() + "|" + route.mode() + "|" + route.objectDerived();
    }

    private record ProbeHintRoute(
        @NotNull String path,
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdFormMode mode,
        @NotNull ScratchBirdAdminExecutor.ExecutionPlan executionPlan,
        @NotNull List<ScratchBirdTaskDefinition> taskDefinitions,
        @Nullable ScratchBirdDestructivePlan destructivePlan,
        boolean objectDerived
    ) {
    }

    private record PathScan(@NotNull String path, int endIndex, boolean rooted) {
    }
}
