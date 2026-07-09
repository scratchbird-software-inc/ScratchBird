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

import java.util.Collection;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

public final class ScratchBirdManagementSurfaceCatalog {

    public static final String ROOT_PATH = "management";
    public static final String REPORT_BASE_PATH = ROOT_PATH + ".diagnostics";

    private static final List<ScratchBirdNavigatorActionRegistry.Action> READ_REPORT_ACTIONS = List.of(
        ScratchBirdNavigatorActionRegistry.Action.OPEN,
        ScratchBirdNavigatorActionRegistry.Action.PROPERTIES,
        ScratchBirdNavigatorActionRegistry.Action.REPORTS,
        ScratchBirdNavigatorActionRegistry.Action.REFRESH,
        ScratchBirdNavigatorActionRegistry.Action.SOURCE_STATUS);

    private static final List<ScratchBirdNavigatorActionRegistry.Action> TASK_REPORT_ACTIONS = List.of(
        ScratchBirdNavigatorActionRegistry.Action.OPEN,
        ScratchBirdNavigatorActionRegistry.Action.PROPERTIES,
        ScratchBirdNavigatorActionRegistry.Action.TASKS,
        ScratchBirdNavigatorActionRegistry.Action.REPORTS,
        ScratchBirdNavigatorActionRegistry.Action.REFRESH,
        ScratchBirdNavigatorActionRegistry.Action.SOURCE_STATUS);

    private static final Map<String, ScratchBirdManagementSurfaceDefinition> SURFACES_BY_PATH =
        new LinkedHashMap<>();

    static {
        surface(ROOT_PATH, "management", "dashboard",
            List.of("sys.catalog_readable.navigator_tree", "SHOW MANAGEMENT"),
            "right.management_runtime_read", "manual_or_30s", READ_REPORT_ACTIONS,
            "Unauthorized nodes are hidden; denied or unavailable sources are shown as source-status refusals.",
            "SBDV-FRM-900");
        surface(ROOT_PATH + ".overview", "overview", "dashboard",
            List.of("sys.server_capabilities", "sys.catalog_readable.navigator_tree", "SHOW METRICS"),
            "right.management_runtime_read", "manual_or_30s", READ_REPORT_ACTIONS,
            "Overview panels show source-status refusal when any source is denied.",
            "SBDV-FRM-900");
        surface(ROOT_PATH + ".sessions", "sessions", "grid",
            List.of("SHOW SESSIONS", "SHOW TRANSACTIONS"),
            "right.management_runtime_read", "manual_or_5s", READ_REPORT_ACTIONS,
            "Session rows are filtered to authorized sessions or refused by the server.",
            "SBDV-FRM-903");
        surface(ROOT_PATH + ".workload", "workload", "report",
            List.of("SHOW STATEMENTS", "SHOW METRICS"),
            "right.management_runtime_read", "manual_or_5s", READ_REPORT_ACTIONS,
            "Statement details remain hidden or refused when the session cannot inspect them.",
            "SBDV-FRM-903");
        surface(ROOT_PATH + ".storage", "storage", "dashboard",
            List.of("sys.catalog_readable.filespaces", "sys.catalog_readable.page_families", "SHOW METRICS"),
            "right.management_runtime_read", "manual_or_30s", READ_REPORT_ACTIONS,
            "Device paths are redacted by policy and unauthorized filespaces are hidden.",
            "SBDV-FRM-903");
        surface(ROOT_PATH + ".memory", "memory", "dashboard",
            List.of("SHOW METRICS FAMILY memory", "sys.catalog_readable.settings"),
            "right.management_runtime_read", "manual_or_5s", READ_REPORT_ACTIONS,
            "Memory panels show policy/status refusal when memory metrics are not visible.",
            "SBDV-FRM-903");
        surface(ROOT_PATH + ".security", "security", "detail_form",
            List.of("sys.catalog_readable.security_subjects", "sys.catalog_readable.privileges",
                "sys.security.permission_probe"),
            "right.security_admin_read", "manual", READ_REPORT_ACTIONS,
            "Principals are hidden unless authorized; mutation previews require server admission.",
            "SBDV-FRM-103");
        surface(ROOT_PATH + ".programmability", "programmability", "detail_form",
            List.of("sys.catalog_readable.operations", "sys.catalog_readable.procedures",
                "sys.catalog_readable.packages"),
            "right.discover", "manual", READ_REPORT_ACTIONS,
            "Invisible routines/packages are hidden; lifecycle actions require server admission.",
            "SBDV-FRM-107");
        surface(ROOT_PATH + ".domains", "domains", "grid",
            List.of("sys.catalog_readable.domains", "sys.catalog_readable.datatypes",
                "sys.catalog_readable.casts"),
            "right.discover", "manual", READ_REPORT_ACTIONS,
            "Invisible domains are hidden; datatype-management actions require server admission.",
            "SBDV-FRM-611");
        surface(ROOT_PATH + ".jobs", "jobs", "grid",
            List.of("SHOW JOBS", "SHOW JOB RUNS", "SHOW JOB DEPENDENCIES", "sys.catalog_readable.jobs"),
            "right.management_runtime_read", "manual_or_30s", TASK_REPORT_ACTIONS,
            "Scheduler source denial is reported as source-status refusal.",
            "SBDV-FRM-102");
        surface(ROOT_PATH + ".agents", "agents", "grid",
            List.of("sys.frontend.agents", "sys.frontend.agent_policies", "sys.frontend.agent_actions",
                "sys.frontend.agent_metric_dependencies", "sys.frontend.agent_overrides",
                "sys.frontend.agent_evidence", "sys.frontend.agent_audit"),
            "right.agent_read", "manual_or_30s", TASK_REPORT_ACTIONS,
            "Agent detail denial is reported as source-status refusal.",
            "SBDV-FRM-106");
        surface(ROOT_PATH + ".configuration", "configuration", "detail_form",
            List.of("sys.configuration.settings", "sys.configuration.profiles",
                "sys.configuration.policy_bindings"),
            "right.configuration_read", "manual", READ_REPORT_ACTIONS,
            "Secret values are redacted and writes require server admission.",
            "SBDV-FRM-101");
        surface(ROOT_PATH + ".parser-and-language", "parser-and-language", "grid",
            List.of("sys.parser.dialects", "sys.catalog_readable.parser_profiles",
                "sys.catalog_readable.resources"),
            "right.parser_profile_read", "manual", READ_REPORT_ACTIONS,
            "Parser and language source denial is reported as source-status refusal.",
            "SBDV-FRM-904");
        surface(ROOT_PATH + ".listener-and-manager", "listener-and-manager", "grid",
            List.of("sys.catalog_readable.listeners", "SHOW LISTENERS", "SHOW PARSERS"),
            "right.management_runtime_read", "manual_or_5s", READ_REPORT_ACTIONS,
            "Runtime inspection denial is reported as source-status refusal.",
            "SBDV-FRM-904");
        surface(ROOT_PATH + ".diagnostics", "diagnostics", "report",
            List.of("sys.catalog_readable.metrics_catalog", "sys.catalog_readable.diagnostics_catalog",
                "SHOW METRICS"),
            "right.management_runtime_read", "manual_or_30s", READ_REPORT_ACTIONS,
            "Denied diagnostics are visible only as source-status refusal.",
            "SBDV-FRM-900");
        surface(ROOT_PATH + ".support", "support", "action_form",
            List.of("sys.catalog_readable.diagnostics_catalog", "SHOW MANAGEMENT"),
            "right.support_bundle_read", "manual", TASK_REPORT_ACTIONS,
            "Support bundle actions require server admission and redaction proof.",
            "SBDV-FRM-900");
        surface(ROOT_PATH + ".triggers", "triggers", "grid",
            List.of("sys.catalog_readable.triggers"),
            "right.discover", "manual", READ_REPORT_ACTIONS,
            "Invisible triggers are hidden; lifecycle actions require server admission.",
            "SBDV-FRM-107");
        surface(ROOT_PATH + ".file-spaces", "file-spaces", "grid",
            List.of("sys.catalog_readable.filespaces"),
            "right.storage_read", "manual", READ_REPORT_ACTIONS,
            "Filespace paths are redacted by policy and unauthorized filespaces are hidden.",
            "SBDV-FRM-903");
        surface(ROOT_PATH + ".cluster", "cluster", "dashboard",
            List.of("cluster.sys.catalog_readable.nodes", "SHOW CLUSTER STATE"),
            "right.cluster_inspect", "manual_or_30s", READ_REPORT_ACTIONS,
            "Cluster surfaces route to the provider boundary or deterministic stub refusal.",
            "SBDV-FRM-903");
        surface(ROOT_PATH + ".emulation", "emulation", "grid",
            List.of("sys.catalog_readable.emulation_profiles"),
            "right.parser_profile_read", "manual", READ_REPORT_ACTIONS,
            "Emulation is hidden when no visible emulation profile exists.",
            "SBDV-FRM-904");
        surface(ROOT_PATH + ".remote", "remote", "grid",
            List.of("sys.catalog_readable.remote_connections"),
            "right.remote_connection_read", "manual", READ_REPORT_ACTIONS,
            "Credentials are redacted and the branch is hidden when no visible remote connection exists.",
            "SBDV-FRM-904");
    }

    private ScratchBirdManagementSurfaceCatalog() {
    }

    @NotNull
    public static Collection<ScratchBirdManagementSurfaceDefinition> allSurfaces() {
        return SURFACES_BY_PATH.values();
    }

    @Nullable
    public static ScratchBirdManagementSurfaceDefinition find(@NotNull String path) {
        return SURFACES_BY_PATH.get(normalize(path));
    }

    public static boolean isManagementPath(@NotNull String path) {
        String normalized = normalize(path);
        return normalized.equals(ROOT_PATH) || normalized.startsWith(ROOT_PATH + ".");
    }

    public static boolean isReportPath(@NotNull String path) {
        return normalize(path).startsWith(REPORT_BASE_PATH + ".");
    }

    @NotNull
    public static List<String> baselineChildPaths() {
        return SURFACES_BY_PATH.keySet().stream()
            .filter(path -> path.startsWith(ROOT_PATH + "."))
            .filter(path -> path.indexOf('.', ROOT_PATH.length() + 1) < 0)
            .toList();
    }

    @NotNull
    public static Set<String> displayTypes() {
        return Set.of("dashboard", "grid", "detail_form", "action_form", "report");
    }

    @NotNull
    public static String reportNavigatorPath(@NotNull String branch, @NotNull String reportId) {
        return REPORT_BASE_PATH + "." + branch + "." + reportId;
    }

    private static void surface(
        @NotNull String path,
        @NotNull String label,
        @NotNull String displayType,
        @NotNull List<String> backingSources,
        @NotNull String requiredPermission,
        @NotNull String refreshPolicy,
        @NotNull List<ScratchBirdNavigatorActionRegistry.Action> actions,
        @NotNull String refusalBehavior,
        @NotNull String formId
    ) {
        if (!displayTypes().contains(displayType)) {
            throw new IllegalArgumentException("Unsupported ScratchBird management display type: " + displayType);
        }
        SURFACES_BY_PATH.put(normalize(path), new ScratchBirdManagementSurfaceDefinition(
            normalize(path),
            label,
            displayType,
            List.copyOf(backingSources),
            requiredPermission,
            refreshPolicy,
            List.copyOf(actions),
            refusalBehavior,
            formId));
    }

    @NotNull
    private static String normalize(@NotNull String path) {
        return path.toLowerCase(java.util.Locale.ENGLISH);
    }
}
