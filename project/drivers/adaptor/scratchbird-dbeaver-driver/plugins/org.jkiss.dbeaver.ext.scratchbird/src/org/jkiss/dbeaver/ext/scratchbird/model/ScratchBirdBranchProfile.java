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

import java.util.List;
import java.util.Locale;

public record ScratchBirdBranchProfile(
    @NotNull String id,
    @NotNull String label,
    @NotNull String formId,
    boolean createAllowed,
    boolean alterAllowed,
    boolean deleteAllowed,
    boolean taskAllowed,
    boolean reportAllowed,
    boolean sourceStatusAllowed,
    boolean inspectOnly,
    @NotNull List<String> expectedChildren,
    @NotNull List<String> focusFields
) {

    @NotNull
    public static ScratchBirdBranchProfile forPath(@NotNull String fullPath) {
        String normalized = normalize(fullPath);
        if (normalized.isEmpty()) {
            return genericNamespace();
        }
        if (normalized.equals("sys")) {
            return profile(
                "SBBP-SYS-ROOT",
                "System root",
                "SBDV-FRM-001",
                false,
                false,
                false,
                true,
                true,
                true,
                false,
                List.of("catalog branch", "security branch", "monitoring branch", "jobs branch", "config branch", "structure branch", "emulation branch", "cluster branch", "internals branch"),
                List.of("database UUID", "branch capability matrix", "protected sub-branches"));
        }
        if (normalized.equals("sys.config") || normalized.startsWith("sys.config.")) {
            return profile(
                "SBBP-SYS-CONFIG",
                "System configuration surface",
                "SBDV-FRM-101",
                false,
                true,
                false,
                true,
                true,
                true,
                false,
                List.of("config section", "config setting", "restart-required policy"),
                List.of("effective value", "apply mode", "restart requirement"));
        }
        if (normalized.equals("sys.jobs") || normalized.startsWith("sys.jobs.")) {
            return profile(
                "SBBP-SYS-JOBS",
                "Scheduler and job surface",
                "SBDV-FRM-102",
                true,
                true,
                true,
                true,
                true,
                true,
                false,
                List.of("job definition", "job run", "dependency edge", "schedule"),
                List.of("schedule", "last run state", "dependency graph"));
        }
        if (normalized.equals("sys.security") || normalized.startsWith("sys.security.")) {
            return profile(
                "SBBP-SYS-SECURITY",
                "Security and identity surface",
                "SBDV-FRM-103",
                true,
                true,
                true,
                true,
                true,
                true,
                false,
                List.of("role", "group", "grant mapping", "auth binding"),
                List.of("principal mapping", "grant posture", "auth precedence"));
        }
        if (normalized.equals("sys.cluster") || normalized.startsWith("sys.cluster.")) {
            return profile(
                "SBBP-SYS-CLUSTER",
                "Cluster control hub",
                "SBDV-FRM-104",
                false,
                true,
                false,
                true,
                true,
                true,
                false,
                List.of("routing control", "admission policy", "cluster task surface"),
                List.of("routing posture", "admission status", "operator readiness"));
        }
        if (normalized.equals("sys.emulation") || normalized.startsWith("sys.emulation.")) {
            return profile(
                "SBBP-SYS-EMULATION",
                "Emulation control hub",
                "SBDV-FRM-105",
                false,
                true,
                false,
                true,
                true,
                true,
                false,
                List.of("emulation engine", "listener publication", "connector task surface"),
                List.of("engine family", "listener binding", "publication root"));
        }
        if (normalized.equals("sys.monitoring") || normalized.startsWith("sys.monitoring.")) {
            return profile(
                "SBBP-SYS-MONITORING",
                "Monitoring and metrics hub",
                "SBDV-FRM-106",
                false,
                false,
                false,
                true,
                true,
                true,
                false,
                List.of("listener report", "parser-pool report", "support-readiness report"),
                List.of("source status", "time range", "process scope"));
        }
        if (normalized.equals("sys.domains") || normalized.startsWith("sys.domains.")) {
            return profile(
                "SBBP-SYS-DOMAINS",
                "System domains and datatype hub",
                "SBDV-FRM-611",
                true,
                true,
                true,
                false,
                true,
                true,
                false,
                List.of("domain", "datatype", "value contract"),
                List.of("datatype family", "default/collation policy", "value manager route"));
        }
        if (normalized.equals("sys.catalog") || normalized.startsWith("sys.catalog.")) {
            return profile(
                "SBBP-SYS-CATALOG",
                "Catalog truth inspector",
                "SBDV-FRM-107",
                false,
                false,
                false,
                true,
                true,
                true,
                true,
                List.of("object identity row", "parent UUID row", "resolver child row"),
                List.of("object UUID", "parent UUID", "resolver source status"));
        }
        if (normalized.equals("sys.structure") || normalized.startsWith("sys.structure.")) {
            return profile(
                "SBBP-SYS-STRUCTURE",
                "Structure policy surface",
                "SBDV-FRM-108",
                false,
                true,
                false,
                true,
                true,
                true,
                false,
                List.of("placement policy", "naming policy", "inheritance policy"),
                List.of("placement rule", "naming rule", "mutability policy"));
        }
        if (normalized.equals("sys.internals") || normalized.startsWith("sys.internals.")) {
            return profile(
                "SBBP-SYS-INTERNALS",
                "Internal runtime inspector",
                "SBDV-FRM-109",
                false,
                false,
                false,
                false,
                true,
                true,
                true,
                List.of("runtime snapshot", "internal capability probe"),
                List.of("operator-only visibility", "runtime state", "admission status"));
        }
        if (normalized.equals("users")) {
            return profile(
                "SBBP-USERS-ROOT",
                "Users root",
                "SBDV-FRM-001",
                false,
                false,
                false,
                false,
                true,
                true,
                false,
                List.of("public users", "home users", "groups"),
                List.of("principal collections", "home/profile layout", "publication posture"));
        }
        if (normalized.equals("users.public") || normalized.equals("users.home")) {
            return profile(
                "SBBP-USERS-COLLECTION",
                "User collection",
                "SBDV-FRM-200",
                true,
                false,
                false,
                false,
                true,
                true,
                false,
                List.of("user"),
                List.of("principal identity", "home layout", "default profile binding"));
        }
        if (normalized.startsWith("users.home.") && normalized.contains(".profiles.")) {
            return profile(
                "SBBP-USER-PROFILE",
                "User profile surface",
                "SBDV-FRM-202",
                false,
                true,
                false,
                false,
                true,
                true,
                false,
                List.of("local profile", "roaming profile"),
                List.of("profile scope", "workspace binding", "settings ownership"));
        }
        if (normalized.startsWith("users.home.") && pathContainsSegment(normalized, "profiles")) {
            return profile(
                "SBBP-USER-PROFILE-COLLECTION",
                "User profile collection",
                "SBDV-FRM-202",
                true,
                false,
                false,
                false,
                true,
                true,
                false,
                List.of("local profile", "roaming profile"),
                List.of("profile scope", "workspace binding", "settings ownership"));
        }
        if (normalized.startsWith("users.home.") && pathContainsSegment(normalized, "links")) {
            return profile(
                "SBBP-USER-LINKS",
                "Home link surface",
                "SBDV-FRM-203",
                true,
                true,
                true,
                true,
                true,
                true,
                false,
                List.of("home link", "mount target"),
                List.of("link target", "validation state", "publication behavior"));
        }
        if (normalized.startsWith("users.home.") && pathContainsSegment(normalized, "scratch")) {
            return profile(
                "SBBP-USER-SCRATCH",
                "Scratch workspace surface",
                "SBDV-FRM-204",
                true,
                true,
                true,
                true,
                true,
                true,
                false,
                List.of("scratch branch", "workspace object"),
                List.of("quota", "cleanup policy", "workspace persistence"));
        }
        if (normalized.startsWith("users.home.")) {
            return profile(
                "SBBP-USER-ENTITY",
                "User entity",
                "SBDV-FRM-200",
                false,
                true,
                true,
                true,
                true,
                true,
                false,
                List.of("profiles", "scratch", "links"),
                List.of("principal identity", "home branch", "effective grants"));
        }
        if (normalized.equals("users.groups")) {
            return profile(
                "SBBP-GROUP-COLLECTION",
                "Group collection",
                "SBDV-FRM-201",
                true,
                false,
                false,
                false,
                true,
                true,
                false,
                List.of("group"),
                List.of("ownership", "membership policy", "publication posture"));
        }
        if (normalized.startsWith("users.groups.") && pathContainsSegment(normalized, "users")) {
            return profile(
                "SBBP-GROUP-MEMBERSHIP",
                "Group membership surface",
                "SBDV-FRM-205",
                true,
                true,
                true,
                true,
                true,
                true,
                false,
                List.of("membership row", "delegated membership"),
                List.of("principal", "membership state", "inherited grants"));
        }
        if (normalized.startsWith("users.groups.") && pathContainsSegment(normalized, "public")) {
            return profile(
                "SBBP-GROUP-PUBLIC",
                "Group public surface",
                "SBDV-FRM-206",
                true,
                true,
                true,
                true,
                true,
                true,
                false,
                List.of("published object", "publication policy"),
                List.of("visibility", "preview-as behavior", "publication grants"));
        }
        if (normalized.startsWith("users.groups.") && pathContainsSegment(normalized, "data")) {
            return profile(
                "SBBP-GROUP-DATA",
                "Group data binding surface",
                "SBDV-FRM-207",
                true,
                true,
                true,
                true,
                true,
                true,
                false,
                List.of("binding policy", "quota policy"),
                List.of("data placement", "ownership", "quota binding"));
        }
        if (normalized.startsWith("users.groups.")) {
            return profile(
                "SBBP-GROUP-ENTITY",
                "Group entity",
                "SBDV-FRM-201",
                false,
                true,
                true,
                true,
                true,
                true,
                false,
                List.of("public surface", "membership surface", "data surface"),
                List.of("ownership", "group admins", "membership policy"));
        }
        if (normalized.equals("cluster")) {
            return profile(
                "SBBP-CLUSTER-ROOT",
                "Cluster root",
                "SBDV-FRM-001",
                false,
                false,
                false,
                true,
                true,
                true,
                false,
                List.of("users", "groups", "domains", "locations", "nodes", "shared"),
                List.of("topology scope", "operator posture", "cluster visibility"));
        }
        if (normalized.equals("cluster.users") || normalized.startsWith("cluster.users.")) {
            return clusterEntityProfile("SBBP-CLUSTER-USERS", "Cluster user surface", "SBDV-FRM-300");
        }
        if (normalized.equals("cluster.groups") || normalized.startsWith("cluster.groups.")) {
            return clusterEntityProfile("SBBP-CLUSTER-GROUPS", "Cluster group surface", "SBDV-FRM-301");
        }
        if (normalized.equals("cluster.domains") || normalized.startsWith("cluster.domains.")) {
            return clusterEntityProfile("SBBP-CLUSTER-DOMAINS", "Cluster domain surface", "SBDV-FRM-302");
        }
        if (normalized.equals("cluster.locations") || normalized.startsWith("cluster.locations.")) {
            return clusterEntityProfile("SBBP-CLUSTER-LOCATIONS", "Cluster location surface", "SBDV-FRM-303");
        }
        if (normalized.equals("cluster.nodes") || normalized.startsWith("cluster.nodes.")) {
            return clusterEntityProfile("SBBP-CLUSTER-NODES", "Cluster node surface", "SBDV-FRM-304");
        }
        if (normalized.equals("cluster.shared") || normalized.startsWith("cluster.shared.")) {
            return clusterEntityProfile("SBBP-CLUSTER-SHARED", "Shared resource surface", "SBDV-FRM-305");
        }
        if (normalized.equals("emulated")) {
            return profile(
                "SBBP-EMULATED-ROOT",
                "Emulated-engine collection",
                "SBDV-FRM-400",
                true,
                false,
                false,
                true,
                true,
                true,
                false,
                List.of("engine family"),
                List.of("engine capability", "default publication behavior", "listener layout"));
        }
        if (normalized.startsWith("emulated.")) {
            int depth = ScratchBirdNamespaceSemantics.getPathDepth(normalized);
            if (depth == 2) {
                return profile(
                    "SBBP-EMULATED-ENGINE",
                    "Emulated engine",
                    "SBDV-FRM-400",
                    true,
                    true,
                    true,
                    true,
                    true,
                    true,
                    false,
                    List.of("server"),
                    List.of("engine family", "listener defaults", "capability posture"));
            }
            if (depth == 3) {
                return profile(
                    "SBBP-EMULATED-SERVER",
                    "Emulated server",
                    "SBDV-FRM-401",
                    false,
                    true,
                    true,
                    true,
                    true,
                    true,
                    false,
                    List.of("reference layout"),
                    List.of("listener binding", "publication root", "isolation policy"));
            }
            return profile(
                "SBBP-EMULATED-LAYOUT",
                "Reference-layout browser",
                "SBDV-FRM-402",
                false,
                false,
                false,
                false,
                true,
                true,
                true,
                List.of("reference-native object"),
                List.of("reference metadata", "compatibility state", "published layout"));
        }
        if (normalized.equals("remote")) {
            return profile(
                "SBBP-REMOTE-ROOT",
                "Remote connector collection",
                "SBDV-FRM-001",
                true,
                false,
                false,
                true,
                true,
                true,
                false,
                List.of("links", "FDW", "adapter family"),
                List.of("connector families", "management posture", "endpoint visibility"));
        }
        if (normalized.equals("remote.links") || normalized.startsWith("remote.links.")) {
            return profile(
                "SBBP-REMOTE-LINKS",
                "Remote link surface",
                "SBDV-FRM-500",
                true,
                true,
                true,
                true,
                true,
                true,
                false,
                List.of("database connection"),
                List.of("endpoint URI", "auth profile", "retry posture"));
        }
        if (normalized.equals("remote.fdw") || normalized.startsWith("remote.fdw.")) {
            return profile(
                "SBBP-REMOTE-FDW",
                "FDW surface",
                "SBDV-FRM-501",
                true,
                false,
                false,
                true,
                true,
                true,
                false,
                List.of("foreign data wrapper"),
                List.of("wrapper family", "capability posture", "import policy"));
        }
        if (normalized.startsWith("remote.")) {
            int depth = ScratchBirdNamespaceSemantics.getPathDepth(normalized);
            if (depth == 3) {
                return profile(
                    "SBBP-REMOTE-ENDPOINT",
                    "Remote endpoint or adapter",
                    "SBDV-FRM-502",
                    false,
                    true,
                    true,
                    true,
                    true,
                    true,
                    false,
                    List.of("mapping policy", "capability policy"),
                    List.of("endpoint capability", "auth posture", "health state"));
            }
            if (depth >= 4) {
                return profile(
                    "SBBP-REMOTE-POLICY",
                    "Remote schema-mapping and policy surface",
                    "SBDV-FRM-503",
                    true,
                    true,
                    true,
                    true,
                    true,
                    true,
                    false,
                    List.of("mapping rule", "capability rule"),
                    List.of("schema mapping", "type translation", "pushdown policy"));
            }
        }
        if (normalized.equals("data")) {
            return profile(
                "SBBP-DATA-ROOT",
                "Data namespace root",
                "SBDV-FRM-001",
                true,
                false,
                false,
                false,
                true,
                true,
                false,
                List.of("application namespace", "domain namespace"),
                List.of("publication root", "object placement", "namespace ownership"));
        }
        if (normalized.startsWith("data.")) {
            return profile(
                "SBBP-DATA-NAMESPACE",
                "Data namespace",
                "SBDV-FRM-001",
                true,
                true,
                true,
                true,
                true,
                true,
                false,
                List.of("child namespace", "table", "view", "sequence", "routine"),
                List.of("namespace path", "owner", "object-family mix"));
        }
        return genericNamespace();
    }

    @NotNull
    public List<ScratchBirdNavigatorActionRegistry.Action> actions() {
        java.util.ArrayList<ScratchBirdNavigatorActionRegistry.Action> actions = new java.util.ArrayList<>();
        if (createAllowed) {
            actions.add(ScratchBirdNavigatorActionRegistry.Action.NEW);
        }
        actions.add(ScratchBirdNavigatorActionRegistry.Action.PROPERTIES);
        if (alterAllowed) {
            actions.add(ScratchBirdNavigatorActionRegistry.Action.ALTER);
        }
        if (deleteAllowed) {
            actions.add(ScratchBirdNavigatorActionRegistry.Action.DELETE);
        }
        if (taskAllowed) {
            actions.add(ScratchBirdNavigatorActionRegistry.Action.TASKS);
        }
        if (reportAllowed) {
            actions.add(ScratchBirdNavigatorActionRegistry.Action.REPORTS);
        }
        actions.add(ScratchBirdNavigatorActionRegistry.Action.REFRESH);
        if (sourceStatusAllowed) {
            actions.add(ScratchBirdNavigatorActionRegistry.Action.SOURCE_STATUS);
        }
        return List.copyOf(actions);
    }

    @NotNull
    public String mutationSummary() {
        if (inspectOnly) {
            return "inspect-only surface";
        }
        if (createAllowed && alterAllowed && deleteAllowed) {
            return "full lifecycle surface";
        }
        if (!createAllowed && alterAllowed && !deleteAllowed) {
            return "alter-only control surface";
        }
        if (!createAllowed && !alterAllowed && !deleteAllowed) {
            return "non-mutating control surface";
        }
        if (createAllowed && !alterAllowed && !deleteAllowed) {
            return "collection surface";
        }
        return "mixed lifecycle surface";
    }

    public boolean allows(@NotNull ScratchBirdFormMode mode) {
        return switch (mode) {
            case INSPECT, READ_ONLY -> true;
            case CREATE -> createAllowed;
            case ALTER -> alterAllowed;
            case DELETE -> deleteAllowed;
            case TASK -> taskAllowed;
            case REPORT -> reportAllowed;
        };
    }

    @NotNull
    private static ScratchBirdBranchProfile genericNamespace() {
        return profile(
            "SBBP-GENERIC",
            "Generic namespace",
            "SBDV-FRM-001",
            true,
            true,
            true,
            true,
            true,
            true,
            false,
            List.of("child namespace", "managed object"),
            List.of("namespace path", "ownership", "capability posture"));
    }

    @NotNull
    private static ScratchBirdBranchProfile clusterEntityProfile(
        @NotNull String id,
        @NotNull String label,
        @NotNull String formId
    ) {
        return profile(
            id,
            label,
            formId,
            true,
            true,
            true,
            true,
            true,
            true,
            false,
            List.of("cluster object"),
            List.of("health", "location", "role"));
    }

    @NotNull
    private static ScratchBirdBranchProfile profile(
        @NotNull String id,
        @NotNull String label,
        @NotNull String formId,
        boolean createAllowed,
        boolean alterAllowed,
        boolean deleteAllowed,
        boolean taskAllowed,
        boolean reportAllowed,
        boolean sourceStatusAllowed,
        boolean inspectOnly,
        @NotNull List<String> expectedChildren,
        @NotNull List<String> focusFields
    ) {
        return new ScratchBirdBranchProfile(
            id,
            label,
            formId,
            createAllowed,
            alterAllowed,
            deleteAllowed,
            taskAllowed,
            reportAllowed,
            sourceStatusAllowed,
            inspectOnly,
            List.copyOf(expectedChildren),
            List.copyOf(focusFields));
    }

    private static boolean pathContainsSegment(@NotNull String fullPath, @NotNull String segment) {
        String marker = "." + segment + ".";
        return fullPath.contains(marker) || fullPath.endsWith("." + segment);
    }

    @NotNull
    private static String normalize(@NotNull String value) {
        return value.toLowerCase(Locale.ENGLISH);
    }
}
