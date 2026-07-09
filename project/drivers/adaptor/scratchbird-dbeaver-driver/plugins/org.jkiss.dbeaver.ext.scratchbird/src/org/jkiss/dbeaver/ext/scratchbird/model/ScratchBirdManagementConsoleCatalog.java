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

public final class ScratchBirdManagementConsoleCatalog {

    public record ConsoleRow(
        @NotNull String key,
        @NotNull String label,
        @NotNull String kind,
        @NotNull String state,
        @NotNull String source,
        @NotNull String detail,
        boolean selectable,
        boolean enabled
    ) {
    }

    public record ConsoleDefinition(
        @NotNull String path,
        @NotNull String tabLabel,
        @NotNull String title,
        @NotNull String purpose,
        @NotNull String itemLabel,
        @NotNull List<ConsoleRow> rows,
        @NotNull List<String> detailSources,
        @NotNull List<String> monitoringSources,
        @NotNull List<String> mutationActions,
        @NotNull List<String> operatorNotes
    ) {
    }

    private ScratchBirdManagementConsoleCatalog() {
    }

    @NotNull
    public static ConsoleDefinition forSurface(@NotNull ScratchBirdManagementSurfaceDefinition surface) {
        String path = surface.path();
        return switch (path) {
            case "management.agents" -> agents(surface);
            case "management.security" -> grouped(surface, "Security", "Security Principals",
                "Manage user, group, role, policy, and authorization visibility through human-oriented security groupings.",
                List.of("Users", "Groups", "Roles", "Policies", "Configurations"),
                List.of("Inspect principal", "Preview grant/revoke", "Open policy editor"));
            case "management.sessions" -> grouped(surface, "Sessions", "Sessions And Transactions",
                "Inspect current sessions, transactions, role context, wait state, and cancellation eligibility.",
                List.of("Active sessions", "Transactions", "Session roles", "Waits", "Cancelable work"),
                List.of("Inspect session", "Cancel statement", "Disconnect session"));
            case "management.workload" -> grouped(surface, "Workload", "Workload And Statement Activity",
                "Inspect active and recent statements, admission posture, parser route, and command timing.",
                List.of("Running statements", "Prepared statements", "Admission queues", "Slow statements", "Parser routes"),
                List.of("Inspect statement", "Explain statement", "Cancel statement"));
            case "management.storage" -> grouped(surface, "Storage", "Storage And Filespaces",
                "Inspect filespaces, page families, capacity pressure, preallocation posture, and storage health.",
                List.of("Filespaces", "Page families", "Preallocation", "Capacity pressure", "Storage health"),
                List.of("Inspect filespace", "Preview grow filespace", "Run storage health"));
            case "management.memory" -> grouped(surface, "Memory", "Memory, Cache, And Buffer Policy",
                "Inspect memory policy, cache pressure, buffer residency, reservation failures, and adaptive limits.",
                List.of("Policy", "Page cache", "Descriptor cache", "Rowset buffers", "Pressure events"),
                List.of("Inspect memory policy", "Preview cache resize", "Run memory diagnostic"));
            case "management.programmability" -> grouped(surface, "Programmability", "Programmability",
                "Browse routines, packages, functions, procedures, exceptions, events, and lifecycle actions.",
                List.of("Procedures", "Functions", "Packages", "Exceptions", "Events"),
                List.of("Open routine", "Preview compile", "Inspect dependencies"));
            case "management.domains" -> grouped(surface, "Domains", "Domains And Datatype Policy",
                "Browse database/cluster scoped domains, datatypes, casts, and validation rules.",
                List.of("Domains", "Datatypes", "Casts", "Validation rules"),
                List.of("Open domain", "Preview domain alter", "Inspect casts"));
            case "management.jobs" -> grouped(surface, "Jobs", "Jobs And Schedules",
                "Inspect job definitions, dependencies, recent runs, recurring schedules, and failure state.",
                List.of("Jobs", "Schedules", "Runs", "Dependencies", "Failures"),
                List.of("Inspect job", "Preview enable/disable", "Replay failed run"));
            case "management.configuration" -> grouped(surface, "Configuration", "Configuration And Policy Bindings",
                "Inspect effective settings, policy profiles, bindings, redaction state, and pending changes.",
                List.of("Settings", "Profiles", "Policy bindings", "Effective values", "Redactions"),
                List.of("Open setting", "Preview policy edit", "Validate configuration"));
            case "management.parser-and-language" -> grouped(surface, "Parser And Language", "Parser, Dialect, And Language Resources",
                "Inspect parser dialects, language resources, localization posture, and parser lifecycle controls.",
                List.of("Dialects", "Language resources", "Parser profiles", "Completion resources", "Resource verification"),
                List.of("Inspect dialect", "Validate language resource", "Preview parser recycle"));
            case "management.listener-and-manager" -> grouped(surface, "Listener And Manager", "Listener, Manager, And Front Door",
                "Inspect listener state, parser pool, manager state, route admission, and protocol health.",
                List.of("Listeners", "Manager", "Parser pool", "Routes", "Handshake failures"),
                List.of("Inspect listener", "Preview drain", "Run front-door diagnostic"));
            case "management.diagnostics" -> grouped(surface, "Diagnostics", "Diagnostics And Metrics",
                "Browse diagnostic catalogs, metrics families, alert starters, and graph/report entry points.",
                List.of("Metrics catalog", "Diagnostics catalog", "Alerts", "Graphs", "Reports"),
                List.of("Open report", "Create alert starter", "Refresh metric sample"));
            case "management.support" -> grouped(surface, "Support", "Support Bundle And Evidence",
                "Prepare redacted support bundle reviews and inspect supportability sources.",
                List.of("Bundle readiness", "Redaction policy", "Evidence chain", "Export targets"),
                List.of("Preview bundle", "Validate redaction", "Prepare support export"));
            case "management.triggers" -> grouped(surface, "Triggers", "Triggers",
                "Browse database, table, view, and event triggers with dependency and lifecycle context.",
                List.of("Database triggers", "Table triggers", "View triggers", "Event triggers"),
                List.of("Open trigger", "Preview enable/disable", "Inspect trigger dependencies"));
            case "management.file-spaces" -> grouped(surface, "File-Spaces", "File-Spaces",
                "Inspect filespace inventory, capacity, owner node, and policy redaction state.",
                List.of("Filespaces", "Capacity", "Device policy", "Growth plans"),
                List.of("Inspect filespace", "Preview grow", "Run capacity diagnostic"));
            case "management.cluster" -> grouped(surface, "Cluster", "Cluster Provider Boundary",
                "Inspect cluster-provider route status and deterministic stub refusals in public single-node builds.",
                List.of("Provider boundary", "Nodes", "Leadership", "Routing", "Refusals"),
                List.of("Inspect cluster boundary", "Show provider refusal"));
            case "management.emulation" -> grouped(surface, "Emulation", "Reference-System Emulation Profiles",
                "Inspect installed emulation profiles, parser coverage, support UDRs, and exact refusal behavior.",
                List.of("Profiles", "Parser families", "Support UDRs", "Replay gates", "Refusals"),
                List.of("Inspect profile", "Open compatibility status"));
            case "management.remote" -> grouped(surface, "Remote", "Remote Connections",
                "Inspect visible remote connections, credential redaction, route status, and link health.",
                List.of("Connections", "Credential redaction", "Route health", "Link policies"),
                List.of("Inspect connection", "Validate route"));
            case "management.overview", "management" -> grouped(surface, "Overview", "Management Overview",
                "Show the management source inventory, source refusals, and operator entry points.",
                List.of("Source inventory", "Server capabilities", "Navigator tree", "Metric overview"),
                List.of("Refresh overview", "Open source status"));
            default -> generic(surface);
        };
    }

    @NotNull
    private static ConsoleDefinition agents(@NotNull ScratchBirdManagementSurfaceDefinition surface) {
        List<ConsoleRow> rows = new ArrayList<>();
        for (ScratchBirdAgentManagementCatalog.AgentDefinition agent : ScratchBirdAgentManagementCatalog.canonicalAgents()) {
            rows.add(new ConsoleRow(
                agent.typeId(),
                agent.typeId(),
                agent.layer(),
                agent.displayState(),
                agent.scope(),
                String.join("\n", agent.detailLines()),
                true,
                agent.enabledByDefault()));
        }
        return new ConsoleDefinition(
            surface.path(),
            "Agents",
            "Agent Runtime Console",
            "Browse canonical ScratchBird agents, their L1-L5 role, policy source, schedule source, running status source, and admissible actions.",
            "Agent",
            List.copyOf(rows),
            List.of(
                "SELECT * FROM sys.frontend.agents",
                "SELECT * FROM sys.frontend.agent_policies",
                "SELECT * FROM sys.frontend.agent_actions",
                "SELECT * FROM sys.frontend.agent_metric_dependencies",
                "SELECT * FROM sys.frontend.agent_overrides",
                "SELECT * FROM sys.frontend.agent_evidence",
                "SELECT * FROM sys.frontend.agent_audit"),
            List.of(
                "sys.frontend.agents",
                "sys.frontend.agent_policies",
                "sys.frontend.agent_actions",
                "sys.frontend.agent_metric_dependencies",
                "sys.frontend.agent_overrides",
                "sys.frontend.agent_evidence",
                "sys.frontend.agent_audit"),
            List.of(
                "Inspect selected agent",
                "Preview enable/disable policy change",
                "Preview restart/quarantine when server admission allows it",
                "Open related policy and action history"),
            List.of(
                "Disabled agents remain visible and selectable so operators can inspect why they are inactive.",
                "Cluster-only agents remain disabled in the public single-node build unless the cluster provider route is compiled and visible.",
                "Runtime state, schedule, policy, and action history must come from live server sources or a deterministic refusal."));
    }

    @NotNull
    private static ConsoleDefinition grouped(
        @NotNull ScratchBirdManagementSurfaceDefinition surface,
        @NotNull String tabLabel,
        @NotNull String title,
        @NotNull String purpose,
        @NotNull List<String> groups,
        @NotNull List<String> mutationActions
    ) {
        List<ConsoleRow> rows = new ArrayList<>();
        for (String group : groups) {
            String key = normalizedKey(group);
            rows.add(new ConsoleRow(
                key,
                group,
                surface.displayType(),
                "server-backed",
                "authorization-filtered",
                "Group: " + group + "\nRows and detail panes are populated from the connected ScratchBird server when visible to the current session.",
                true,
                true));
        }
        return new ConsoleDefinition(
            surface.path(),
            tabLabel,
            title,
            purpose,
            "Item",
            List.copyOf(rows),
            sourceQueries(surface),
            surface.backingSources(),
            mutationActions,
            List.of(
                "Rows are authorization-filtered by the connected ScratchBird server.",
                "Mutation actions are preview-only until server admission validates the exact command and session authority.",
                "Unavailable sources must display deterministic refusal text rather than placeholder data."));
    }

    @NotNull
    private static ConsoleDefinition generic(@NotNull ScratchBirdManagementSurfaceDefinition surface) {
        return grouped(
            surface,
            titleCase(surface.label()),
            titleCase(surface.label()),
            "ScratchBird-owned management surface for " + surface.path() + ".",
            surface.backingSources(),
            List.of("Inspect", "Refresh source status"));
    }

    @NotNull
    private static List<String> sourceQueries(@NotNull ScratchBirdManagementSurfaceDefinition surface) {
        return surface.backingSources().stream()
            .map(ScratchBirdManagementConsoleCatalog::sourceCommand)
            .toList();
    }

    @NotNull
    private static String sourcePlan(@NotNull ScratchBirdManagementSurfaceDefinition surface) {
        return String.join("\n", sourceQueries(surface));
    }

    @NotNull
    private static String firstSource(@NotNull ScratchBirdManagementSurfaceDefinition surface) {
        return surface.backingSources().isEmpty() ? "-" : sourceCommand(surface.backingSources().get(0));
    }

    @NotNull
    private static String sourceCommand(@NotNull String source) {
        return source.startsWith("SHOW ") ? source : "SELECT * FROM " + source;
    }

    @NotNull
    private static String normalizedKey(@NotNull String value) {
        return value.toLowerCase(Locale.ENGLISH).replaceAll("[^a-z0-9]+", "_").replaceAll("^_|_$", "");
    }

    @NotNull
    private static String titleCase(@NotNull String value) {
        String[] parts = value.replace('-', ' ').split("\\s+");
        List<String> titled = new ArrayList<>();
        for (String part : parts) {
            if (part.isEmpty()) {
                continue;
            }
            titled.add(part.substring(0, 1).toUpperCase(Locale.ENGLISH) + part.substring(1));
        }
        return titled.isEmpty() ? value : String.join(" ", titled);
    }
}
