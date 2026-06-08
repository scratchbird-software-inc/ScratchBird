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
import java.util.Locale;

public final class ScratchBirdTaskCatalog {

    private ScratchBirdTaskCatalog() {
    }

    @NotNull
    public static List<ScratchBirdTaskDefinition> tasksFor(@NotNull String targetPath) {
        String normalized = normalize(targetPath);
        if (normalized.isEmpty() || ScratchBirdNamespaceSemantics.isMetricsPath(normalized)) {
            return List.of();
        }

        List<ScratchBirdTaskDefinition> tasks = new ArrayList<>();
        if (normalized.equals("sys.jobs") || normalized.startsWith("sys.jobs.")) {
            tasks.add(task(
                "SBDV-TSK-201",
                "Scheduler Health Snapshot",
                "Review scheduler inventory, recent runs, and queue posture before mutating jobs.",
                capability(targetPath),
                "SELECT * FROM sys.jobs",
                "SELECT * FROM sys.job_runs ORDER BY started_at DESC LIMIT 50",
                "Submit a scheduler-health audit job after the capability probe admits delegated execution.",
                List.of(
                    "{\"scope\":\"" + targetPath + "\",\"include_runs\":true,\"limit\":50}",
                    "{\"job_name\":\"<job_name>\",\"mode\":\"validate\"}"),
                List.of("sys.jobs", "sys.job_runs"),
                "Scheduler tasks require visibility to sys.jobs and any server-side management surfaces."));
            tasks.add(task(
                "SBDV-TSK-202",
                "Dependency And Failure Audit",
                "Inspect dependency edges and failed or running job state before replay or disable actions.",
                capability(targetPath),
                "SELECT * FROM sys.job_dependencies",
                "SELECT * FROM sys.job_runs WHERE state IN ('FAILED', 'RUNNING')",
                "Capture job-failure evidence and then schedule replay or disable through the scheduler lane.",
                List.of(
                    "{\"scope\":\"" + targetPath + "\",\"state_filter\":[\"FAILED\",\"RUNNING\"]}",
                    "{\"include_dependencies\":true}"),
                List.of("sys.job_dependencies", "sys.job_runs"),
                "Operational review is read-only until a later server-admitted task or destructive flow is selected."));
            return List.copyOf(tasks);
        }
        if (normalized.equals("sys.monitoring") || normalized.startsWith("sys.monitoring.")) {
            tasks.add(task(
                "SBDV-TSK-301",
                "Listener And Front-Door Snapshot",
                "Inspect listener posture, open connections, and queue pressure from the monitoring branch.",
                capability(targetPath),
                "SHOW MANAGEMENT LISTENERS",
                "SHOW MANAGEMENT PARSER POOL FOR LISTENER 'scratchbird'",
                "Schedule a listener-health capture after validating the connected principal may read process-scoped surfaces.",
                List.of(
                    "{\"listener\":\"scratchbird\",\"include_parser_pool\":true}",
                    "{\"scope\":\"" + targetPath + "\"}"),
                List.of("SHOW MANAGEMENT LISTENERS", "SHOW MANAGEMENT PARSER POOL FOR LISTENER <protocol>"),
                "Listener metrics are process-scoped; parser-pool health is typically superuser-visible only."));
            tasks.add(task(
                "SBDV-TSK-302",
                "Support Bundle Readiness Snapshot",
                "Collect support-bundle safety and readiness status before deeper operator workflows.",
                capability(targetPath),
                "SHOW CLUSTER STATE READINESS_HEALTH",
                "SHOW CLUSTER STATE SUPPORT_BUNDLE_SAFETY",
                "Schedule a support-bundle readiness capture if the support workflow must be repeated.",
                List.of(
                    "{\"scope\":\"cluster\",\"report\":\"readiness\"}",
                    "{\"include_support_bundle\":true}"),
                List.of("SHOW CLUSTER STATE READINESS_HEALTH", "SHOW CLUSTER STATE SUPPORT_BUNDLE_SAFETY"),
                "These cluster views are server-authoritative and may be limited to elevated principals."));
            return List.copyOf(tasks);
        }
        if (normalized.equals("sys.cluster") || normalized.startsWith("sys.cluster.") ||
            normalized.startsWith("cluster.")) {
            tasks.add(task(
                "SBDV-TSK-401",
                "Cluster Routing And Admission Review",
                "Inspect routing plan and admission posture before topology or policy changes.",
                capability(targetPath),
                "SHOW CLUSTER ADMISSION STATUS",
                "SHOW CLUSTER ROUTING PLAN",
                "Schedule a recurring routing-plan audit after admission status is validated.",
                List.of(
                    "{\"scope\":\"" + targetPath + "\",\"include_admission\":true}",
                    "{\"routing_epoch\":\"current\"}"),
                List.of("SHOW CLUSTER ADMISSION STATUS", "SHOW CLUSTER ROUTING PLAN"),
                "Cluster routing and admission surfaces are typically restricted to operator-capable principals."));
            tasks.add(task(
                "SBDV-TSK-402",
                "SLO And Error-Budget Snapshot",
                "Capture readiness, SLO, and burn-rate posture for cluster operations.",
                capability(targetPath),
                "SHOW CLUSTER STATE READINESS_HEALTH",
                "SHOW CLUSTER STATE SLO_STATUS",
                "Schedule an SLO snapshot once the cluster operator confirms recurring collection policy.",
                List.of(
                    "{\"scope\":\"cluster\",\"report\":\"slo\"}",
                    "{\"include_error_budget\":true}"),
                List.of("SHOW CLUSTER STATE READINESS_HEALTH", "SHOW CLUSTER STATE SLO_STATUS"),
                "SLO and error-budget state remains server-authoritative and can be hidden by permissions."));
            return List.copyOf(tasks);
        }
        if (normalized.equals("sys.catalog") || normalized.startsWith("sys.catalog.")) {
            tasks.add(task(
                "SBDV-TSK-501",
                "Catalog Identity Audit",
                "Inspect object identity, parentage, and resolver rows for the selected scope.",
                capability(targetPath),
                "SELECT object_id, object_type, schema_path, full_path, object_name FROM sys.catalog.object_resolver WHERE full_path = " + literal(targetPath),
                "SELECT object_id, object_type, schema_path, full_path, object_name FROM sys.catalog.object_resolver WHERE schema_path = " + literal(targetPath),
                "Schedule a catalog-audit capture before migration or destructive operations.",
                List.of(
                    "{\"scope\":\"" + targetPath + "\",\"include_children\":true}",
                    "{\"resolver_mode\":\"full-path\"}"),
                List.of("sys.catalog.object_resolver"),
                "Catalog truth is read-only in the client; mutating actions still depend on dedicated object editors."));
            return List.copyOf(tasks);
        }
        if (normalized.equals("sys.structure") || normalized.startsWith("sys.structure.") ||
            normalized.equals("sys.config") || normalized.startsWith("sys.config.") ||
            normalized.equals("sys.security") || normalized.startsWith("sys.security.")) {
            tasks.add(task(
                "SBDV-TSK-601",
                "Capability And Drift Review",
                "Run a control-plane capability check and drift summary before editing protected configuration surfaces.",
                capability(targetPath),
                "SHOW MANAGEMENT MANAGER",
                "SHOW MANAGEMENT DRIFT",
                "Schedule a drift scan only after the manager plane admits recurring control-plane jobs.",
                List.of(
                    "{\"scope\":\"" + targetPath + "\",\"include_drift\":true}",
                    "{\"effective_principal\":\"current\"}"),
                List.of("SHOW MANAGEMENT MANAGER", "SHOW MANAGEMENT DRIFT"),
                "Protected system branches remain server-authoritative; this wizard only prepares admitted tasks."));
            return List.copyOf(tasks);
        }
        if (normalized.equals("sys.internals") || normalized.startsWith("sys.internals.")) {
            tasks.add(task(
                "SBDV-TSK-611",
                "Internal Runtime Snapshot",
                "Prepare an internal runtime snapshot request for a server-admitted operator without exposing direct mutation.",
                capability(targetPath),
                "SELECT object_id, object_type, schema_path, full_path, object_name FROM sys.catalog.object_resolver WHERE full_path = " + literal(targetPath),
                "SELECT object_id, object_type, schema_path, full_path, object_name FROM sys.catalog.object_resolver WHERE schema_path = " + literal(targetPath),
                "No schedule preview is exposed until the server publishes an explicit internal-runtime operator lane.",
                List.of(
                    "{\"scope\":\"" + targetPath + "\",\"operator_only\":true}",
                    "{\"include_runtime_state\":true}"),
                List.of("sys.catalog.object_resolver", "sys.server_capabilities"),
                "sys.internals remains client-denied until the server exposes an explicit admitted surface."));
            return List.copyOf(tasks);
        }
        if (normalized.startsWith("remote.") || normalized.startsWith("emulated.") ||
            normalized.equals("sys.emulation") || normalized.startsWith("sys.emulation.")) {
            tasks.add(task(
                "SBDV-TSK-701",
                "Connector And Listener Inventory",
                "Inspect management-server inventory and connector instructions for remote or emulation endpoints.",
                capability(targetPath),
                "SHOW MANAGEMENT SERVERS",
                "SHOW MANAGEMENT INSTRUCTIONS",
                "Schedule a connector-readiness bundle after the management plane admits recurring audits.",
                List.of(
                    "{\"scope\":\"" + targetPath + "\",\"include_servers\":true}",
                    "{\"include_instructions\":true}"),
                List.of("SHOW MANAGEMENT SERVERS", "SHOW MANAGEMENT INSTRUCTIONS"),
                "Remote and emulation management surfaces are usually operator-only and may span multiple processes."));
            return List.copyOf(tasks);
        }
        tasks.add(task(
            "SBDV-TSK-801",
            "Object Dependency Preview",
            "Inspect catalog identity and dependency posture before mutating the selected object.",
            capability(targetPath),
            "SELECT object_id, object_type, schema_path, full_path, object_name FROM sys.catalog.object_resolver WHERE full_path = " + literal(targetPath),
            "SELECT * FROM sys.table_stats",
            "Schedule a statistics or dependency audit through the job lane after the server admits the task.",
            List.of(
                "{\"scope\":\"" + targetPath + "\",\"include_dependents\":true}",
                "{\"statistics_scope\":\"object\"}"),
            List.of("sys.catalog.object_resolver", "sys.table_stats"),
            "Concrete object tasks remain read-mostly until a server-admitted administrative command is selected."));
        tasks.add(task(
            "SBDV-TSK-802",
            "Index Health Snapshot",
            "Review index and access-path posture for the selected object family.",
            capability(targetPath),
            "SELECT * FROM sys.table_stats",
            "SHOW INDEX " + quotePath(targetPath) + " HEALTH",
            "Schedule an index-health run after validating the object type and permissions server-side.",
            List.of(
                "{\"scope\":\"" + targetPath + "\",\"report\":\"index-health\"}",
                "{\"include_storage\":true}"),
            List.of("sys.table_stats", "SHOW INDEX ... HEALTH"),
            "This preview assumes the selected object participates in access-path analysis; server refusal remains authoritative."));
        return List.copyOf(tasks);
    }

    @NotNull
    private static ScratchBirdTaskDefinition task(
        @NotNull String id,
        @NotNull String title,
        @NotNull String summary,
        @NotNull String precheckCommand,
        @NotNull String validateCommand,
        @NotNull String executeCommand,
        @NotNull String schedulePreview,
        @NotNull List<String> parameterTemplate,
        @NotNull List<String> resultSurfaces,
        @NotNull String admissionNote
    ) {
        return new ScratchBirdTaskDefinition(
            id,
            title,
            summary,
            precheckCommand,
            validateCommand,
            executeCommand,
            schedulePreview,
            parameterTemplate,
            resultSurfaces,
            admissionNote,
            false);
    }

    @NotNull
    private static String capability(@NotNull String targetPath) {
        return ScratchBirdPermissionProbe.serverAuthorizationScript(targetPath);
    }

    @NotNull
    private static String quotePath(@NotNull String path) {
        StringBuilder builder = new StringBuilder();
        for (String segment : path.split("\\.")) {
            if (segment.isEmpty()) {
                continue;
            }
            if (builder.length() > 0) {
                builder.append('.');
            }
            builder.append(quoteIdentifier(segment));
        }
        return builder.toString();
    }

    @NotNull
    private static String quoteIdentifier(@NotNull String identifier) {
        if (identifier.matches("[A-Za-z_][A-Za-z0-9_]*")) {
            return identifier;
        }
        return '"' + identifier.replace("\"", "\"\"") + '"';
    }

    @NotNull
    private static String literal(@NotNull String value) {
        return "'" + value.replace("'", "''") + "'";
    }

    @NotNull
    private static String normalize(@NotNull String value) {
        return value.toLowerCase(Locale.ENGLISH);
    }
}
