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

public final class ScratchBirdAgentManagementCatalog {

    public record AgentDefinition(
        @NotNull String typeId,
        @NotNull String layer,
        @NotNull String deployment,
        @NotNull String scope,
        @NotNull String authority,
        @NotNull String defaultActivation
    ) {
        public boolean clusterOnly() {
            return "cluster".equals(deployment);
        }

        public boolean enabledByDefault() {
            return !"disabled".equals(defaultActivation);
        }

        @NotNull
        public String displayState() {
            if (!enabledByDefault()) {
                return clusterOnly() ? "disabled until cluster provider" : "disabled by default";
            }
            return switch (defaultActivation) {
                case "observe_only" -> "observe-only";
                case "dry_run" -> "dry-run";
                case "recommend_only" -> "recommend-only";
                default -> defaultActivation;
            };
        }

        @NotNull
        public String sourceQuery() {
            return "SELECT * FROM sys.frontend.agents WHERE agent_type_id = '" + typeId + "'";
        }

        @NotNull
        public String policyQuery() {
            return "SELECT * FROM sys.frontend.agent_policies WHERE agent_name = '" + typeId + "'";
        }

        @NotNull
        public String actionQuery() {
            return "SELECT * FROM sys.frontend.agent_actions WHERE agent_name = '" + typeId + "'";
        }

        @NotNull
        public String metricDependencyQuery() {
            return "SELECT * FROM sys.frontend.agent_metric_dependencies WHERE agent_name = '" + typeId + "'";
        }

        @NotNull
        public String overrideQuery() {
            return "SELECT * FROM sys.frontend.agent_overrides WHERE target_name = '" + typeId + "'";
        }

        @NotNull
        public String evidenceQuery() {
            return "SELECT * FROM sys.frontend.agent_evidence WHERE agent_name = '" + typeId + "'";
        }

        @NotNull
        public String auditQuery() {
            return "SELECT * FROM sys.frontend.agent_audit";
        }

        @NotNull
        public List<String> detailLines() {
            return List.of(
                "Agent type: " + typeId,
                "Runtime layer: " + layer,
                "Deployment: " + deployment,
                "Scope: " + scope,
                "Authority class: " + authority,
                "Default activation: " + defaultActivation,
                "Default state: " + displayState());
        }
    }

    private static final List<AgentDefinition> CANONICAL_AGENTS = List.of(
        agent("node_resource_agent", "L1 observation", "local", "node", "observe-only", "observe_only"),
        agent("metrics_registry_manager", "L2 recorder", "both", "node/database/cluster", "direct bounded action", "dry_run"),
        agent("storage_health_manager", "L3 dispatcher", "local", "node/database/filespace", "recommend-only", "recommend_only"),
        agent("filespace_capacity_manager", "L3 dispatcher", "local", "node/database/filespace", "request action", "recommend_only"),
        agent("page_allocation_manager", "L3 dispatcher", "local", "database/filespace/page_family/page_type", "request action", "recommend_only"),
        agent("memory_governor", "L3 dispatcher", "local", "node/database/session/workload", "direct bounded action", "dry_run"),
        agent("index_health_manager", "L3 dispatcher", "local", "database/index", "recommend-only", "recommend_only"),
        agent("cluster_autoscale_manager", "L3 dispatcher", "cluster", "cluster", "request action", "disabled"),
        agent("admission_control_manager", "L3 dispatcher", "both", "database/cluster/workload", "direct bounded action", "dry_run"),
        agent("parser_interface_manager", "L3 dispatcher", "local", "node/parser/interface", "request action", "recommend_only"),
        agent("transaction_pressure_manager", "L3 dispatcher", "both", "database/cluster", "request action", "recommend_only"),
        agent("storage_version_cleanup_agent", "L3 dispatcher", "local", "database/filespace/page_family/row_version", "direct bounded action", "dry_run"),
        agent("cleanup_archive_manager", "L3 dispatcher", "both", "database/cluster", "direct bounded action", "dry_run"),
        agent("policy_recommendation_manager", "L3 dispatcher", "both", "database/cluster", "recommend-only", "recommend_only"),
        agent("distributed_query_metrics_agent", "L1 observation", "cluster", "cluster/query", "observe-only", "disabled"),
        agent("remote_query_routing_agent", "L3 dispatcher", "cluster", "cluster/query/route", "request action", "disabled"),
        agent("runtime_learning_agent", "L3 dispatcher", "local", "database/optimizer", "recommend-only", "recommend_only"),
        agent("support_bundle_triage_agent", "L3 dispatcher", "both", "node/database/cluster/support", "request action", "recommend_only"),
        agent("cluster_scheduler_manager", "L3 dispatcher", "cluster", "cluster/jobs", "request action", "disabled"),
        agent("job_control_manager", "L3 dispatcher", "both", "database/cluster/jobs", "request action", "recommend_only"),
        agent("backup_manager", "L3 dispatcher", "both", "database/cluster/backup", "request action", "recommend_only"),
        agent("archive_manager", "L3 dispatcher", "both", "database/cluster/archive", "direct bounded action", "dry_run"),
        agent("restore_drill_manager", "L3 dispatcher", "both", "database/cluster/restore", "request action", "recommend_only"),
        agent("pitr_manager", "L3 dispatcher", "both", "database/cluster/pitr", "request action", "recommend_only"),
        agent("identity_manager", "L3 dispatcher", "both", "database/cluster/security", "request action", "recommend_only"),
        agent("session_control_manager", "L3 dispatcher", "both", "database/cluster/session", "request action", "recommend_only"),
        agent("alert_manager", "L3 dispatcher", "both", "node/database/cluster", "direct bounded action", "dry_run"),
        agent("export_adapter_manager", "L3 dispatcher", "both", "node/database/cluster/export", "request action", "recommend_only"),
        agent("cluster_upgrade_manager", "L3 dispatcher", "cluster", "cluster/upgrade", "request action", "disabled"));

    private ScratchBirdAgentManagementCatalog() {
    }

    @NotNull
    public static List<AgentDefinition> canonicalAgents() {
        return CANONICAL_AGENTS;
    }

    @NotNull
    private static AgentDefinition agent(
        @NotNull String typeId,
        @NotNull String layer,
        @NotNull String deployment,
        @NotNull String scope,
        @NotNull String authority,
        @NotNull String activation
    ) {
        return new AgentDefinition(typeId, layer, deployment, scope, authority, activation);
    }
}
