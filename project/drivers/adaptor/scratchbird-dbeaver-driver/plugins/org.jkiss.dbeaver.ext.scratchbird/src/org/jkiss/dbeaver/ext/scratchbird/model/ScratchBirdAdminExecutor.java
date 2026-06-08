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

public final class ScratchBirdAdminExecutor {

    public record ExecutionPlan(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdFormMode mode,
        @NotNull String targetPath,
        @NotNull String commandText,
        boolean executable,
        boolean destructive,
        @NotNull String authority
    ) {
    }

    private ScratchBirdAdminExecutor() {
    }

    @NotNull
    public static String previewCommand(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdFormMode mode,
        @NotNull String targetPath
    ) {
        return plan(form, mode, targetPath).commandText();
    }

    @NotNull
    public static ExecutionPlan plan(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdFormMode mode,
        @NotNull String targetPath
    ) {
        String authority = authorityFor(form, targetPath);
        return switch (mode) {
            case CREATE -> new ExecutionPlan(
                form,
                mode,
                targetPath,
                createCommand(form, targetPath),
                true,
                false,
                authority);
            case ALTER -> new ExecutionPlan(
                form,
                mode,
                targetPath,
                alterCommand(form, targetPath),
                true,
                false,
                authority);
            case DELETE -> new ExecutionPlan(
                form,
                mode,
                targetPath,
                dropCommand(form, targetPath),
                "SBDV-FRM-014".equals(form.id()),
                true,
                authority + "; destructive actions require explicit SBDV-FRM-014 confirmation");
            case TASK -> new ExecutionPlan(
                form,
                mode,
                targetPath,
                taskCommand(targetPath),
                !ScratchBirdTaskCatalog.tasksFor(targetPath).isEmpty(),
                false,
                authority + "; task previews expose capability probe, validate-only, execute-now, and schedule guidance from the ScratchBird task catalog");
            case REPORT -> new ExecutionPlan(
                form,
                mode,
                targetPath,
                reportCommand(targetPath),
                true,
                false,
                "report execution uses report catalog sources and server-side SELECT/SHOW permissions");
            case INSPECT, READ_ONLY -> new ExecutionPlan(
                form,
                mode,
                targetPath,
                inspectCommand(form, targetPath),
                true,
                false,
                "read-only catalog inspection");
        };
    }

    @NotNull
    private static String createCommand(@NotNull ScratchBirdFormDefinition form, @NotNull String targetPath) {
        String branchCommand = branchCreateCommand(form, targetPath);
        if (branchCommand != null) {
            return branchCommand;
        }
        if ("SBDV-FRM-611".equals(form.id())) {
            return "CREATE DOMAIN " + qualify(targetPath, "new_domain") + " AS TEXT";
        }
        if (form.id().startsWith("SBDV-FRM-60")) {
            return switch (form.id()) {
                case "SBDV-FRM-601" -> "CREATE TABLE " + qualify(targetPath, "new_table") + " (id UUID PRIMARY KEY)";
                case "SBDV-FRM-602" -> "CREATE TABLE " + qualify(targetPath, "new_document") + " (id UUID PRIMARY KEY, payload JSONB)";
                case "SBDV-FRM-603" -> "CREATE VIEW " + qualify(targetPath, "new_view") + " AS SELECT 1 AS value";
                case "SBDV-FRM-604" -> "CREATE VIEW " + qualify(targetPath, "new_payload_view") + " AS SELECT payload FROM " + qualify(targetPath, "source_table");
                case "SBDV-FRM-605" -> "ALTER TABLE " + quotePath(targetPath) + " ADD COLUMN new_field TEXT";
                case "SBDV-FRM-606" -> "ALTER TABLE " + quotePath(targetPath) + " ADD CONSTRAINT new_constraint CHECK (1 = 1)";
                case "SBDV-FRM-607" -> "CREATE INDEX " + quoteIdentifier("idx_" + leafName(targetPath)) + " ON " + quotePath(targetPath) + " (id)";
                case "SBDV-FRM-608" -> "CREATE SEQUENCE " + qualify(targetPath, "new_sequence");
                case "SBDV-FRM-609" -> "CREATE PROCEDURE " + qualify(targetPath, "new_procedure") + "() AS BEGIN RETURN; END";
                case "SBDV-FRM-610" -> "CREATE TRIGGER " + qualify(targetPath, "new_trigger") + " BEFORE INSERT ON " + quotePath(targetPath) + " AS BEGIN RETURN; END";
                case "SBDV-FRM-612" -> "ALTER TABLE " + quotePath(targetPath) + " SET PAYLOAD MODEL JSONB";
                default -> "DESCRIBE " + formObjectType(form) + " " + quotePath(targetPath);
            };
        }
        return "CREATE SCHEMA " + quotePath(qualify(targetPath, "new_branch"));
    }

    @NotNull
    private static String alterCommand(@NotNull ScratchBirdFormDefinition form, @NotNull String targetPath) {
        String branchCommand = branchAlterCommand(form, targetPath);
        if (branchCommand != null) {
            return branchCommand;
        }
        if ("SBDV-FRM-611".equals(form.id())) {
            return "ALTER DOMAIN " + quotePath(targetPath) + " SET DEFAULT NULL";
        }
        if (form.id().startsWith("SBDV-FRM-60")) {
            return switch (form.id()) {
                case "SBDV-FRM-601", "SBDV-FRM-602" -> "ALTER TABLE " + quotePath(targetPath) + " ADD COLUMN new_column TEXT";
                case "SBDV-FRM-603", "SBDV-FRM-604" -> "ALTER VIEW " + quotePath(targetPath) + " RENAME TO " + quoteIdentifier(leafName(targetPath) + "_renamed");
                case "SBDV-FRM-605" -> "ALTER TABLE " + quotePath(parentPath(targetPath)) + " ALTER COLUMN " + quoteIdentifier(leafName(targetPath)) + " SET DEFAULT NULL";
                case "SBDV-FRM-606" -> "ALTER TABLE " + quotePath(parentPath(targetPath)) + " ALTER CONSTRAINT " + quoteIdentifier(leafName(targetPath)) + " NOT DEFERRABLE";
                case "SBDV-FRM-607" -> "ALTER INDEX " + quotePath(targetPath) + " RENAME TO " + quoteIdentifier(leafName(targetPath) + "_renamed");
                case "SBDV-FRM-608" -> "ALTER SEQUENCE " + quotePath(targetPath) + " RESTART WITH 1";
                case "SBDV-FRM-609" -> "CREATE PROCEDURE " + quotePath(targetPath) + "() AS BEGIN RETURN; END";
                case "SBDV-FRM-610" -> "ALTER TRIGGER " + quotePath(targetPath) + " ACTIVE";
                case "SBDV-FRM-612" -> "ALTER TABLE " + quotePath(parentPath(targetPath)) + " SET PAYLOAD MODEL JSONB";
                default -> "DESCRIBE " + formObjectType(form) + " " + quotePath(targetPath);
            };
        }
        return "ALTER SCHEMA " + quotePath(targetPath) + " RENAME TO " + quoteIdentifier(leafName(targetPath) + "_renamed");
    }

    @NotNull
    private static String dropCommand(@NotNull ScratchBirdFormDefinition form, @NotNull String targetPath) {
        String branchCommand = branchDropCommand(form, targetPath);
        if (branchCommand != null) {
            return branchCommand;
        }
        if ("SBDV-FRM-611".equals(form.id())) {
            return "DROP DOMAIN " + quotePath(targetPath);
        }
        if (form.id().startsWith("SBDV-FRM-60")) {
            return switch (form.id()) {
                case "SBDV-FRM-601", "SBDV-FRM-602" -> "DROP TABLE " + quotePath(targetPath);
                case "SBDV-FRM-603", "SBDV-FRM-604" -> "DROP VIEW " + quotePath(targetPath);
                case "SBDV-FRM-605" -> "ALTER TABLE " + quotePath(parentPath(targetPath)) + " DROP COLUMN " + quoteIdentifier(leafName(targetPath));
                case "SBDV-FRM-606" -> "ALTER TABLE " + quotePath(parentPath(targetPath)) + " DROP CONSTRAINT " + quoteIdentifier(leafName(targetPath));
                case "SBDV-FRM-607" -> "DROP INDEX " + quotePath(targetPath);
                case "SBDV-FRM-608" -> "DROP SEQUENCE " + quotePath(targetPath);
                case "SBDV-FRM-609" -> "DROP PROCEDURE " + quotePath(targetPath);
                case "SBDV-FRM-610" -> "DROP TRIGGER " + quotePath(targetPath);
                default -> "DESCRIBE " + formObjectType(form) + " " + quotePath(targetPath);
            };
        }
        return "DROP SCHEMA " + quotePath(targetPath);
    }

    @NotNull
    private static String reportCommand(@NotNull String targetPath) {
        ScratchBirdReportDefinition report = ScratchBirdReportCatalog.findByNavigatorPath(targetPath);
        if (report == null) {
            return "SHOW METRICS";
        }
        return ScratchBirdReportPlan.forReport(report).scriptPreview();
    }

    @NotNull
    private static String taskCommand(@NotNull String targetPath) {
        List<ScratchBirdTaskDefinition> tasks = ScratchBirdTaskCatalog.tasksFor(targetPath);
        if (tasks.isEmpty()) {
            return ScratchBirdPermissionProbe.serverAuthorizationScript(targetPath);
        }
        return tasks.get(0).primaryCommand();
    }

    @NotNull
    private static String inspectCommand(@NotNull ScratchBirdFormDefinition form, @NotNull String targetPath) {
        String branchCommand = branchInspectCommand(form, targetPath);
        if (branchCommand != null) {
            return branchCommand;
        }
        return "SELECT object_id, object_type, schema_path, full_path, object_name " +
            "FROM sys.catalog.object_resolver WHERE full_path = " + literal(targetPath);
    }

    @NotNull
    private static String authorityFor(@NotNull ScratchBirdFormDefinition form, @NotNull String targetPath) {
        ScratchBirdBranchProfile profile = ScratchBirdBranchProfile.forPath(targetPath);
        return "server-authoritative v3 SQL/admin execution; client preview only until executed; branch profile " +
            profile.id() + " routes via " + form.id();
    }

    private static String branchCreateCommand(@NotNull ScratchBirdFormDefinition form, @NotNull String targetPath) {
        return switch (form.id()) {
            case "SBDV-FRM-102" -> createNamedObject("JOB", suggestedName(targetPath, "new_job")) + " AS SELECT 1";
            case "SBDV-FRM-103" -> createNamedObject("ROLE", suggestedName(targetPath, "new_role"));
            case "SBDV-FRM-200", "SBDV-FRM-300" -> createNamedObject("USER", suggestedName(targetPath, "new_user"));
            case "SBDV-FRM-201", "SBDV-FRM-301" -> createNamedObject("GROUP", suggestedName(targetPath, "new_group"));
            case "SBDV-FRM-302", "SBDV-FRM-303", "SBDV-FRM-304", "SBDV-FRM-305" ->
                createNamedObject("CLUSTER", suggestedName(targetPath, "new_cluster_member"));
            case "SBDV-FRM-400", "SBDV-FRM-401", "SBDV-FRM-502" ->
                createNamedObject("SERVER", suggestedName(targetPath, "new_server"));
            case "SBDV-FRM-500", "SBDV-FRM-203" ->
                createNamedObject("DATABASE CONNECTION", suggestedName(targetPath, "new_connection"));
            case "SBDV-FRM-501" -> createNamedObject("FOREIGN DATA WRAPPER", suggestedName(targetPath, "new_fdw"));
            case "SBDV-FRM-503", "SBDV-FRM-206", "SBDV-FRM-207" ->
                createNamedObject("POLICY", suggestedName(targetPath, "new_policy"));
            default -> null;
        };
    }

    private static String branchAlterCommand(@NotNull ScratchBirdFormDefinition form, @NotNull String targetPath) {
        return switch (form.id()) {
            case "SBDV-FRM-101" -> "ALTER SYSTEM SET sample_setting = 'value'";
            case "SBDV-FRM-102" -> renameNamedObject("JOB", targetPath);
            case "SBDV-FRM-103" -> renameNamedObject("ROLE", targetPath);
            case "SBDV-FRM-104", "SBDV-FRM-302", "SBDV-FRM-303", "SBDV-FRM-304", "SBDV-FRM-305" ->
                renameNamedObject("CLUSTER", targetPath);
            case "SBDV-FRM-105", "SBDV-FRM-400", "SBDV-FRM-401", "SBDV-FRM-502" ->
                renameNamedObject("SERVER", targetPath);
            case "SBDV-FRM-108" -> "ALTER SCHEMA " + quotePath(targetPath) + " RENAME TO " + quoteIdentifier(leafName(targetPath) + "_policy");
            case "SBDV-FRM-200", "SBDV-FRM-300" -> renameNamedObject("USER", targetPath);
            case "SBDV-FRM-201", "SBDV-FRM-301" -> renameNamedObject("GROUP", targetPath);
            case "SBDV-FRM-202" -> renameNamedObject("USER", parentLeafName(targetPath));
            case "SBDV-FRM-203", "SBDV-FRM-500" -> renameNamedObject("DATABASE CONNECTION", targetPath);
            case "SBDV-FRM-205" -> "ALTER USER MAPPING " + quoteIdentifier(suggestedName(targetPath, "current_mapping"));
            case "SBDV-FRM-206", "SBDV-FRM-207", "SBDV-FRM-503" -> renameNamedObject("POLICY", targetPath);
            default -> null;
        };
    }

    private static String branchDropCommand(@NotNull ScratchBirdFormDefinition form, @NotNull String targetPath) {
        return switch (form.id()) {
            case "SBDV-FRM-102" -> dropNamedObject("JOB", targetPath);
            case "SBDV-FRM-103" -> dropNamedObject("ROLE", targetPath);
            case "SBDV-FRM-200", "SBDV-FRM-300" -> dropNamedObject("USER", targetPath);
            case "SBDV-FRM-201", "SBDV-FRM-301" -> dropNamedObject("GROUP", targetPath);
            case "SBDV-FRM-104", "SBDV-FRM-302", "SBDV-FRM-303", "SBDV-FRM-304", "SBDV-FRM-305" ->
                dropNamedObject("CLUSTER", targetPath);
            case "SBDV-FRM-105", "SBDV-FRM-400", "SBDV-FRM-401", "SBDV-FRM-502" ->
                dropNamedObject("SERVER", targetPath);
            case "SBDV-FRM-203", "SBDV-FRM-500" -> dropNamedObject("DATABASE CONNECTION", targetPath);
            case "SBDV-FRM-205" -> "DROP USER MAPPING " + quoteIdentifier(suggestedName(targetPath, "current_mapping"));
            case "SBDV-FRM-206", "SBDV-FRM-207", "SBDV-FRM-503" -> dropNamedObject("POLICY", targetPath);
            default -> null;
        };
    }

    private static String branchInspectCommand(@NotNull ScratchBirdFormDefinition form, @NotNull String targetPath) {
        return switch (form.id()) {
            case "SBDV-FRM-101" -> String.join("\n",
                "SHOW MANAGEMENT MANAGER;",
                "SHOW MANAGEMENT DRIFT;");
            case "SBDV-FRM-102" -> String.join("\n",
                "SHOW JOBS;",
                "SELECT * FROM sys.jobs;",
                "SELECT * FROM sys.job_runs ORDER BY started_at DESC LIMIT 50;");
            case "SBDV-FRM-103" -> String.join("\n",
                "SHOW ROLES;",
                "SHOW GRANTS;");
            case "SBDV-FRM-104" -> String.join("\n",
                "SHOW CLUSTER ADMISSION STATUS;",
                "SHOW CLUSTER ROUTING PLAN;");
            case "SBDV-FRM-105" -> String.join("\n",
                "SHOW MANAGEMENT SERVERS;",
                "SHOW MANAGEMENT INSTRUCTIONS;");
            case "SBDV-FRM-106" -> String.join("\n",
                "SHOW MANAGEMENT LISTENERS;",
                "SHOW MANAGEMENT PARSER POOL FOR LISTENER 'scratchbird';");
            case "SBDV-FRM-107" -> String.join("\n",
                "SHOW SCHEMA PATH;",
                "SELECT object_id, object_type, schema_path, full_path, object_name " +
                    "FROM sys.catalog.object_resolver WHERE schema_path = " + literal(targetPath) + ";");
            case "SBDV-FRM-108" -> String.join("\n",
                "SHOW SCHEMA PATH;",
                "SELECT object_id, object_type, schema_path, full_path, object_name " +
                    "FROM sys.catalog.object_resolver WHERE full_path = " + literal(targetPath) + ";");
            case "SBDV-FRM-109" -> String.join("\n",
                "SHOW SCHEMA PATH;",
                "SELECT object_id, object_type, schema_path, full_path, object_name " +
                    "FROM sys.catalog.object_resolver WHERE full_path = " + literal(targetPath) + ";");
            case "SBDV-FRM-200", "SBDV-FRM-201", "SBDV-FRM-300", "SBDV-FRM-301" -> String.join("\n",
                "SHOW ROLES;",
                "SHOW GRANTS;",
                "SELECT object_id, object_type, schema_path, full_path, object_name " +
                    "FROM sys.catalog.object_resolver WHERE full_path = " + literal(targetPath) + ";");
            case "SBDV-FRM-202", "SBDV-FRM-203", "SBDV-FRM-204", "SBDV-FRM-205", "SBDV-FRM-206", "SBDV-FRM-207" ->
                String.join("\n",
                    "SHOW SCHEMA PATH;",
                    "SELECT object_id, object_type, schema_path, full_path, object_name " +
                        "FROM sys.catalog.object_resolver WHERE full_path = " + literal(targetPath) + ";");
            case "SBDV-FRM-400", "SBDV-FRM-401", "SBDV-FRM-402", "SBDV-FRM-500", "SBDV-FRM-501",
                "SBDV-FRM-502", "SBDV-FRM-503" -> String.join("\n",
                "SHOW MANAGEMENT SERVERS;",
                "SHOW MANAGEMENT INSTRUCTIONS;",
                "SELECT object_id, object_type, schema_path, full_path, object_name " +
                    "FROM sys.catalog.object_resolver WHERE full_path = " + literal(targetPath) + ";");
            default -> null;
        };
    }

    @NotNull
    private static String qualify(@NotNull String path, @NotNull String childName) {
        return quotePath(path + "." + childName);
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
    private static String createNamedObject(@NotNull String objectSurface, @NotNull String name) {
        return "CREATE " + objectSurface + " " + quoteIdentifier(name);
    }

    @NotNull
    private static String renameNamedObject(@NotNull String objectSurface, @NotNull String targetPath) {
        String name = targetNameForObject(targetPath);
        return "ALTER " + objectSurface + " " + quoteIdentifier(name) +
            " RENAME TO " + quoteIdentifier(name + "_renamed");
    }

    @NotNull
    private static String dropNamedObject(@NotNull String objectSurface, @NotNull String targetPath) {
        return "DROP " + objectSurface + " " + quoteIdentifier(targetNameForObject(targetPath));
    }

    @NotNull
    private static String targetNameForObject(@NotNull String targetPath) {
        return suggestedName(targetPath, leafName(targetPath));
    }

    @NotNull
    private static String suggestedName(@NotNull String targetPath, @NotNull String fallback) {
        String leaf = leafName(targetPath);
        if (leaf.isBlank() || leaf.equals("sys") || leaf.equals("users") || leaf.equals("cluster") ||
            leaf.equals("emulated") || leaf.equals("remote") || leaf.equals("data")) {
            return fallback;
        }
        return leaf.replace('-', '_');
    }

    @NotNull
    private static String leafName(@NotNull String fullPath) {
        int separator = fullPath.lastIndexOf('.');
        return separator < 0 ? fullPath : fullPath.substring(separator + 1);
    }

    @NotNull
    private static String parentPath(@NotNull String fullPath) {
        int separator = fullPath.lastIndexOf('.');
        return separator < 0 ? fullPath : fullPath.substring(0, separator);
    }

    @NotNull
    private static String parentLeafName(@NotNull String fullPath) {
        return leafName(parentPath(fullPath));
    }

    @NotNull
    private static String formObjectType(@NotNull ScratchBirdFormDefinition form) {
        return switch (form.id()) {
            case "SBDV-FRM-605" -> "COLUMN";
            case "SBDV-FRM-606" -> "CHECK";
            case "SBDV-FRM-607" -> "INDEX";
            case "SBDV-FRM-608" -> "SEQUENCE";
            case "SBDV-FRM-609" -> "PROCEDURE";
            case "SBDV-FRM-610" -> "TRIGGER";
            case "SBDV-FRM-611" -> "DOMAIN";
            default -> "TABLE";
        };
    }
}
