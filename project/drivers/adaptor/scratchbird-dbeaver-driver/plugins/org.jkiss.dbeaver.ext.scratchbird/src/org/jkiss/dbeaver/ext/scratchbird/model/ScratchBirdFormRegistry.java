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
import org.jkiss.dbeaver.ext.generic.model.GenericDataType;
import org.jkiss.dbeaver.ext.generic.model.GenericProcedure;
import org.jkiss.dbeaver.ext.generic.model.GenericSequence;
import org.jkiss.dbeaver.ext.generic.model.GenericTableBase;
import org.jkiss.dbeaver.ext.generic.model.GenericTableColumn;
import org.jkiss.dbeaver.ext.generic.model.GenericTableForeignKey;
import org.jkiss.dbeaver.ext.generic.model.GenericTableIndex;
import org.jkiss.dbeaver.ext.generic.model.GenericTrigger;
import org.jkiss.dbeaver.ext.generic.model.GenericUniqueKey;
import org.jkiss.dbeaver.model.struct.DBSObject;

import java.util.Collection;
import java.util.EnumSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public final class ScratchBirdFormRegistry {

    private static final List<String> SHELL_MUST = List.of(
        "object type",
        "canonical path or qualified name",
        "object display name",
        "persisted versus draft state",
        "connection or database target",
        "effective principal or role context",
        "read-only versus editable mode indicator");

    private static final List<String> SHELL_SHOULD = List.of(
        "internal UUID or stable server identity",
        "owner or parent object",
        "created timestamp",
        "updated timestamp",
        "status banner for validation or server refusal");

    private static final List<String> SHELL_OPTIONAL = List.of(
        "raw definition SQL",
        "raw metadata JSON",
        "audit or change-history panel",
        "notes or operator comments");

    private static final Map<String, ScratchBirdFormDefinition> FORMS_BY_ID = new LinkedHashMap<>();

    static {
        register("SBDV-FRM-000", "Shared Form Shell", "all forms",
            "Common identity, authority, status, mode, and audit shell.", modes(ScratchBirdFormMode.INSPECT),
            SHELL_MUST, SHELL_SHOULD, SHELL_OPTIONAL);
        register("SBDV-FRM-001", "Namespace And Branch Editor", "canonical root branches and recursive schema branches",
            "Inspect and manage UUID-root branch identity, parentage, visibility, mutability, and child creation capability.",
            allLifecycleModes(), plus(SHELL_MUST, "database UUID", "object UUID", "parent UUID", "branch full path", "catalog-backed/client-only flag"),
            plus(SHELL_SHOULD, "branch capability probe", "child count", "source surface status"),
            plus(SHELL_OPTIONAL, "raw sys.schemas row", "raw sys.catalog.object_resolver row"),
            "SBDV-FRM-014", "SBDV-FRM-015", "SBDV-FRM-016");
        register("SBDV-FRM-010", "ScratchBird Data Source Connection Form", "DBeaver connection wizard or edit page",
            "Create, edit, validate, and persist a ScratchBird JDBC lane connection definition.",
            modes(ScratchBirdFormMode.INSPECT, ScratchBirdFormMode.ALTER),
            List.of("connection name", "host", "port", "database label", "driver/protocol lane", "authentication mode", "SSL or TLS mode", "connection test action"),
            List.of("default branch or schema", "connect timeout", "socket timeout", "effective role", "default transaction mode"),
            List.of("SSH/tunnel/proxy profile", "client application tag", "startup validation query", "connection debug tracing toggle"),
            "SBDV-FRM-011", "SBDV-FRM-012", "SBDV-FRM-013", "SBDV-FRM-015");
        register("SBDV-FRM-011", "Transport And Wire-Protocol Form", "connection transport and handshake",
            "Configure native ScratchBird transport, handshake, SSL/TLS, and compatibility behavior.",
            modes(ScratchBirdFormMode.INSPECT, ScratchBirdFormMode.ALTER),
            List.of("host", "port", "protocol lane", "handshake/capability summary", "SSL or TLS mode", "server compatibility result"),
            List.of("connect timeout", "socket timeout", "compression policy", "channel-binding policy", "server identity preview"),
            List.of("trust store", "key store", "wire trace toggle", "front-door mode"));
        register("SBDV-FRM-012", "Authentication And Secret Form", "connection authentication",
            "Configure username/password, token, manager, dormant reattach, and workload identity authentication.",
            modes(ScratchBirdFormMode.INSPECT, ScratchBirdFormMode.ALTER),
            List.of("user", "password or token secret", "auth method id", "auth payload", "required/forbidden methods"),
            List.of("manager auth token", "workload identity token", "proxy principal assertion", "channel-binding requirement"),
            List.of("auth provider profile", "dormant id", "dormant reattach token", "credential rotation notes"));
        register("SBDV-FRM-013", "Session Defaults And Startup Form", "connection session defaults",
            "Configure role, schema, search path, startup flags, and client identity.",
            modes(ScratchBirdFormMode.INSPECT, ScratchBirdFormMode.ALTER),
            List.of("current schema", "search path", "role", "application name", "startup validation"),
            List.of("read-only default", "transaction isolation default", "manager client intent", "manager client flags"),
            List.of("startup SQL", "session variables", "diagnostic tags"));
        register("SBDV-FRM-014", "Permission-Aware Destructive Action Wizard", "destructive lifecycle actions",
            "Confirm and preview delete, drop, reset, disable, detach, or irreversible alter operations.",
            modes(ScratchBirdFormMode.DELETE),
            List.of("target object", "destructive action", "server capability/refusal", "generated SQL or command", "explicit confirmation phrase"),
            List.of("dependent object preview", "dry-run result", "audit note", "schedule-as-job option"),
            List.of("rollback guidance", "support bundle recommendation"));
        register("SBDV-FRM-015", "Validation, Lint, And Parser Diagnostics Panel", "SQL-bearing or rule-bearing forms",
            "Attach optional Java v3 parser diagnostics without overriding server execution or authorization.",
            modes(ScratchBirdFormMode.INSPECT),
            List.of("SQL or rule text", "parser lane status", "diagnostic list", "server-authoritative warning"),
            List.of("source-position map", "lint hints", "quick-fix candidates"),
            List.of("raw parser payload", "grammar version", "diagnostic reference UUID"));
        register("SBDV-FRM-016", "Admin Task Launch And Execution Wizard", "immediate administrative tasks",
            "Launch server-admitted admin tasks with preview, validate-only, execute, schedule, and result-log modes.",
            modes(ScratchBirdFormMode.TASK),
            List.of("task type", "target scope", "server capability/refusal", "generated command", "execution mode"),
            List.of("dry-run", "validate-only", "schedule-as-job", "result log"),
            List.of("task parameters JSON", "retry policy", "operator note"),
            "SBDV-FRM-014", "SBDV-FRM-015");

        branch("SBDV-FRM-101", "Configuration Editor", "sys.config", "Inspect and mutate admitted configuration objects.");
        branch("SBDV-FRM-102", "Job And Schedule Editor", "sys.jobs", "Inspect and manage scheduler jobs, runs, dependencies, and schedules.");
        branch("SBDV-FRM-103", "Security Mapping And Identity Editor", "sys.security", "Inspect users, groups, roles, auth mappings, and security policy.");
        branch("SBDV-FRM-104", "Cluster Control Editor", "sys.cluster", "Inspect cluster control, routing, admission, and topology state.");
        branch("SBDV-FRM-105", "Emulation Control Editor", "sys.emulation", "Inspect and manage donor-engine emulation control objects.");
        branch("SBDV-FRM-106", "Monitoring Browser And Metrics Dashboard", "sys.monitoring", "Browse monitoring surfaces and jump to metrics dashboards.");
        branch("SBDV-FRM-107", "Catalog Browser And Object Identity Inspector", "sys.catalog", "Inspect catalog truth, object UUIDs, parent UUIDs, and resolver output.");
        branch("SBDV-FRM-108", "Structure Policy Editor", "sys.structure", "Inspect and manage structure policy and recursive namespace behavior.");
        branch("SBDV-FRM-109", "Internal Runtime Inspector", "sys.internals", "Inspect internal runtime state; mutation is disabled unless explicitly admitted.");
        branch("SBDV-FRM-110", "Grant, Role, And Ownership Editor", "security child pages and SQL objects", "Inspect and manage grants, roles, ownership, and effective permissions.");

        branch("SBDV-FRM-200", "User Editor", "users.public and users.home.<user>", "Manage user lifecycle, profile attachment, home links, scratch workspace, and grants.");
        branch("SBDV-FRM-201", "Group Editor", "users.groups.<group>", "Manage group lifecycle, membership, public surface, data bindings, and grants.");
        branch("SBDV-FRM-202", "Profile Editor", "users.home.<user>.profiles.*", "Manage local and roaming user profiles.");
        branch("SBDV-FRM-203", "Home Link Editor", "users.home.<user>.links", "Manage user home links and branch bindings.");
        branch("SBDV-FRM-204", "Scratch Workspace Editor", "users.home.<user>.scratch", "Manage user scratch workspace objects.");
        branch("SBDV-FRM-205", "Group Membership Editor", "users.groups.<group>.users", "Manage group membership and inherited grants.");
        branch("SBDV-FRM-206", "Group Public Surface Editor", "users.groups.<group>.public", "Manage group-public branches and grants.");
        branch("SBDV-FRM-207", "Group Data Binding Editor", "users.groups.<group>.data", "Manage group data bindings and lifecycle forms.");

        branch("SBDV-FRM-300", "Cluster User Editor", "cluster.users", "Manage cluster user identity and ownership surfaces.");
        branch("SBDV-FRM-301", "Cluster Group Editor", "cluster.groups", "Manage cluster group identity and grants.");
        branch("SBDV-FRM-302", "Domain Editor", "cluster.domains", "Manage cluster domain policy and node/resource child binding.");
        branch("SBDV-FRM-303", "Location Editor", "cluster.locations", "Manage cluster location topology.");
        branch("SBDV-FRM-304", "Node Editor", "cluster.nodes", "Manage node identity, health, and admitted operations.");
        branch("SBDV-FRM-305", "Shared Resource Editor", "cluster.shared", "Manage shared cluster resources.");

        branch("SBDV-FRM-400", "Emulation Engine Editor", "emulated.<engine>", "Manage donor emulation engine branches.");
        branch("SBDV-FRM-401", "Emulation Server Editor", "emulated.<engine>.<server>", "Manage donor emulation server endpoints.");
        branch("SBDV-FRM-402", "Donor-Layout Catalog Browser", "emulated.<engine>.<server>.<engine-native layout>", "Browse donor-native catalog layouts inspect-only by default.");

        branch("SBDV-FRM-500", "Remote Link Editor", "remote.links", "Manage remote links and connectivity dashboards.");
        branch("SBDV-FRM-501", "FDW Editor", "remote.fdw", "Manage foreign data wrapper definitions.");
        branch("SBDV-FRM-502", "Remote Endpoint Or Adapter Editor", "remote.<class_or_engine>.<endpoint>", "Manage remote endpoints, adapters, and capabilities.");
        branch("SBDV-FRM-503", "Remote Schema Mapping And Capability Policy Editor", "remote adapter mapping", "Manage schema mapping and capability policy.");

        sql("SBDV-FRM-601", "Relational Table Editor", "relational tables", "Create, inspect, alter, delete, preview SQL, and validate ScratchBird relational table definitions.");
        sql("SBDV-FRM-602", "Non-Relational Table Editor", "document, key-value, vector, graph-adjacent tables", "Create, inspect, alter, delete, and validate ScratchBird non-relational table definitions.");
        sql("SBDV-FRM-603", "Relational View Editor", "relational views", "Create, inspect, alter, delete, preview SQL, and validate relational views.");
        sql("SBDV-FRM-604", "Non-Relational View Editor", "document, graph, vector, search, mixed-model views", "Create, inspect, alter, delete, and validate non-relational views.");
        register("SBDV-FRM-605", "Column Or Field Editor", "fields and attributes",
            "Edit relational columns and non-relational fields with datatype/domain binding.",
            allLifecycleModes(),
            plus(SHELL_MUST, "object definition", "parent schema UUID", "server capability/refusal", "generated SQL preview"),
            plus(SHELL_SHOULD, "validation diagnostics", "grants", "metrics shortcut", "dependency preview"),
            plus(SHELL_OPTIONAL, "raw metadata JSON", "DDL history", "saved template"),
            "SBDV-FRM-014", "SBDV-FRM-015", "SBDV-FRM-110", "SBDV-FRM-613", "SBDV-FRM-901");
        sql("SBDV-FRM-606", "Constraint Editor", "key, foreign-key, unique, and check constraints", "Edit constraints with generated SQL preview and validation.");
        sql("SBDV-FRM-607", "Index Editor", "relational and multi-model indexes", "Edit relational, document, vector, search, and hybrid indexes.");
        sql("SBDV-FRM-608", "Sequence Editor", "sequences and generators", "Edit sequence current-value, cache, cycle, and identity binding.");
        sql("SBDV-FRM-609", "Routine Editor", "procedures and functions", "Edit procedure and function definitions.");
        sql("SBDV-FRM-610", "Trigger Editor", "triggers", "Edit triggers and firing conditions.");
        register("SBDV-FRM-611", "Domain And Datatype Definition Editor", "sys.domains",
            "Create, inspect, alter, and delete ScratchBird domains and datatype definitions.",
            allLifecycleModes(),
            plus(SHELL_MUST, "object definition", "parent schema UUID", "server capability/refusal", "generated SQL preview"),
            plus(SHELL_SHOULD, "validation diagnostics", "grants", "metrics shortcut", "dependency preview"),
            plus(SHELL_OPTIONAL, "raw metadata JSON", "DDL history", "saved template"),
            "SBDV-FRM-014", "SBDV-FRM-015", "SBDV-FRM-110", "SBDV-FRM-613", "SBDV-FRM-901");
        register("SBDV-FRM-612", "Payload Model Editor", "non-relational payload and shape contracts",
            "Edit payload shape, identity, versioning, and flexible-schema policy.",
            allLifecycleModes(),
            plus(SHELL_MUST, "object definition", "parent schema UUID", "server capability/refusal", "generated SQL preview"),
            plus(SHELL_SHOULD, "validation diagnostics", "grants", "metrics shortcut", "dependency preview"),
            plus(SHELL_OPTIONAL, "raw metadata JSON", "DDL history", "saved template"),
            "SBDV-FRM-014", "SBDV-FRM-015", "SBDV-FRM-110", "SBDV-FRM-613", "SBDV-FRM-901");
        register("SBDV-FRM-613", "ScratchBird Value Manager And Literal Editor", "datatype-aware value editing",
            "Display, edit, serialize, literalize, and validate ScratchBird values.",
            modes(ScratchBirdFormMode.INSPECT, ScratchBirdFormMode.ALTER),
            List.of("datatype family", "null state", "display value", "edit value", "SQL literal", "validation result"),
            List.of("raw payload", "copy-as-SQL-literal", "roundtrip status", "domain binding"),
            List.of("binary/large value viewer", "structured payload viewer", "format preset"),
            "SBDV-FRM-611", "SBDV-FRM-015");

        register("SBDV-FRM-900", "Management Home Dashboard", "ScratchBird management landing surface",
            "Open the main ScratchBird management surface and drill into admin, report, task, and connection forms.",
            modes(ScratchBirdFormMode.INSPECT, ScratchBirdFormMode.REPORT),
            List.of("connection", "database", "principal", "source status", "health summary", "navigation shortcuts"),
            List.of("pinned reports", "recent refusals", "active tasks", "server capabilities"),
            List.of("saved presets", "support bundle readiness"),
            "SBDV-FRM-010", "SBDV-FRM-016", "SBDV-FRM-106", "SBDV-FRM-901", "SBDV-FRM-902", "SBDV-FRM-903", "SBDV-FRM-904");
        register("SBDV-FRM-901", "Object Metrics Panel", "object-level metrics",
            "Show object-level metrics and report drilldowns.", reportModes(), reportMust(), reportShould(), reportOptional());
        register("SBDV-FRM-902", "Branch Metrics Dashboard", "branch or schema metrics",
            "Show branch or schema metrics and report drilldowns.", reportModes(), reportMust(), reportShould(), reportOptional());
        register("SBDV-FRM-903", "Database Or Node Operations Dashboard", "database, cluster, node, domain, and resource metrics",
            "Show database, cluster, node, domain, and resource operations metrics.", reportModes(), reportMust(), reportShould(), reportOptional());
        register("SBDV-FRM-904", "Emulation And Remote Connectivity Dashboard", "emulation, protocol, and remote connectivity metrics",
            "Show emulation, listener, parser, and remote connectivity metrics.", reportModes(), reportMust(), reportShould(), reportOptional());
    }

    private ScratchBirdFormRegistry() {
    }

    @NotNull
    public static Collection<ScratchBirdFormDefinition> allForms() {
        return FORMS_BY_ID.values();
    }

    @Nullable
    public static ScratchBirdFormDefinition find(@NotNull String formId) {
        return FORMS_BY_ID.get(formId);
    }

    @NotNull
    public static ScratchBirdFormDefinition require(@NotNull String formId) {
        ScratchBirdFormDefinition definition = find(formId);
        if (definition == null) {
            throw new IllegalArgumentException("Unknown ScratchBird form: " + formId);
        }
        return definition;
    }

    @NotNull
    public static ScratchBirdFormDefinition resolveForPath(
        @NotNull String fullPath,
        @NotNull ScratchBirdNavigatorActionRegistry.Action action
    ) {
        ScratchBirdReportDefinition report = ScratchBirdReportCatalog.findByNavigatorPath(fullPath);
        if (report != null) {
            return require(report.parentForm());
        }
        if (ScratchBirdNamespaceSemantics.isMetricsPath(fullPath)) {
            return require(action == ScratchBirdNavigatorActionRegistry.Action.SOURCE_STATUS ? "SBDV-FRM-901" : "SBDV-FRM-900");
        }
        if (action == ScratchBirdNavigatorActionRegistry.Action.DELETE) {
            return require("SBDV-FRM-014");
        }
        if (action == ScratchBirdNavigatorActionRegistry.Action.TASKS) {
            return require("SBDV-FRM-016");
        }
        if (ScratchBirdNamespaceSemantics.isDomainPath(fullPath)) {
            return require("SBDV-FRM-611");
        }
        return require(ScratchBirdBranchProfile.forPath(fullPath).formId());
    }

    @NotNull
    public static ScratchBirdFormDefinition resolveForObject(
        @NotNull DBSObject object,
        @NotNull ScratchBirdNavigatorActionRegistry.Action action
    ) {
        if (object instanceof ScratchBirdSchemaNode schemaNode) {
            return resolveForPath(schemaNode.getFullPath(), action);
        }
        if (action == ScratchBirdNavigatorActionRegistry.Action.DELETE) {
            return require("SBDV-FRM-014");
        }
        if (object instanceof GenericTableColumn) {
            return require("SBDV-FRM-605");
        }
        if (object instanceof GenericTableForeignKey) {
            return require("SBDV-FRM-606");
        }
        if (object instanceof GenericUniqueKey) {
            return require("SBDV-FRM-606");
        }
        if (object instanceof GenericTableIndex) {
            return require("SBDV-FRM-607");
        }
        if (object instanceof GenericSequence) {
            return require("SBDV-FRM-608");
        }
        if (object instanceof GenericProcedure) {
            return require("SBDV-FRM-609");
        }
        if (object instanceof GenericTrigger) {
            return require("SBDV-FRM-610");
        }
        if (object instanceof GenericDataType) {
            return require("SBDV-FRM-611");
        }
        if (object instanceof GenericTableBase table) {
            boolean nonRelational = ScratchBirdObjectFormContext.isLikelyNonRelationalTable(table);
            return require(table.isView() ? (nonRelational ? "SBDV-FRM-604" : "SBDV-FRM-603") :
                (nonRelational ? "SBDV-FRM-602" : "SBDV-FRM-601"));
        }
        return require("SBDV-FRM-001");
    }

    private static void branch(@NotNull String id, @NotNull String name, @NotNull String scope, @NotNull String purpose) {
        register(id, name, scope, purpose, allLifecycleModes(),
            plus(SHELL_MUST, "full path", "object UUID", "parent UUID", "visibility", "mutability"),
            plus(SHELL_SHOULD, "owner", "capability/refusal status", "grants", "metrics shortcut"),
            plus(SHELL_OPTIONAL, "raw sys row", "change history", "operator note"),
            "SBDV-FRM-014", "SBDV-FRM-015", "SBDV-FRM-016", "SBDV-FRM-901", "SBDV-FRM-902");
    }

    private static void sql(@NotNull String id, @NotNull String name, @NotNull String scope, @NotNull String purpose) {
        register(id, name, scope, purpose, allLifecycleModes(),
            plus(SHELL_MUST, "object definition", "parent schema UUID", "server capability/refusal", "generated SQL preview"),
            plus(SHELL_SHOULD, "validation diagnostics", "grants", "metrics shortcut", "dependency preview"),
            plus(SHELL_OPTIONAL, "raw metadata JSON", "DDL history", "saved template"),
            "SBDV-FRM-014", "SBDV-FRM-015", "SBDV-FRM-110", "SBDV-FRM-901");
    }

    private static void register(
        @NotNull String id,
        @NotNull String name,
        @NotNull String scope,
        @NotNull String purpose,
        @NotNull EnumSet<ScratchBirdFormMode> modes,
        @NotNull List<String> mustFields,
        @NotNull List<String> shouldFields,
        @NotNull List<String> optionalFields,
        @NotNull String... childForms
    ) {
        FORMS_BY_ID.put(id, new ScratchBirdFormDefinition(
            id,
            name,
            scope,
            purpose,
            modes,
            mustFields,
            shouldFields,
            optionalFields,
            List.of(childForms)));
    }

    @NotNull
    private static EnumSet<ScratchBirdFormMode> modes(@NotNull ScratchBirdFormMode first, @NotNull ScratchBirdFormMode... rest) {
        EnumSet<ScratchBirdFormMode> modes = EnumSet.of(first);
        modes.addAll(List.of(rest));
        return modes;
    }

    @NotNull
    private static EnumSet<ScratchBirdFormMode> allLifecycleModes() {
        return modes(ScratchBirdFormMode.INSPECT, ScratchBirdFormMode.CREATE, ScratchBirdFormMode.ALTER, ScratchBirdFormMode.DELETE);
    }

    @NotNull
    private static EnumSet<ScratchBirdFormMode> reportModes() {
        return modes(ScratchBirdFormMode.INSPECT, ScratchBirdFormMode.REPORT, ScratchBirdFormMode.READ_ONLY);
    }

    @NotNull
    private static List<String> plus(@NotNull List<String> base, @NotNull String... values) {
        java.util.ArrayList<String> result = new java.util.ArrayList<>(base);
        result.addAll(List.of(values));
        return List.copyOf(result);
    }

    @NotNull
    private static List<String> reportMust() {
        return List.of("report form ID", "report title", "target connection and database", "effective principal", "source surface status", "refresh control", "time range/live indicator", "primary visualization", "refusal or missing-source banner");
    }

    @NotNull
    private static List<String> reportShould() {
        return List.of("drilldown source query or metric family", "label/dimension selector", "export action", "alert status", "last refresh timestamp");
    }

    @NotNull
    private static List<String> reportOptional() {
        return List.of("pinned-to-management-home toggle", "compare-to-baseline selector", "raw result payload view", "saved report preset");
    }
}
