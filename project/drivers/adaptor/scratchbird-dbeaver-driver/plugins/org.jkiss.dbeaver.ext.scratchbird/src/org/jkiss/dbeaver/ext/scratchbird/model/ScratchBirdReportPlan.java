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

public record ScratchBirdReportPlan(
    @NotNull ScratchBirdReportDefinition report,
    @NotNull ScratchBirdRefusalModel sourceStatus,
    @NotNull List<String> sourceQueries,
    @NotNull String alertExpressionStarter,
    @NotNull List<String> drilldownFields
) {

    @NotNull
    public static ScratchBirdReportPlan forReport(@NotNull ScratchBirdReportDefinition report) {
        return new ScratchBirdReportPlan(
            report,
            ScratchBirdMetricSourceResolver.sourceStatus(report),
            report.sourceSurfaces().stream().map(ScratchBirdReportPlan::sourceQueryFor).distinct().toList(),
            alertExpressionFor(report),
            drilldownFieldsFor(report));
    }

    @NotNull
    public String scriptPreview() {
        StringBuilder builder = new StringBuilder();
        builder.append("-- ScratchBird report ").append(report.id()).append(": ").append(report.title()).append('\n');
        builder.append("-- Output: ").append(report.bestOutput()).append('\n');
        builder.append("-- Aggregation: ").append(report.aggregationGrain()).append('\n');
        builder.append("-- Retention: ").append(report.defaultRetention()).append('\n');
        builder.append("-- Source status: ").append(sourceStatus.kind()).append(" - ").append(sourceStatus.message()).append('\n');
        if (report.futureGated()) {
            builder.append("-- Future gated: backing server surface is not available yet.\n");
        }
        for (String query : sourceQueries) {
            builder.append(query).append('\n');
        }
        return builder.toString().stripTrailing();
    }

    @NotNull
    public List<String> summaryLines() {
        List<String> lines = new ArrayList<>();
        lines.add("Report: " + report.id() + " - " + report.title());
        lines.add("Branch: " + report.branch());
        lines.add("Parent form: " + report.parentForm());
        lines.add("Output: " + report.bestOutput());
        lines.add("Aggregation grain: " + report.aggregationGrain());
        lines.add("Default retention: " + report.defaultRetention());
        lines.add("Alert starter: " + report.alertStarter());
        lines.add("Access notes: " + report.accessNotes());
        lines.add("Source status: " + sourceStatus.kind() + " - " + sourceStatus.message());
        lines.add("Future gated: " + report.futureGated());
        return List.copyOf(lines);
    }

    @NotNull
    private static List<String> drilldownFieldsFor(@NotNull ScratchBirdReportDefinition report) {
        List<String> fields = new ArrayList<>();
        fields.add("time range or live mode");
        fields.add("effective principal");
        fields.add("source surface status");
        fields.add("export action");
        for (String part : report.aggregationGrain().split(",")) {
            String field = part.trim();
            if (!field.isEmpty() && !"current plus trend".equalsIgnoreCase(field)) {
                fields.add(field);
            }
        }
        if (report.sourceSurfaces().stream().anyMatch(source -> source.endsWith("_*"))) {
            fields.add("histogram bucket");
            fields.add("percentile selector");
        }
        return List.copyOf(fields);
    }

    @NotNull
    private static String alertExpressionFor(@NotNull ScratchBirdReportDefinition report) {
        if (report.futureGated()) {
            return "-- Future gated alert for " + report.id() + ": enable when " +
                String.join(", ", report.sourceSurfaces()) + " is published.";
        }
        return "-- Alert starter for " + report.id() + ": " + report.alertStarter();
    }

    @NotNull
    private static String sourceQueryFor(@NotNull String source) {
        if (source.startsWith("SHOW ")) {
            return source;
        }
        if (source.startsWith("MON_")) {
            return "SELECT * FROM " + source;
        }
        if (source.startsWith("sys.")) {
            return sysQueryFor(source);
        }
        if (source.endsWith("_*")) {
            return "SHOW METRICS";
        }
        if (source.startsWith("scratchbird_") || source.startsWith("sb_")) {
            return "SHOW METRICS";
        }
        if (source.startsWith("alert rule")) {
            return "-- Configure alert rule from report matrix primary inputs";
        }
        return "SELECT * FROM sys." + source;
    }

    @NotNull
    private static String sysQueryFor(@NotNull String source) {
        String[] segments = source.split("\\.");
        if (segments.length <= 2) {
            return "SELECT * FROM " + source;
        }
        String column = segments[segments.length - 1];
        String relation = String.join(".", List.of(segments).subList(0, segments.length - 1));
        return "SELECT " + quoteIdentifier(column) + " FROM " + relation;
    }

    @NotNull
    private static String quoteIdentifier(@NotNull String identifier) {
        if (identifier.matches("[A-Za-z_][A-Za-z0-9_]*")) {
            return identifier;
        }
        return '"' + identifier.replace("\"", "\"\"") + '"';
    }
}
