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

public final class ScratchBirdEditorPageCatalog {

    public record EditorPage(
        @NotNull String id,
        @NotNull String tabLabel,
        @NotNull String title,
        @NotNull String purpose,
        @NotNull List<String> controls,
        @NotNull List<String> validationWidgets,
        @NotNull List<String> evidenceAnchors
    ) {
        @NotNull
        public List<String> summaryLines() {
            List<String> lines = new ArrayList<>();
            lines.add("Page: " + id + " - " + title);
            lines.add("Purpose: " + purpose);
            lines.add("Controls: " + String.join(", ", controls));
            lines.add("Validation widgets: " + String.join(", ", validationWidgets));
            lines.add("Evidence anchors: " + String.join(", ", evidenceAnchors));
            return List.copyOf(lines);
        }
    }

    public record EditorPlan(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdFormMode mode,
        @NotNull String targetPath,
        @NotNull List<EditorPage> pages
    ) {
        @NotNull
        public List<String> summaryLines() {
            return pages.stream()
                .map(page -> page.id() + " -> " + page.title())
                .toList();
        }
    }

    private ScratchBirdEditorPageCatalog() {
    }

    @NotNull
    public static EditorPlan planFor(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdFormMode mode,
        @NotNull String targetPath
    ) {
        List<EditorPage> pages = new ArrayList<>();
        pages.add(identityPage(form, targetPath));
        pages.add(primaryPage(form, mode, targetPath));
        if (isLifecycleForm(form)) {
            pages.add(lifecyclePage(form, mode));
        }
        if (isAdministrativeForm(form)) {
            pages.add(administrationPage(form, mode, targetPath));
        }
        if (isReportForm(form)) {
            pages.add(reportPage(form, targetPath));
        }
        if (isDatatypeForm(form)) {
            pages.add(datatypePage(form));
        }
        if ("SBDV-FRM-014".equals(form.id())) {
            pages.add(destructivePage(targetPath));
        }
        if ("SBDV-FRM-016".equals(form.id())) {
            pages.add(taskPage(targetPath));
        }
        pages.add(validationPage(form));
        pages.add(authorityPage(form, mode, targetPath));
        return new EditorPlan(form, mode, targetPath, List.copyOf(pages));
    }

    @NotNull
    private static EditorPage identityPage(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull String targetPath
    ) {
        return page(
            "identity",
            "Identity",
            "Object Identity Page",
            "Displays the selected ScratchBird path, UUID/parentage fields when available, form route, and current persistence posture.",
            List.of("object type", "display path", "canonical path", "object UUID", "parent UUID", "form route"),
            List.of("missing UUID warning", "parentage mismatch warning", "client-only branch marker"),
            List.of(form.id(), targetPath, "sys.catalog.object_resolver"));
    }

    @NotNull
    private static EditorPage primaryPage(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdFormMode mode,
        @NotNull String targetPath
    ) {
        return page(
            "primary-" + form.id().toLowerCase(),
            "Editor",
            form.name() + " Page",
            form.purpose(),
            primaryControls(form, mode),
            List.of("required field status", "server refusal banner", "generated command preview", "dirty-state indicator"),
            List.of(form.id(), mode.name(), targetPath));
    }

    @NotNull
    private static EditorPage lifecyclePage(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdFormMode mode
    ) {
        return page(
            "lifecycle-" + form.id().toLowerCase(),
            "Lifecycle",
            lifecycleFamily(form) + " Lifecycle Page",
            "Provides create, inspect, alter, and delete posture for the specific SQL object family.",
            List.of("definition editor", "child object attachment list", "generated DDL preview", "dependency preview", "mode selector " + mode.name()),
            List.of("Java v3 parser diagnostics", "object-family admission warnings", "selected-target mismatch warnings"),
            List.of("SBDV-FRM-014", "SBDV-FRM-015", "SBDV-FRM-110"));
    }

    @NotNull
    private static EditorPage administrationPage(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdFormMode mode,
        @NotNull String targetPath
    ) {
        ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(targetPath);
        return page(
            "admin-" + form.id().toLowerCase(),
            "Admin",
            profile.label() + " Administration Page",
            "Shows branch-specific administrative actions, child forms, source status, and task routing.",
            List.of("branch posture", "action set", "child surface selector", "task shortcuts", "source status"),
            List.of("capability posture", "server authz probe", "representative read-only probe", "mutation admission probe for " + mode.name()),
            List.of(profile.id(), form.id(), "sys.server_capabilities", "sys.security.permission_probe"));
    }

    @NotNull
    private static EditorPage reportPage(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull String targetPath
    ) {
        return page(
            "report-" + form.id().toLowerCase(),
            "Reports",
            "Metrics, Report, And Alert Page",
            "Renders report source status, chart intent, drilldown fields, and alert starter expressions for metrics/report nodes.",
            List.of("time range", "refresh action", "source preview", "chart intent", "alert starter", "drilldown selector"),
            List.of("missing source banner", "future-gated marker", "raw metric family requirement"),
            List.of(targetPath, "SHOW METRICS", "SBsql metrics snapshot", "SBDV-ALERT-*"));
    }

    @NotNull
    private static EditorPage datatypePage(@NotNull ScratchBirdFormDefinition form) {
        return page(
            "datatype-" + form.id().toLowerCase(),
            "Datatype",
            "Datatype And Value Widget Page",
            "Provides ScratchBird-specific value widgets, literal previews, and roundtrip guidance for typed object fields.",
            List.of("typed value editor", "SQL literal preview", "canonical text view", "domain binding", "copy-as-literal"),
            List.of("roundtrip status", "content-type validation", "null-state validation", "domain constraint warning"),
            List.of("SBDV-FRM-613", "ScratchBirdValueProfile", "ScratchBirdValueBinding"));
    }

    @NotNull
    private static EditorPage destructivePage(@NotNull String targetPath) {
        return page(
            "destructive-flow",
            "Delete",
            "Destructive Action Wizard Page",
            "Requires explicit confirmation, dependency review, dry-run guidance, and rollback guidance before destructive operations.",
            List.of("confirmation phrase", "dependency preview", "dry-run commands", "schedule guidance", "rollback guidance"),
            List.of("confirmation mismatch", "dependency surface unavailable", "server refusal"),
            List.of(targetPath, "SBDV-FRM-014", "sys.catalog.object_resolver"));
    }

    @NotNull
    private static EditorPage taskPage(@NotNull String targetPath) {
        return page(
            "task-wizard",
            "Tasks",
            "Admin Task Wizard Page",
            "Provides preview, validate, execute, schedule guidance, and retained result evidence for admitted task definitions.",
            List.of("task selector", "preview probe", "validate probe", "execute probe", "result log"),
            List.of("task catalog coverage", "read-only safety gate", "server refusal", "history persistence"),
            List.of(targetPath, "SBDV-FRM-016", "ScratchBirdTaskCatalog", "ScratchBirdProbeHistory"));
    }

    @NotNull
    private static EditorPage validationPage(@NotNull ScratchBirdFormDefinition form) {
        return page(
            "validation-" + form.id().toLowerCase(),
            "Validation",
            "Parser Validation And Semantic Lint Page",
            "Displays Java v3 parser diagnostics, statement inventory, advisory semantic lint, context hints, form hints, and quick-fix candidates.",
            List.of("statement inventory", "diagnostics", "lint hints", "context hints", "form hints", "completion hints"),
            List.of("parser advisory boundary", "rooted path lint", "DML/DCL semantic lint", "transaction publication lint"),
            List.of("SBDV-FRM-015", "ScratchBirdValidationBridge", "ScratchBirdSqlQuickAssistProcessor"));
    }

    @NotNull
    private static EditorPage authorityPage(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdFormMode mode,
        @NotNull String targetPath
    ) {
        return page(
            "authority-" + form.id().toLowerCase(),
            "Authority",
            "Server Authority And Evidence Page",
            "Separates client preview from server execution, server authorization, live probes, retained local history, and stock-install evidence.",
            List.of("static posture", "server capability inventory", "mutation permission probe", "live read-only probe", "history selector"),
            List.of("server denied", "missing capability surface", "surrogate probe marker", "unverified stock GUI evidence marker"),
            List.of(form.id(), mode.name(), targetPath, "sys.server_capabilities", "sys.security.permission_probe"));
    }

    @NotNull
    private static List<String> primaryControls(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdFormMode mode
    ) {
        if (form.id().startsWith("SBDV-FRM-01")) {
            return List.of("wizard field matrix", "transport/auth/session pages", "connection test", "server refusal banner");
        }
        if (isReportForm(form)) {
            return List.of("report source selector", "chart/drilldown controls", "alert expression starter", "refresh control");
        }
        if (isDatatypeForm(form)) {
            return List.of("datatype selector", "literal editor", "domain constraints", "roundtrip preview");
        }
        if (isLifecycleForm(form)) {
            return List.of("definition editor", "mode " + mode.name(), "DDL preview", "grant child page", "metrics child page");
        }
        if (isAdministrativeForm(form)) {
            return List.of("branch field matrix", "child object selector", "task shortcuts", "authorization probe");
        }
        return List.of("field matrix", "generated preview", "validation", "review packet");
    }

    private static boolean isLifecycleForm(@NotNull ScratchBirdFormDefinition form) {
        return form.id().matches("SBDV-FRM-60[1-9]") ||
            "SBDV-FRM-610".equals(form.id()) ||
            "SBDV-FRM-611".equals(form.id()) ||
            "SBDV-FRM-612".equals(form.id());
    }

    private static boolean isAdministrativeForm(@NotNull ScratchBirdFormDefinition form) {
        return form.id().equals("SBDV-FRM-001") ||
            form.id().equals("SBDV-FRM-110") ||
            form.id().matches("SBDV-FRM-[1-5][0-9][0-9]");
    }

    private static boolean isDatatypeForm(@NotNull ScratchBirdFormDefinition form) {
        return "SBDV-FRM-611".equals(form.id()) ||
            "SBDV-FRM-612".equals(form.id()) ||
            "SBDV-FRM-613".equals(form.id()) ||
            form.childForms().contains("SBDV-FRM-613");
    }

    private static boolean isReportForm(@NotNull ScratchBirdFormDefinition form) {
        return form.id().matches("SBDV-FRM-90[0-4]");
    }

    @NotNull
    private static String lifecycleFamily(@NotNull ScratchBirdFormDefinition form) {
        return switch (form.id()) {
            case "SBDV-FRM-601" -> "Relational table";
            case "SBDV-FRM-602" -> "Non-relational table";
            case "SBDV-FRM-603" -> "Relational view";
            case "SBDV-FRM-604" -> "Non-relational view";
            case "SBDV-FRM-605" -> "Column or field";
            case "SBDV-FRM-606" -> "Constraint";
            case "SBDV-FRM-607" -> "Index";
            case "SBDV-FRM-608" -> "Sequence";
            case "SBDV-FRM-609" -> "Routine";
            case "SBDV-FRM-610" -> "Trigger";
            case "SBDV-FRM-611" -> "Domain or datatype definition";
            case "SBDV-FRM-612" -> "Payload model";
            default -> form.name();
        };
    }

    @NotNull
    private static EditorPage page(
        @NotNull String id,
        @NotNull String tabLabel,
        @NotNull String title,
        @NotNull String purpose,
        @NotNull List<String> controls,
        @NotNull List<String> validationWidgets,
        @NotNull List<String> evidenceAnchors
    ) {
        return new EditorPage(
            id,
            tabLabel,
            title,
            purpose,
            List.copyOf(controls),
            List.copyOf(validationWidgets),
            List.copyOf(evidenceAnchors));
    }
}
