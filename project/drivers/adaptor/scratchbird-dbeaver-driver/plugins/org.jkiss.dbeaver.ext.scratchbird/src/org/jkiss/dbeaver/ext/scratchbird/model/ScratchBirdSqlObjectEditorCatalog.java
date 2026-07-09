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

import java.util.List;

public final class ScratchBirdSqlObjectEditorCatalog {

    public record EditorDefinition(
        @NotNull String formId,
        @NotNull String title,
        @NotNull String objectFamily,
        @NotNull List<String> primaryFields,
        @NotNull List<String> sourceQueries,
        @NotNull List<String> securitySurfaces,
        @NotNull List<String> recursiveSchemaRules,
        @NotNull List<String> ddlCapabilities,
        @NotNull List<String> validationRules
    ) {
    }

    private ScratchBirdSqlObjectEditorCatalog() {
    }

    @Nullable
    public static EditorDefinition forForm(@NotNull ScratchBirdFormDefinition form, @NotNull String targetPath) {
        return switch (form.id()) {
            case "SBDV-FRM-001" -> definition(form, "Recursive Namespace Editor", "schema/branch",
                List.of("branch path", "parent UUID", "object UUID", "catalog-backed/client-only state", "allowed child object families"),
                List.of(objectResolverByPath(targetPath), "SELECT * FROM sys.catalog_readable.navigator_tree"),
                List.of("sys.security.permission_probe", "sys.catalog_readable.privileges"),
                List.of("Schemas are recursive branches, not a fixed two-level catalog.",
                    "Create/alter/drop must preserve parent-child UUID identity and visibility filtering."),
                List.of("CREATE SCHEMA", "ALTER SCHEMA", "DROP SCHEMA"),
                commonValidation());
            case "SBDV-FRM-601" -> definition(form, "Relational Table Editor", "relational table",
                List.of("table name", "recursive schema path", "columns", "primary key", "constraints", "indexes", "triggers", "filespace/storage hints"),
                tableSources(targetPath),
                securitySources(),
                schemaRules(),
                List.of("CREATE TABLE", "ALTER TABLE", "DROP TABLE", "COMMENT ON TABLE", "GRANT/REVOKE ON TABLE"),
                commonValidation());
            case "SBDV-FRM-602" -> definition(form, "Multi-Model Table Editor", "document/key-value/vector/graph/search table",
                List.of("table name", "payload model", "identity column", "native payload datatype", "model-specific indexes", "storage hints"),
                tableSources(targetPath),
                securitySources(),
                schemaRules(),
                List.of("CREATE TABLE", "ALTER TABLE SET PAYLOAD MODEL", "DROP TABLE", "CREATE VECTOR/SEARCH/DOCUMENT INDEX"),
                commonValidation());
            case "SBDV-FRM-603" -> definition(form, "Relational View Editor", "relational view",
                List.of("view name", "recursive schema path", "select body", "columns", "dependencies", "security barrier/invoker policy"),
                viewSources(targetPath),
                securitySources(),
                schemaRules(),
                List.of("CREATE VIEW", "ALTER VIEW", "DROP VIEW", "COMMENT ON VIEW", "GRANT/REVOKE ON VIEW"),
                commonValidation());
            case "SBDV-FRM-604" -> definition(form, "Multi-Model View Editor", "document/graph/vector/search view",
                List.of("view name", "source model", "projection body", "payload shape", "dependencies", "security policy"),
                viewSources(targetPath),
                securitySources(),
                schemaRules(),
                List.of("CREATE VIEW", "ALTER VIEW", "DROP VIEW", "model-specific projection validation"),
                commonValidation());
            case "SBDV-FRM-605" -> definition(form, "Column And Field Editor", "column/field",
                List.of("column name", "datatype/domain", "required/nullability", "default", "generated expression", "collation/charset", "field path"),
                List.of(objectResolverByPath(targetPath), "SELECT * FROM sys.catalog_readable.columns"),
                securitySources(),
                List.of("Column parent may be a table or view; recursive schema is resolved from the parent object."),
                List.of("ALTER TABLE ADD COLUMN", "ALTER TABLE ALTER COLUMN", "ALTER TABLE DROP COLUMN"),
                commonValidation());
            case "SBDV-FRM-606" -> definition(form, "Constraint Editor", "constraint",
                List.of("constraint name", "constraint kind", "columns", "check expression", "foreign target", "deferrability"),
                List.of(objectResolverByPath(targetPath), "SELECT * FROM sys.catalog_readable.constraints"),
                securitySources(),
                List.of("Constraints are owned by their parent table UUID and must not be detached from it."),
                List.of("ALTER TABLE ADD CONSTRAINT", "ALTER TABLE ALTER CONSTRAINT", "ALTER TABLE DROP CONSTRAINT"),
                commonValidation());
            case "SBDV-FRM-607" -> definition(form, "Index Editor", "index",
                List.of("index name", "index family", "key columns/expressions", "predicate", "include columns", "storage/index worker policy"),
                List.of(objectResolverByPath(targetPath), "SELECT * FROM sys.catalog_readable.indexes"),
                securitySources(),
                List.of("Indexes are child objects of a table/view path but may have asynchronous maintenance state."),
                List.of("CREATE INDEX", "ALTER INDEX", "DROP INDEX", "CREATE VECTOR/SEARCH/HYBRID INDEX"),
                commonValidation());
            case "SBDV-FRM-608" -> definition(form, "Sequence Editor", "sequence/generator",
                List.of("sequence name", "start", "increment", "min/max", "cache", "cycle", "identity bindings"),
                List.of(objectResolverByPath(targetPath), "SELECT * FROM sys.catalog_readable.sequences"),
                securitySources(),
                schemaRules(),
                List.of("CREATE SEQUENCE", "ALTER SEQUENCE", "DROP SEQUENCE"),
                commonValidation());
            case "SBDV-FRM-609" -> definition(form, "Routine Editor", "procedure/function/package routine",
                List.of("routine name", "kind", "parameters", "returns", "body", "language", "security invoker/definer", "dependencies"),
                List.of(objectResolverByPath(targetPath), "SELECT * FROM sys.catalog_readable.procedures", "SELECT * FROM sys.catalog_readable.operations"),
                securitySources(),
                schemaRules(),
                List.of("CREATE PROCEDURE", "CREATE FUNCTION", "ALTER PROCEDURE/FUNCTION", "DROP PROCEDURE/FUNCTION", "CREATE/ALTER PACKAGE"),
                commonValidation());
            case "SBDV-FRM-610" -> definition(form, "Trigger Editor", "trigger",
                List.of("trigger name", "timing", "events", "parent table/view", "condition", "body", "enabled state"),
                List.of(objectResolverByPath(targetPath), "SELECT * FROM sys.catalog_readable.triggers"),
                securitySources(),
                List.of("Table/view triggers are positioned under parent objects; database/event triggers are database scoped."),
                List.of("CREATE TRIGGER", "ALTER TRIGGER", "DROP TRIGGER"),
                commonValidation());
            case "SBDV-FRM-611" -> definition(form, "Domain And Datatype Editor", "domain/datatype",
                List.of("domain name", "base datatype", "default", "not-null", "check rules", "cluster/database scope", "casts"),
                List.of(objectResolverByPath(targetPath), "SELECT * FROM sys.catalog_readable.domains", "SELECT * FROM sys.catalog_readable.datatypes", "SELECT * FROM sys.catalog_readable.casts"),
                securitySources(),
                List.of("Domains are database/cluster scoped and are displayed through management/domains, not as ordinary schema-only children."),
                List.of("CREATE DOMAIN", "ALTER DOMAIN", "DROP DOMAIN", "CREATE CAST"),
                commonValidation());
            case "SBDV-FRM-612" -> definition(form, "Payload Model Editor", "payload model",
                List.of("payload model", "shape version", "identity mapping", "validation rules", "index families"),
                List.of(objectResolverByPath(targetPath), "SELECT * FROM sys.catalog_readable.resources"),
                securitySources(),
                schemaRules(),
                List.of("ALTER TABLE SET PAYLOAD MODEL", "ALTER TABLE DROP PAYLOAD MODEL"),
                commonValidation());
            case "SBDV-FRM-110" -> definition(form, "Security Grant And Ownership Editor", "security DDL",
                List.of("principal", "role/group membership", "direct grants", "ownership", "policy", "effective visibility"),
                List.of("SELECT * FROM sys.catalog_readable.privileges", "SELECT * FROM sys.security.permission_probe"),
                List.of("sys.security.roles", "sys.security.principals", "sys.security.policies", "sys.security.permission_probe"),
                List.of("Security objects are filtered by visibility and may be rooted outside ordinary schemas."),
                List.of("GRANT", "REVOKE", "ALTER OWNER", "CREATE/ALTER/DROP ROLE", "CREATE/ALTER/DROP USER/GROUP"),
                commonValidation());
            default -> form.supportsMode(ScratchBirdFormMode.CREATE) || form.supportsMode(ScratchBirdFormMode.ALTER) ||
                form.supportsMode(ScratchBirdFormMode.DELETE)
                ? definition(form, form.name(), "ScratchBird lifecycle object",
                    form.mustFields(),
                    List.of(objectResolverByPath(targetPath)),
                    securitySources(),
                    schemaRules(),
                    List.of("CREATE", "ALTER", "DROP"),
                    commonValidation())
                : null;
        };
    }

    @NotNull
    private static EditorDefinition definition(
        @NotNull ScratchBirdFormDefinition form,
        @NotNull String title,
        @NotNull String objectFamily,
        @NotNull List<String> primaryFields,
        @NotNull List<String> sourceQueries,
        @NotNull List<String> securitySurfaces,
        @NotNull List<String> recursiveSchemaRules,
        @NotNull List<String> ddlCapabilities,
        @NotNull List<String> validationRules
    ) {
        return new EditorDefinition(
            form.id(),
            title,
            objectFamily,
            List.copyOf(primaryFields),
            List.copyOf(sourceQueries),
            List.copyOf(securitySurfaces),
            List.copyOf(recursiveSchemaRules),
            List.copyOf(ddlCapabilities),
            List.copyOf(validationRules));
    }

    @NotNull
    private static List<String> tableSources(@NotNull String targetPath) {
        return List.of(objectResolverByPath(targetPath), "SELECT * FROM sys.catalog_readable.tables", "SELECT * FROM sys.catalog_readable.columns");
    }

    @NotNull
    private static List<String> viewSources(@NotNull String targetPath) {
        return List.of(objectResolverByPath(targetPath), "SELECT * FROM sys.catalog_readable.views", "SELECT * FROM sys.catalog_readable.columns");
    }

    @NotNull
    private static List<String> securitySources() {
        return List.of("SELECT * FROM sys.catalog_readable.privileges", "SELECT * FROM sys.security.permission_probe");
    }

    @NotNull
    private static List<String> schemaRules() {
        return List.of(
            "Resolve object names through ScratchBird recursive schema paths, not fixed catalog.schema assumptions.",
            "Display names are user-language/session preferences; UUID identity remains server authoritative.",
            "Hidden parents or children are not synthesized by DBeaver.");
    }

    @NotNull
    private static List<String> commonValidation() {
        return List.of(
            "Local parser/lint is advisory only.",
            "Create, alter, and drop require a live server permission probe for the exact draft SQL hash.",
            "Server must lower SBsql to SBLR/UUID outside the engine and revalidate authority, role, group, visibility, policy epoch, and MGA transaction scope.",
            "Mutation apply must use the ScratchBird JDBC/SBsql route and report server refusal without client-side substitution.");
    }

    @NotNull
    private static String objectResolverByPath(@NotNull String targetPath) {
        return "SELECT object_id, object_type, schema_path, full_path, object_name FROM sys.catalog.object_resolver WHERE full_path = '" +
            targetPath.replace("'", "''") + "'";
    }
}
