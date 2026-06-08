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
import org.jkiss.dbeaver.model.struct.DBSTypedObject;

import java.util.ArrayList;
import java.util.Collection;
import java.util.List;

public final class ScratchBirdFormPanelCatalog {

    public record Entry(@NotNull String label, @NotNull String value) {
    }

    public record Panel(
        @NotNull String id,
        @NotNull String tabLabel,
        @NotNull String title,
        @NotNull List<Entry> entries
    ) {
    }

    private ScratchBirdFormPanelCatalog() {
    }

    @NotNull
    public static List<Panel> panelsFor(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdFormMode mode,
        @NotNull String targetPath,
        @NotNull ScratchBirdRefusalModel permission,
        @NotNull ScratchBirdAdminExecutor.ExecutionPlan plan,
        @NotNull ScratchBirdLiveProbe.ProbePlan authzProbePlan,
        @NotNull ScratchBirdLiveProbe.ProbePlan liveProbePlan,
        @NotNull List<ScratchBirdTaskDefinition> taskDefinitions,
        @Nullable ScratchBirdDestructivePlan destructivePlan,
        @Nullable DBSTypedObject typedObject
    ) {
        List<Panel> panels = new ArrayList<>();
        if (isAdministrativeForm(form)) {
            panels.add(adminSurfacePanel(form, targetPath, permission, authzProbePlan, liveProbePlan));
            panels.add(adminToolingPanel(form, targetPath, plan, taskDefinitions, destructivePlan));
        }
        if (isLifecycleForm(form)) {
            panels.add(lifecyclePanel(form, mode, targetPath, plan));
        }
        if (typedObject != null || supportsDatatypeAuthoring(form)) {
            panels.add(datatypePanel(form, typedObject));
        }
        return List.copyOf(panels);
    }

    @NotNull
    private static Panel adminSurfacePanel(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull String targetPath,
        @NotNull ScratchBirdRefusalModel permission,
        @NotNull ScratchBirdLiveProbe.ProbePlan authzProbePlan,
        @NotNull ScratchBirdLiveProbe.ProbePlan liveProbePlan
    ) {
        ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(targetPath);
        return panel(
            "admin-surface",
            "Admin",
            "ScratchBird administrative surface",
            entry("Branch profile", profile.id() + " - " + profile.label()),
            entry("Form route", form.summary()),
            entry("Mutation posture", profile.mutationSummary()),
            entry("Action set", joinActions(profile.actions())),
            entry("Expected children", String.join(", ", profile.expectedChildren())),
            entry("Focus fields", String.join(", ", profile.focusFields())),
            entry("Static posture", permission.kind() + ": " + permission.message()),
            entry("Server authz probe", probeState(authzProbePlan)),
            entry("Live probe", probeState(liveProbePlan)));
    }

    @NotNull
    private static Panel adminToolingPanel(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull String targetPath,
        @NotNull ScratchBirdAdminExecutor.ExecutionPlan plan,
        @NotNull List<ScratchBirdTaskDefinition> taskDefinitions,
        @Nullable ScratchBirdDestructivePlan destructivePlan
    ) {
        List<Entry> entries = new ArrayList<>();
        entries.add(entry("Target scope", targetPath));
        entries.add(entry("Preview authority", plan.authority()));
        entries.add(entry("Default preview", plan.commandText()));
        entries.add(entry("Task catalog size", Integer.toString(taskDefinitions.size())));
        if (!taskDefinitions.isEmpty()) {
            ScratchBirdTaskDefinition primaryTask = taskDefinitions.get(0);
            entries.add(entry("Primary task", primaryTask.id() + " - " + primaryTask.title()));
            entries.add(entry("Primary task modes", String.join(", ", primaryTask.executionModes())));
            entries.add(entry("Primary task result surfaces", String.join(", ", primaryTask.resultSurfaces())));
        }
        if (destructivePlan != null) {
            entries.add(entry("Delete confirmation", destructivePlan.confirmationPhrase()));
            entries.add(entry("Dry-run / validate-only", String.join(" | ", destructivePlan.dryRunCommands())));
        }
        if ("SBDV-FRM-110".equals(form.id())) {
            entries.add(entry("Grant editor role", "Reusable child form for grants, ownership, and effective permission review."));
        }
        return new Panel("admin-tooling", "Tooling", "Administrative tooling and review", List.copyOf(entries));
    }

    @NotNull
    private static Panel lifecyclePanel(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdFormMode mode,
        @NotNull String targetPath,
        @NotNull ScratchBirdAdminExecutor.ExecutionPlan plan
    ) {
        ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(targetPath);
        List<Entry> entries = new ArrayList<>();
        entries.add(entry("Object family", lifecycleFamily(form)));
        entries.add(entry("Lifecycle modes", joinModes(form.modes())));
        entries.add(entry("Active mode", mode.name()));
        entries.add(entry("Namespace profile", profile.id() + " - " + profile.label()));
        entries.add(entry("Preview authority", plan.authority()));
        entries.add(entry("Executable preview", Boolean.toString(plan.executable())));
        entries.add(entry("Destructive preview", Boolean.toString(plan.destructive())));
        entries.add(entry("Default preview", plan.commandText()));
        entries.add(entry("Child forms", childFormSummary(form)));
        if (hasChildForm(form, "SBDV-FRM-015")) {
            entries.add(entry("Validation attachment", "SBDV-FRM-015 - Validation, Lint, And Parser Diagnostics Panel"));
        }
        if (hasChildForm(form, "SBDV-FRM-110")) {
            entries.add(entry("Grant attachment", "SBDV-FRM-110 - Grant, Role, And Ownership Editor"));
        }
        if (hasChildForm(form, "SBDV-FRM-901")) {
            entries.add(entry("Metrics attachment", "SBDV-FRM-901 - Object Metrics Panel"));
        }
        if (hasChildForm(form, "SBDV-FRM-014")) {
            entries.add(entry("Delete attachment", "SBDV-FRM-014 - Permission-Aware Destructive Action Wizard"));
        }
        return new Panel("lifecycle", "Lifecycle", "ScratchBird lifecycle route", List.copyOf(entries));
    }

    @NotNull
    private static Panel datatypePanel(
        @NotNull ScratchBirdFormDefinition form,
        @Nullable DBSTypedObject typedObject
    ) {
        ScratchBirdValueProfile valueProfile = typedObject == null ?
            genericValueProfile(form) :
            ScratchBirdValueProfile.fromTypedObject(typedObject);
        return panel(
            "datatype",
            "Datatype",
            "ScratchBird datatype and value authoring",
            entry("Datatype surface", datatypeSurface(form, typedObject, valueProfile)),
            entry("Widget strategy", widgetStrategy(valueProfile)),
            entry("Must controls", String.join(", ", mustControls(valueProfile))),
            entry("Should controls", String.join(", ", shouldControls(valueProfile))),
            entry("Optional controls", String.join(", ", optionalControls(valueProfile))),
            entry("Canonical text contract", valueProfile.canonicalTextForm()),
            entry("Content type", valueProfile.contentTypeOrDefault()),
            entry("Round-trip posture", valueProfile.explicitTextRoundTrip() ?
                "Explicit CAST/text roundtrip is expected." :
                "Direct scalar/text handling is expected."),
            entry("Example literal", ScratchBirdValueBinding.exampleLiteralForType(valueProfile.declaredTypeName())));
    }

    private static boolean isAdministrativeForm(@NotNull ScratchBirdFormDefinition form) {
        return form.id().equals("SBDV-FRM-001") ||
            form.id().equals("SBDV-FRM-110") ||
            form.id().matches("SBDV-FRM-[1-5][0-9][0-9]");
    }

    private static boolean isLifecycleForm(@NotNull ScratchBirdFormDefinition form) {
        return form.id().matches("SBDV-FRM-60[1-9]") ||
            "SBDV-FRM-610".equals(form.id()) ||
            "SBDV-FRM-611".equals(form.id()) ||
            "SBDV-FRM-612".equals(form.id());
    }

    private static boolean supportsDatatypeAuthoring(@NotNull ScratchBirdFormDefinition form) {
        return "SBDV-FRM-613".equals(form.id()) ||
            hasChildForm(form, "SBDV-FRM-613") ||
            "SBDV-FRM-611".equals(form.id()) ||
            "SBDV-FRM-612".equals(form.id());
    }

    @NotNull
    private static String joinActions(@NotNull List<ScratchBirdNavigatorActionRegistry.Action> actions) {
        return actions.stream()
            .map(Enum::name)
            .reduce((left, right) -> left + ", " + right)
            .orElse("-");
    }

    @NotNull
    private static String joinModes(@NotNull Collection<ScratchBirdFormMode> modes) {
        return modes.stream()
            .map(Enum::name)
            .reduce((left, right) -> left + ", " + right)
            .orElse("-");
    }

    @NotNull
    private static String probeState(@NotNull ScratchBirdLiveProbe.ProbePlan plan) {
        return plan.executable() ? plan.label() + " (ready)" : plan.label() + " (unavailable)";
    }

    private static boolean hasChildForm(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull String childFormId
    ) {
        return form.childForms().stream().anyMatch(childFormId::equals);
    }

    @NotNull
    private static String childFormSummary(@NotNull ScratchBirdFormDefinition form) {
        if (form.childForms().isEmpty()) {
            return "-";
        }
        return form.childForms().stream()
            .map(childFormId -> {
                ScratchBirdFormDefinition child = ScratchBirdFormRegistry.find(childFormId);
                return child == null ? childFormId : child.summary();
            })
            .reduce((left, right) -> left + " | " + right)
            .orElse("-");
    }

    @NotNull
    private static String lifecycleFamily(@NotNull ScratchBirdFormDefinition form) {
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
    private static String datatypeSurface(
        @NotNull ScratchBirdFormDefinition form,
        @Nullable DBSTypedObject typedObject,
        @NotNull ScratchBirdValueProfile valueProfile
    ) {
        if (typedObject != null) {
            return valueProfile.declaredTypeName() + " via " + valueProfile.familyLabel();
        }
        return switch (form.id()) {
            case "SBDV-FRM-611" -> "Domain definitions with ScratchBird datatype profile routing";
            case "SBDV-FRM-612" -> "Payload model bindings with ScratchBird value-profile routing";
            case "SBDV-FRM-613" -> "Generic ScratchBird value-manager attachment";
            default -> form.summary();
        };
    }

    @NotNull
    private static ScratchBirdValueProfile genericValueProfile(@NotNull ScratchBirdFormDefinition form) {
        return switch (form.id()) {
            case "SBDV-FRM-611", "SBDV-FRM-612" -> ScratchBirdValueProfile.fromTypeName("JSONB");
            case "SBDV-FRM-613" -> ScratchBirdValueProfile.fromTypeName("TEXT");
            default -> ScratchBirdValueProfile.fromTypeName("TEXT");
        };
    }

    @NotNull
    private static String widgetStrategy(@NotNull ScratchBirdValueProfile valueProfile) {
        return switch (valueProfile.family()) {
            case JSON, XML -> "Structured payload editor with raw-text mirror and literal preview";
            case VECTOR -> "Ordered numeric vector editor with deterministic bracketed serialization";
            case GEOMETRY -> "Canonical geometry-text editor with optional coordinate preview";
            case RANGE, NETWORK, FULLTEXT, INTERVAL, MONEY, COMPOSITE ->
                "Structured text editor with contract-aware validation";
            case BINARY -> "Hex and file-backed binary payload editor";
            case UUID -> "Strict canonical UUID text editor";
            case BOOLEAN -> "Boolean toggle editor with SQL literal preview";
            case NUMERIC -> "Scalar numeric editor with precision-aware literal preview";
            case TEMPORAL -> "Temporal editor with canonical text and timezone review";
            case ENUM_SET -> "Enumerated selector with raw text fallback";
            case TEXT, OTHER -> "Scalar text editor with literal preview";
        };
    }

    @NotNull
    private static List<String> mustControls(@NotNull ScratchBirdValueProfile valueProfile) {
        return switch (valueProfile.family()) {
            case JSON, XML -> List.of("structured payload field", "raw text preview", "SQL literal preview", "validation result");
            case VECTOR -> List.of("ordered numeric entry grid", "dimension summary", "SQL literal preview", "validation result");
            case GEOMETRY -> List.of("canonical geometry text", "SRID/shape review", "SQL literal preview", "validation result");
            case RANGE, NETWORK, FULLTEXT, INTERVAL, MONEY, COMPOSITE ->
                List.of("contract-aware text editor", "SQL literal preview", "validation result", "null-state toggle");
            case BINARY -> List.of("hex/file payload view", "size summary", "SQL literal preview", "validation result");
            case UUID -> List.of("canonical UUID field", "SQL literal preview", "validation result", "null-state toggle");
            default -> List.of("typed value field", "SQL literal preview", "validation result", "null-state toggle");
        };
    }

    @NotNull
    private static List<String> shouldControls(@NotNull ScratchBirdValueProfile valueProfile) {
        return switch (valueProfile.family()) {
            case JSON, XML -> List.of("pretty formatter", "content-type badge", "roundtrip status", "domain binding");
            case VECTOR -> List.of("dimension guard", "copy-as-literal", "roundtrip status", "domain binding");
            case GEOMETRY -> List.of("coordinate preview", "copy-as-literal", "roundtrip status", "domain binding");
            case RANGE, NETWORK, FULLTEXT, INTERVAL, MONEY, COMPOSITE ->
                List.of("copy-as-literal", "roundtrip status", "domain binding", "content-type badge");
            case BINARY -> List.of("download/export action", "roundtrip status", "domain binding", "content-type badge");
            case UUID -> List.of("copy-as-literal", "roundtrip status", "domain binding", "format status");
            default -> List.of("copy-as-literal", "roundtrip status", "domain binding", "format status");
        };
    }

    @NotNull
    private static List<String> optionalControls(@NotNull ScratchBirdValueProfile valueProfile) {
        return switch (valueProfile.family()) {
            case JSON, XML -> List.of("schema-aware hinting", "sample payload template", "diff view");
            case VECTOR -> List.of("dimension preset", "CSV paste/import", "sample vector template");
            case GEOMETRY -> List.of("map preview", "WKT/WKB toggle", "sample geometry template");
            case RANGE, NETWORK, FULLTEXT -> List.of("sample contract template", "history snippets", "raw metadata view");
            case BINARY -> List.of("image/binary preview", "checksum panel", "raw metadata view");
            case UUID -> List.of("generator shortcut", "history snippets", "raw metadata view");
            default -> List.of("history snippets", "raw metadata view", "operator notes");
        };
    }

    @NotNull
    private static Panel panel(
        @NotNull String id,
        @NotNull String tabLabel,
        @NotNull String title,
        @NotNull Entry... entries
    ) {
        return new Panel(id, tabLabel, title, List.of(entries));
    }

    @NotNull
    private static Entry entry(@NotNull String label, @NotNull String value) {
        return new Entry(label, value);
    }
}
