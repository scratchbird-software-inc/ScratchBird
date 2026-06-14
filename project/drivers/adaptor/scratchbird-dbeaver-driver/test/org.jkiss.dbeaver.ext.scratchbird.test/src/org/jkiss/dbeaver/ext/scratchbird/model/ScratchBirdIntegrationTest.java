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

import org.eclipse.core.runtime.Platform;
import org.jkiss.dbeaver.model.DBPDataKind;
import org.jkiss.dbeaver.model.DBPKeywordType;
import org.jkiss.dbeaver.model.connection.DBPConnectionConfiguration;
import org.jkiss.dbeaver.model.connection.DBPDriver;
import org.jkiss.dbeaver.model.connection.DBPDriverConfigurationType;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3Lexer;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3ParseResult;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3Parser;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3StatementFamily;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3StatementKind;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3Statement;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3TokenType;
import org.junit.Assert;
import org.junit.Test;
import org.osgi.framework.Bundle;

import java.io.IOException;
import java.io.InputStream;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.Collection;
import java.util.List;

public class ScratchBirdIntegrationTest {

    @Test
    public void pluginXmlDeclaresRecursiveMetadataTreeAndTableStructureFolders() throws IOException {
        String pluginXml = readHostPluginXml();
        Bundle bundle = getHostBundle();

        Assert.assertTrue(pluginXml.contains("dialect=\"scratchbird\""));
        Assert.assertTrue(pluginXml.contains("<extension point=\"org.jkiss.dbeaver.sqlDialect\">"));
        Assert.assertTrue(pluginXml.contains("id=\"scratchbird\""));
        Assert.assertTrue(pluginXml.contains("class=\"org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdSQLDialect\""));

        Assert.assertTrue(pluginXml.contains("property=\"schemaTree\""));
        Assert.assertTrue(pluginXml.contains("property=\"childSchemas\""));
        Assert.assertTrue(pluginXml.contains("type=\"org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdSchemaNode\""));
        Assert.assertTrue(pluginXml.contains("label=\"%tree.schemas.node.name\""));
        Assert.assertTrue(pluginXml.contains("visibleIf=\"object.schemaBranchesFolderVisible\""));
        Assert.assertTrue(pluginXml.contains("recursive=\"../..\""));
        Assert.assertTrue(pluginXml.contains("sb_namespace.svg"));
        Assert.assertTrue(pluginXml.contains("sb_system.svg"));
        Assert.assertTrue(pluginXml.contains("sb_domains.svg"));
        Assert.assertTrue(pluginXml.contains("sb_metrics.svg"));

        Assert.assertTrue(pluginXml.contains("type=\"org.jkiss.dbeaver.ext.generic.model.GenericUniqueKey\""));
        Assert.assertTrue(pluginXml.contains("type=\"org.jkiss.dbeaver.ext.generic.model.GenericTableForeignKey\""));
        Assert.assertTrue(pluginXml.contains("type=\"org.jkiss.dbeaver.ext.generic.model.GenericTableIndex\""));
        Assert.assertTrue(pluginXml.contains("label=\"%tree.uni_keys.node.name\""));
        Assert.assertTrue(pluginXml.contains("label=\"%tree.foreign_keys.node.name\""));
        Assert.assertTrue(pluginXml.contains("label=\"%tree.references.node.name\""));
        Assert.assertTrue(pluginXml.contains("path=\"table\" property=\"physicalTables\""));
        Assert.assertTrue(pluginXml.contains("path=\"view\" property=\"views\""));
        Assert.assertTrue(pluginXml.contains("property=\"constraints\""));
        Assert.assertTrue(pluginXml.contains("property=\"associations\""));
        Assert.assertTrue(pluginXml.contains("property=\"indexes\""));
        Assert.assertTrue(pluginXml.contains("property=\"sequences\""));
        Assert.assertTrue(pluginXml.contains("property=\"references\""));
        Assert.assertTrue(pluginXml.contains("property=\"dataTypes\""));
        Assert.assertTrue(pluginXml.contains("visibleIf=\"object.tableFoldersVisible\""));
        Assert.assertTrue(pluginXml.contains("visibleIf=\"object.dataTypesFolderVisible\""));
        Assert.assertTrue(pluginXml.contains("visibleIf=\"object.sequenceFoldersVisible"));
        Assert.assertTrue(pluginXml.contains("<extension point=\"org.jkiss.dbeaver.objectManager\">"));
        Assert.assertTrue(pluginXml.contains("ScratchBirdSchemaNodeManager"));
        Assert.assertTrue(pluginXml.contains("ScratchBirdTableManager"));
        Assert.assertTrue(pluginXml.contains("ScratchBirdViewManager"));
        Assert.assertTrue(pluginXml.contains("ScratchBirdTableColumnManager"));
        Assert.assertTrue(pluginXml.contains("ScratchBirdConstraintManager"));
        Assert.assertTrue(pluginXml.contains("ScratchBirdForeignKeyManager"));
        Assert.assertTrue(pluginXml.contains("ScratchBirdIndexManager"));
        Assert.assertTrue(pluginXml.contains("ScratchBirdSequenceManager"));
        Assert.assertTrue(pluginXml.contains("ScratchBirdProcedureManager"));
        Assert.assertTrue(pluginXml.contains("ScratchBirdTableTriggerManager"));
        Assert.assertTrue(pluginXml.contains("ScratchBirdDomainManager"));
        Assert.assertTrue(pluginXml.contains("objectType=\"org.jkiss.dbeaver.ext.generic.model.GenericUniqueKey\""));
        Assert.assertTrue(pluginXml.contains("objectType=\"org.jkiss.dbeaver.ext.generic.model.GenericTableForeignKey\""));
        Assert.assertTrue(pluginXml.contains("objectType=\"org.jkiss.dbeaver.ext.generic.model.GenericTableIndex\""));
        Assert.assertTrue(pluginXml.contains("objectType=\"org.jkiss.dbeaver.ext.generic.model.GenericDataType\""));
        Assert.assertTrue(pluginXml.contains("<extension point=\"org.jkiss.dbeaver.dataTypeProvider\">"));
        Assert.assertTrue(pluginXml.contains("ScratchBirdValueHandlerProvider"));
        Assert.assertFalse(pluginXml.contains("<extension point=\"org.jkiss.dbeaver.dashboard\">"));
        Assert.assertFalse(pluginXml.contains("scratchbird.sessions"));
        Assert.assertFalse(pluginXml.contains("scratchbird.performance"));
        Assert.assertFalse(pluginXml.contains("<query>SHOW METRICS</query>"));
        Assert.assertFalse(pluginXml.contains("sys.performance"));
        Assert.assertTrue(pluginXml.contains("<folder type=\"org.jkiss.dbeaver.ext.generic.model.GenericTable\""));
        Assert.assertTrue(pluginXml.contains("<folder type=\"org.jkiss.dbeaver.ext.generic.model.GenericView\""));

        Assert.assertTrue(pluginXml.contains("id=\"scratchbird_jdbc\""));
        Assert.assertTrue(pluginXml.contains("class=\"com.scratchbird.jdbc.SBDriver\""));
        Assert.assertTrue(pluginXml.contains("sampleURL=\"jdbc:scratchbird://{host}[:{port}]/{database}?sslmode=require&amp;binary_transfer=true&amp;metadata_fixture_catalog=driver_test\"")
            || pluginXml.contains("sampleURL=\"jdbc:scratchbird://{host}[:{port}]/{database}?sslmode=require&binary_transfer=true&metadata_fixture_catalog=driver_test\""));
        Assert.assertTrue(pluginXml.contains("defaultPort=\"3092\""));
        Assert.assertTrue(pluginXml.contains("path=\"drivers/scratchbird\""));
        Assert.assertTrue(pluginXml.contains("name=\"driver-properties\""));
        Assert.assertTrue(pluginXml.contains("value=\"connect_timeout,socket_timeout,binary_transfer"));
        Assert.assertTrue(pluginXml.contains("id=\"protocol\""));
        Assert.assertTrue(pluginXml.contains("id=\"schema\""));
        Assert.assertTrue(pluginXml.contains("id=\"search_path\""));
        Assert.assertTrue(pluginXml.contains("id=\"connect_timeout\""));
        Assert.assertTrue(pluginXml.contains("id=\"socket_timeout\""));
        Assert.assertTrue(pluginXml.contains("id=\"binary_transfer\""));
        Assert.assertTrue(pluginXml.contains("id=\"compression\""));
        Assert.assertTrue(pluginXml.contains("id=\"pooling\""));
        Assert.assertTrue(pluginXml.contains("id=\"rewrite_batched_inserts\""));
        Assert.assertTrue(pluginXml.contains("id=\"loggerLevel\""));
        Assert.assertTrue(pluginXml.contains("name=\"supports-references\""));
        Assert.assertTrue(pluginXml.contains("name=\"supports-table-constraints\""));
        Assert.assertTrue(pluginXml.contains("id=\"front_door_mode\""));
        Assert.assertTrue(pluginXml.contains("id=\"manager_auth_token\""));
        Assert.assertTrue(pluginXml.contains("id=\"auth_token\""));
        Assert.assertTrue(pluginXml.contains("id=\"auth_method_id\""));
        Assert.assertTrue(pluginXml.contains("id=\"workload_identity_token\""));
        Assert.assertTrue(pluginXml.contains("id=\"proxy_principal_assertion\""));
        Assert.assertTrue(pluginXml.contains("id=\"dormant_id\""));
        Assert.assertTrue(pluginXml.contains("id=\"dormant_reattach_token\""));
        Assert.assertTrue(pluginXml.contains("id=\"metadataExpandSchemaParents\""));
        Assert.assertTrue(pluginXml.contains("id=\"metadata_fixture_catalog\""));
        Assert.assertTrue(pluginXml.contains("defaultValue=\"driver_test\""));
        Assert.assertTrue(pluginXml.contains("execKeywords\" value=\"CALL,ANALYZE,BACKUP,CHECKPOINT,CLUSTER"));
        Assert.assertFalse(pluginXml.contains("COPY,DESC"));
        Assert.assertFalse(pluginXml.contains("DESCRIBE,DO"));
        Assert.assertFalse(pluginXml.contains("DO,EXEC"));
        Assert.assertTrue(pluginXml.contains("keywords\" value=\"ALTER,ANALYZE,AND,AS,BACKUP"));
        Assert.assertTrue(pluginXml.contains("STATUS"));
        Assert.assertTrue(pluginXml.contains("METRICS"));
        Assert.assertFalse(pluginXml.contains("PROCESSLIST"));
        Assert.assertFalse(pluginXml.contains("VARIABLES"));
        Assert.assertFalse(pluginXml.contains("WARNINGS"));

        Assert.assertTrue(pluginXml.contains("TIMESTAMPTZ"));
        Assert.assertTrue(pluginXml.contains("UUID"));
        Assert.assertTrue(pluginXml.contains("JSONB"));
        Assert.assertTrue(pluginXml.contains("JSONPATH"));
        Assert.assertTrue(pluginXml.contains("VECTOR"));
        Assert.assertTrue(pluginXml.contains("RANGE"));
        Assert.assertTrue(pluginXml.contains("MACADDR8"));
        Assert.assertTrue(pluginXml.contains("HASH256"));
        Assert.assertTrue(pluginXml.contains("property=\"triggers\""));
        Assert.assertTrue(pluginXml.contains("property=\"proceduresOnly\""));
        Assert.assertTrue(pluginXml.contains("property=\"functionsOnly\""));
        Assert.assertTrue(pluginXml.contains("property=\"tableTriggers\""));
        if (bundle != null) {
            Assert.assertNotNull(bundle.getEntry("drivers/scratchbird/scratchbird-jdbc.jar"));
        }
    }

    @Test
    public void scratchBirdMetaModelUsesScratchBirdDialect() throws Exception {
        String source = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdMetaModel.java");
        Assert.assertTrue(source.contains("new ScratchBirdSQLDialect()"));
        Assert.assertTrue(source.contains("return new ScratchBirdSchema("));
        Assert.assertTrue(source.contains("return new ScratchBirdView("));
        Assert.assertTrue(source.contains("return new ScratchBirdTable("));
    }

    @Test
    public void scratchBirdQualifiedNamesSplitSchemaPathsBySegment() {
        Assert.assertEquals(
            "users.public.compat_identity_user_map_contract",
            ScratchBirdQualifiedNames.qualifyPath("users.public", "compat_identity_user_map_contract"));
        Assert.assertEquals(
            "sys.catalog.object_resolver",
            ScratchBirdQualifiedNames.qualifyPath("sys.catalog", "object_resolver"));
        Assert.assertEquals(
            "users.home.\"Jane Doe\".profiles",
            ScratchBirdQualifiedNames.qualifyPath("users.home.Jane Doe", "profiles"));
        Assert.assertNotEquals(
            "\"users.public\".compat_identity_user_map_contract",
            ScratchBirdQualifiedNames.qualifyPath("users.public", "compat_identity_user_map_contract"));
    }

    @Test
    public void scratchBirdPluginShipsLocalizationBundleAndSchemaNodeDelegatesMetadataLookups() throws Exception {
        Bundle bundle = getHostBundle();
        if (bundle != null) {
            Assert.assertNotNull(bundle.getEntry("OSGI-INF/l10n/bundle.properties"));
        }

        Path bundlePath = Path.of(System.getProperty("user.dir"))
            .resolve("../../plugins/org.jkiss.dbeaver.ext.scratchbird/OSGI-INF/l10n/bundle.properties")
            .normalize();
        Assert.assertTrue("ScratchBird localization bundle not found: " + bundlePath, Files.exists(bundlePath));

        String manifest = Files.readString(
            Path.of(System.getProperty("user.dir"))
                .resolve("../../plugins/org.jkiss.dbeaver.ext.scratchbird/META-INF/MANIFEST.MF")
                .normalize(),
            StandardCharsets.UTF_8);
        Assert.assertTrue(manifest.contains("Bundle-Localization: OSGI-INF/l10n/bundle"));

        String schemaNodeSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdSchemaNode.java");
        Assert.assertTrue(schemaNodeSource.contains("return ownerCatalog;"));
        Assert.assertTrue(schemaNodeSource.contains("return super.getTables(monitor);"));
        Assert.assertTrue(schemaNodeSource.contains("return super.getTable(monitor, name);"));
        Assert.assertTrue(schemaNodeSource.contains("getTableCache().setCache(tables);"));
        Assert.assertTrue(schemaNodeSource.contains("session.getMetaData().getTables(null, fullPath, \"%\", PHYSICAL_TABLE_TYPES)"));
        Assert.assertTrue(schemaNodeSource.contains("session.getMetaData().getTables(null, fullPath, \"%\", VIEW_TYPES)"));
        Assert.assertTrue(schemaNodeSource.contains("new ScratchBirdTable(this, tableName"));
        Assert.assertTrue(schemaNodeSource.contains("new ScratchBirdView(this, viewName"));
        Assert.assertTrue(schemaNodeSource.contains("return querySchema.getDataTypes(monitor);"));
        Assert.assertTrue(schemaNodeSource.contains("return getConstraintKeysCache().getObjects(monitor, this, null);"));
        Assert.assertTrue(schemaNodeSource.contains("ScratchBird schema constraints are not available for navigator"));
        Assert.assertTrue(schemaNodeSource.contains("ScratchBird schema indexes are not available for navigator"));
        Assert.assertTrue(schemaNodeSource.contains("public boolean isSchemaBranchesFolderVisible()"));
        Assert.assertTrue(schemaNodeSource.contains("isTableFoldersVisible()"));
        Assert.assertTrue(schemaNodeSource.contains("isDataTypesFolderVisible()"));
        Assert.assertTrue(schemaNodeSource.contains("childSchemas.isEmpty()"));

        String metaModelSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdMetaModel.java");
        Assert.assertTrue(metaModelSource.contains("public boolean supportsSequences"));
        Assert.assertTrue(metaModelSource.contains("FROM information_schema.sequences"));

        String dataSourceSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdDataSource.java");
        Assert.assertTrue(dataSourceSource.contains("public synchronized Collection<ScratchBirdSchemaNode> getSchemaTree"));
        Assert.assertTrue(dataSourceSource.contains("getOrCreateSyntheticRootCatalog()"));
        Assert.assertTrue(dataSourceSource.contains("syntheticRootCatalog = null;"));

        String catalogSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdCatalog.java");
        Assert.assertTrue(catalogSource.contains("WITH RECURSIVE schema_tree AS"));
        Assert.assertTrue(catalogSource.contains("JOIN schema_tree ON c.parent_object_id = schema_tree.object_id"));
        Assert.assertTrue(catalogSource.contains("ORDER BY depth, full_path"));

        String schemaNodeManagerSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/edit/ScratchBirdSchemaNodeManager.java");
        Assert.assertTrue(schemaNodeManagerSource.contains("container instanceof ScratchBirdSchemaNode schemaNode"));
        Assert.assertTrue(schemaNodeManagerSource.contains("return false;"));
        Assert.assertTrue(schemaNodeManagerSource.contains("container instanceof ScratchBirdCatalog scratchBirdCatalog"));
    }

    @Test
    public void scratchBirdDialectExposesScratchBirdSpecificCapabilities() {
        ScratchBirdSQLDialect dialect = new ScratchBirdSQLDialect();

        Assert.assertEquals("scratchbird", dialect.getDialectId());
        Assert.assertTrue(Arrays.asList(dialect.getDDLKeywords()).contains("TRUNCATE"));
        Assert.assertTrue(Arrays.asList(dialect.getExecuteKeywords()).contains("EXECUTE"));
        Assert.assertTrue(Arrays.asList(dialect.getExecuteKeywords()).contains("SHOW"));
        Assert.assertTrue(Arrays.asList(dialect.getExecuteKeywords()).contains("SET"));
        Assert.assertTrue(Arrays.asList(dialect.getExecuteKeywords()).contains("RESET"));
        Assert.assertTrue(Arrays.asList(dialect.getExecuteKeywords()).contains("DESCRIBE"));
        Assert.assertTrue(Arrays.asList(dialect.getExecuteKeywords()).contains("CLUSTER"));
        Assert.assertTrue(Arrays.asList(dialect.getExecuteKeywords()).contains("VALIDATE"));
        Assert.assertTrue(Arrays.asList(dialect.getExecuteKeywords()).contains("SWEEP"));
        Assert.assertFalse(Arrays.asList(dialect.getExecuteKeywords()).contains("DESC"));
        Assert.assertFalse(Arrays.asList(dialect.getExecuteKeywords()).contains("EXEC"));
        Assert.assertFalse(Arrays.asList(dialect.getExecuteKeywords()).contains("DO"));
        Assert.assertNull(dialect.getKeywordType("left"));
        Assert.assertNull(dialect.getKeywordType("value"));
        Assert.assertEquals(DBPKeywordType.KEYWORD, dialect.getKeywordType("SHOW"));
        Assert.assertEquals(DBPKeywordType.KEYWORD, dialect.getKeywordType("STATUS"));
        Assert.assertEquals(DBPKeywordType.KEYWORD, dialect.getKeywordType("METRICS"));
        Assert.assertNull(dialect.getKeywordType("VARIABLES"));
        Assert.assertNull(dialect.getKeywordType("PROCESSLIST"));
        Assert.assertEquals(DBPKeywordType.TYPE, dialect.getKeywordType("UUID"));
        Assert.assertTrue(dialect.supportsAliasInHaving());
        Assert.assertTrue(dialect.supportsInsertAllDefaultValuesStatement());
        Assert.assertTrue(dialect.supportsIndexCreateAndDrop());
        Assert.assertTrue(dialect.validIdentifierPart('$', false));
    }

    @Test
    public void javaV3ParserUsesGatekeeperKeywordsAndContextualDispatch() {
        ScratchBirdV3Lexer lexer = new ScratchBirdV3Lexer("CREATE TABLE t (left int, value varchar(20));");
        Assert.assertTrue(lexer.tokenize().stream()
            .anyMatch(token -> token.type() == ScratchBirdV3TokenType.IDENTIFIER && "left".equalsIgnoreCase(token.text())));
        Assert.assertFalse(ScratchBirdV3Parser.gatekeeperKeywords().contains("LEFT"));
        Assert.assertFalse(ScratchBirdV3Parser.gatekeeperKeywords().contains("VALUE"));

        ScratchBirdV3ParseResult result = ScratchBirdV3Parser.parse("""
            SHOW SCHEMA;
            SHOW MANAGEMENT LISTENERS;
            SET SCHEMA users.public;
            CREATE DOMAIN sys.domains.email AS VARCHAR(320);
            ALTER TRIGGER data.app.orders_bi ACTIVE;
            EXECUTE BLOCK AS BEGIN RETURN; END;
            """);
        Assert.assertTrue(result.diagnostics().toString(), result.success());
        Assert.assertEquals(6, result.statements().size());
        Assert.assertEquals(ScratchBirdV3StatementKind.SHOW, result.statements().get(0).kind());
        Assert.assertEquals(ScratchBirdV3StatementFamily.MANAGEMENT, result.statements().get(1).family());
        Assert.assertEquals(ScratchBirdV3StatementKind.SET, result.statements().get(2).kind());
        Assert.assertEquals("CREATE DOMAIN", result.statements().get(3).surface());
        Assert.assertEquals("ALTER TRIGGER", result.statements().get(4).surface());
        Assert.assertEquals(ScratchBirdV3StatementKind.EXECUTE_BLOCK, result.statements().get(5).kind());

        ScratchBirdV3ParseResult showV3Surfaces = ScratchBirdV3Parser.parse("""
            SHOW METRICS;
            SHOW CURRENT_SCHEMA;
            SHOW SEARCH PATH;
            SHOW ALL;
            SHOW VERSION;
            SHOW DATABASE;
            SHOW SYSTEM;
            SHOW TIME ZONE;
            SHOW SQL DIALECT;
            """);
        Assert.assertTrue(showV3Surfaces.diagnostics().toString(), showV3Surfaces.success());
        Assert.assertEquals(9, showV3Surfaces.statements().size());
        Assert.assertEquals("SHOW METRICS", showV3Surfaces.statements().get(0).surface());
        Assert.assertEquals("SHOW CURRENT_SCHEMA", showV3Surfaces.statements().get(1).surface());
        Assert.assertEquals("SHOW SEARCH PATH", showV3Surfaces.statements().get(2).surface());

        ScratchBirdV3ParseResult rejected = ScratchBirdV3Parser.parse("DESC sys.catalog.tables;");
        Assert.assertFalse(rejected.success());
        Assert.assertTrue(rejected.diagnostics().get(0).message().contains("DESC alias"));

        Assert.assertTrue(ScratchBirdV3Parser.completionsAt("SHOW ", 5).stream()
            .anyMatch(completion -> "SCHEMA".equals(completion.label())));
        Assert.assertTrue(ScratchBirdV3Parser.completionsAt("SHOW ", 5).stream()
            .anyMatch(completion -> "METRICS".equals(completion.label())));
        Assert.assertTrue(ScratchBirdV3Parser.completionsAt("SHOW V", 6).stream()
            .anyMatch(completion -> "VERSION".equals(completion.label())));
        Assert.assertTrue(ScratchBirdV3Parser.completionsAt("SHOW STATUS L", 13).isEmpty());
        Assert.assertTrue(ScratchBirdV3Parser.completionsAt("SHOW GLOBAL ", 12).isEmpty());
        Assert.assertTrue(ScratchBirdV3Parser.completionsAt("SHOW MAN", 8).stream()
            .anyMatch(completion -> "MANAGEMENT".equals(completion.label())));
        Assert.assertTrue(ScratchBirdV3Parser.completionsAt("SHOW MANAGEMENT ", 16).stream()
            .anyMatch(completion -> "LISTENERS".equals(completion.label())));
        Assert.assertTrue(ScratchBirdV3Parser.completionsAt("SHOW CLUSTER ", 13).stream()
            .anyMatch(completion -> "ROUTING PLAN".equals(completion.label())));
        Assert.assertTrue(ScratchBirdV3Parser.completionsAt("SET SCHEMA ", 11).stream()
            .anyMatch(completion -> "users.public".equals(completion.label())));
        Assert.assertTrue(ScratchBirdV3Parser.completionsAt("ALTER ", 6).stream()
            .anyMatch(completion -> "TRIGGER".equals(completion.label())));
        Assert.assertTrue(ScratchBirdV3Parser.completionsAt("CREATE TABLE t ", 15).stream()
            .anyMatch(completion -> "PRIMARY KEY".equals(completion.label())));
        Assert.assertTrue(ScratchBirdV3Parser.gatekeeperKeywords().contains("CLUSTER"));
        Assert.assertTrue(ScratchBirdV3Parser.gatekeeperKeywords().contains("VALIDATE"));
        Assert.assertTrue(ScratchBirdV3Parser.dialectKeywords().contains("METRICS"));
        Assert.assertFalse(ScratchBirdV3Parser.dialectKeywords().contains("VARIABLES"));
        Assert.assertFalse(ScratchBirdV3Parser.gatekeeperKeywords().contains("VARIABLES"));
        Assert.assertTrue(ScratchBirdValidationBridge.diagnosticsFor("SHOW SCHEMA;").get(0)
            .contains("Java v3 parser accepted"));
        Assert.assertTrue(ScratchBirdValidationBridge.diagnosticsFor("SHOW METRICS;").get(0)
            .contains("Java v3 parser accepted"));
        Assert.assertTrue(ScratchBirdValidationBridge.completionHintsFor("SHOW ", 5).stream()
            .anyMatch(hint -> hint.contains("METRICS")));
    }

    @Test
    public void sqlPromptPlannerBuildsInlinePromptAndStatusMessageFromParserContext() {
        ScratchBirdSqlPromptPlanner.PromptPlan showPrompt = ScratchBirdSqlPromptPlanner.plan("SHOW ", 5);
        Assert.assertEquals("SCHEMA", showPrompt.inlineSuggestion());
        Assert.assertTrue(showPrompt.statusMessage().contains("SCHEMA"));
        Assert.assertTrue(showPrompt.statusMessage().contains("Tab/Right accepts the inline prompt."));

        ScratchBirdSqlPromptPlanner.PromptPlan managementPrompt = ScratchBirdSqlPromptPlanner.plan("SHOW MAN", 8);
        Assert.assertEquals("MANAGEMENT", managementPrompt.inlineSuggestion());
        Assert.assertTrue(managementPrompt.statusMessage().contains("MANAGEMENT"));

        ScratchBirdSqlPromptPlanner.PromptPlan inTokenPrompt = ScratchBirdSqlPromptPlanner.plan("SHOW MANAGEMENT", 10);
        Assert.assertNull(inTokenPrompt.inlineSuggestion());
        Assert.assertTrue(inTokenPrompt.statusMessage().contains("MANAGEMENT"));

        ScratchBirdSqlPromptPlanner.PromptPlan blankPrompt = ScratchBirdSqlPromptPlanner.plan("", 0);
        Assert.assertNull(blankPrompt.inlineSuggestion());
        Assert.assertTrue(blankPrompt.statusMessage().contains("no SQL text"));

        Assert.assertTrue(ScratchBirdSqlPromptPlanner.completionCandidates("SHOW ", 5).stream()
            .anyMatch(completion -> "SCHEMA".equals(completion.label())));
        Assert.assertTrue(ScratchBirdSqlPromptPlanner.completionCandidates("SHOW V", 6).stream()
            .anyMatch(completion -> "VERSION".equals(completion.label())));
        Assert.assertTrue(ScratchBirdSqlPromptPlanner.completionCandidates("SHOW STATUS ", 12).isEmpty());
        Assert.assertEquals("AGEMENT", ScratchBirdSqlPromptPlanner.proposalInsertion("SHOW MAN", 8, "MANAGEMENT"));
        Assert.assertEquals("SION", ScratchBirdSqlPromptPlanner.proposalInsertion("SHOW VER", 8, "VERSION"));
        Assert.assertEquals("", ScratchBirdSqlPromptPlanner.proposalInsertion("SHOW MANAGEMENT", 15, "MANAGEMENT"));
        Assert.assertEquals("SCHEMA", ScratchBirdSqlPromptPlanner.proposalInsertion("SHOW ", 5, "SCHEMA"));
        Assert.assertEquals("MANAGEMENT", ScratchBirdSqlPromptPlanner.proposalInsertion("ALTER ", 6, "MANAGEMENT"));
    }

    @Test
    public void validationBridgeBuildsStatementInventoryAndLintHints() {
        List<String> inventory = ScratchBirdValidationBridge.statementSummaryFor("""
            SHOW MANAGEMENT LISTENERS;
            SET CURRENT_SCHEMA users.public
            """);
        Assert.assertEquals(2, inventory.size());
        Assert.assertTrue(inventory.get(0).contains("SHOW MANAGEMENT LISTENERS"));
        Assert.assertTrue(inventory.get(0).contains("MANAGEMENT"));
        Assert.assertTrue(inventory.get(1).contains("unterminated"));

        List<String> lintHints = ScratchBirdValidationBridge.lintHintsFor(
            "SET CURRENT_SCHEMA users.public",
            "SET CURRENT_SCHEMA users.public".length());
        Assert.assertTrue(lintHints.stream().anyMatch(hint ->
            hint.contains("prefer SET SCHEMA") || hint.contains("Prefer SET SCHEMA")));

        List<String> authHints = ScratchBirdValidationBridge.lintHintsFor(
            "SHOW MANAGEMENT LISTENERS;",
            "SHOW MANAGEMENT LISTENERS;".length());
        Assert.assertTrue(authHints.stream().anyMatch(hint -> hint.contains("server-controlled surface")));

        List<String> protectedBranchHints = ScratchBirdValidationBridge.lintHintsFor(
            "ALTER TABLE sys.catalog.object_resolver ADD COLUMN scratch_col INT;",
            "ALTER TABLE sys.catalog.object_resolver ADD COLUMN scratch_col INT;".length());
        Assert.assertTrue(protectedBranchHints.stream().anyMatch(hint -> hint.contains("SBBP-SYS-CATALOG")));
        Assert.assertTrue(protectedBranchHints.stream().anyMatch(hint -> hint.contains("SBDV-FRM-107")));

        List<String> domainPlacementHints = ScratchBirdValidationBridge.lintHintsFor(
            "CREATE DOMAIN data.app.email AS VARCHAR(320);",
            "CREATE DOMAIN data.app.email AS VARCHAR(320);".length());
        Assert.assertTrue(domainPlacementHints.stream().anyMatch(hint -> hint.contains("sys.domains")));

        List<String> domainCollectionPathHints = ScratchBirdValidationBridge.lintHintsFor(
            "ALTER DOMAIN sys.domains SET DEFAULT 'example';",
            "ALTER DOMAIN sys.domains SET DEFAULT 'example';".length());
        Assert.assertTrue(domainCollectionPathHints.toString(), domainCollectionPathHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L037")));
        Assert.assertTrue(domainCollectionPathHints.toString(), domainCollectionPathHints.stream().anyMatch(hint -> hint.contains("domain hub collection surface")));
        Assert.assertTrue(domainCollectionPathHints.toString(), domainCollectionPathHints.stream().anyMatch(hint -> hint.contains("sys.domains")));

        List<String> unrootedLifecycleHints = ScratchBirdValidationBridge.lintHintsFor(
            "CREATE DOMAIN email AS VARCHAR(320);",
            "CREATE DOMAIN email AS VARCHAR(320);".length());
        Assert.assertTrue(unrootedLifecycleHints.toString(), unrootedLifecycleHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L027")));
        Assert.assertTrue(unrootedLifecycleHints.toString(), unrootedLifecycleHints.stream().anyMatch(hint -> hint.contains("rooted ScratchBird path")));

        List<String> targetLintHints = ScratchBirdValidationBridge.lintHintsFor(
            "CREATE DOMAIN email AS VARCHAR(320);",
            "CREATE DOMAIN email AS VARCHAR(320);".length(),
            "sys.monitoring");
        Assert.assertTrue(targetLintHints.stream().anyMatch(hint -> hint.contains("sys.monitoring")));
        Assert.assertTrue(targetLintHints.stream().anyMatch(hint -> hint.contains("outside sys.domains")));

        List<String> domainCollectionTargetHints = ScratchBirdValidationBridge.lintHintsFor(
            "ALTER DOMAIN email SET DEFAULT 'example';",
            "ALTER DOMAIN email SET DEFAULT 'example';".length(),
            "sys.domains");
        Assert.assertTrue(domainCollectionTargetHints.toString(), domainCollectionTargetHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L038")));
        Assert.assertTrue(domainCollectionTargetHints.toString(), domainCollectionTargetHints.stream().anyMatch(hint -> hint.contains("domain hub collection surface")));
        Assert.assertTrue(domainCollectionTargetHints.toString(), domainCollectionTargetHints.stream().anyMatch(hint -> hint.contains("sys.domains")));

        List<String> targetMismatchHints = ScratchBirdValidationBridge.lintHintsFor(
            "ALTER TABLE sys.jobs.cleanup_job ADD COLUMN scratch_col INT;",
            "ALTER TABLE sys.jobs.cleanup_job ADD COLUMN scratch_col INT;".length(),
            "sys.monitoring");
        Assert.assertTrue(targetMismatchHints.stream().anyMatch(hint -> hint.contains("sys.jobs.cleanup_job")));
        Assert.assertTrue(targetMismatchHints.stream().anyMatch(hint -> hint.contains("retarget")));

        List<String> derivedManagementScopeHints = ScratchBirdValidationBridge.lintHintsFor(
            "SHOW MANAGEMENT LISTENERS;",
            "SHOW MANAGEMENT LISTENERS;".length(),
            "data.sales");
        Assert.assertTrue(derivedManagementScopeHints.toString(), derivedManagementScopeHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L028")));
        Assert.assertTrue(derivedManagementScopeHints.toString(), derivedManagementScopeHints.stream().anyMatch(hint -> hint.contains("sys.monitoring")));
        Assert.assertTrue(derivedManagementScopeHints.toString(), derivedManagementScopeHints.stream().anyMatch(hint -> hint.contains("data.sales")));

        List<String> derivedSecurityScopeHints = ScratchBirdValidationBridge.lintHintsFor(
            "GRANT SELECT ON data.sales.orders TO analyst;",
            "GRANT SELECT ON data.sales.orders TO analyst;".length(),
            "data.sales.orders");
        Assert.assertTrue(derivedSecurityScopeHints.toString(), derivedSecurityScopeHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L029")));
        Assert.assertTrue(derivedSecurityScopeHints.toString(), derivedSecurityScopeHints.stream().anyMatch(hint -> hint.contains("sys.security")));
        Assert.assertTrue(derivedSecurityScopeHints.toString(), derivedSecurityScopeHints.stream().anyMatch(hint -> hint.contains("data.sales.orders")));

        List<String> jobLifecycleScopeHints = ScratchBirdValidationBridge.lintHintsFor(
            "CREATE JOB cleanup_job AS SELECT 1;",
            "CREATE JOB cleanup_job AS SELECT 1;".length(),
            "data.sales");
        Assert.assertTrue(jobLifecycleScopeHints.toString(), jobLifecycleScopeHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L028")));
        Assert.assertTrue(jobLifecycleScopeHints.toString(), jobLifecycleScopeHints.stream().anyMatch(hint -> hint.contains("sys.jobs")));
        Assert.assertTrue(jobLifecycleScopeHints.toString(), jobLifecycleScopeHints.stream().anyMatch(hint -> hint.contains("SBDV-FRM-102")));

        List<String> securityLifecycleScopeHints = ScratchBirdValidationBridge.contextHintsFor(
            "CREATE ROLE analyst;",
            "CREATE ROLE analyst;".length());
        Assert.assertTrue(securityLifecycleScopeHints.toString(), securityLifecycleScopeHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_H031")));
        Assert.assertTrue(securityLifecycleScopeHints.toString(), securityLifecycleScopeHints.stream().anyMatch(hint -> hint.contains("sys.security")));
        Assert.assertTrue(securityLifecycleScopeHints.toString(), securityLifecycleScopeHints.stream().anyMatch(hint -> hint.contains("SBDV-FRM-103")));

        List<String> protectedDmlPathHints = ScratchBirdValidationBridge.lintHintsFor(
            "UPDATE sys.catalog.object_resolver SET object_name = 'scratch';",
            "UPDATE sys.catalog.object_resolver SET object_name = 'scratch';".length());
        Assert.assertTrue(protectedDmlPathHints.toString(), protectedDmlPathHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L039")));
        Assert.assertTrue(protectedDmlPathHints.toString(), protectedDmlPathHints.stream().anyMatch(hint -> hint.contains("inspect-only system surface")));
        Assert.assertTrue(protectedDmlPathHints.toString(), protectedDmlPathHints.stream().anyMatch(hint -> hint.contains("SBDV-FRM-107")));

        List<String> protectedDmlTargetHints = ScratchBirdValidationBridge.lintHintsFor(
            "UPDATE object_resolver SET object_name = 'scratch';",
            "UPDATE object_resolver SET object_name = 'scratch';".length(),
            "sys.catalog");
        Assert.assertTrue(protectedDmlTargetHints.toString(), protectedDmlTargetHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L040")));
        Assert.assertTrue(protectedDmlTargetHints.toString(), protectedDmlTargetHints.stream().anyMatch(hint -> hint.contains("sys.catalog")));
        Assert.assertTrue(protectedDmlTargetHints.toString(), protectedDmlTargetHints.stream().anyMatch(hint -> hint.contains("retarget to a concrete mutable data object")));

        List<String> dataDmlHints = ScratchBirdValidationBridge.lintHintsFor(
            "UPDATE data.sales.orders SET status = 'closed';",
            "UPDATE data.sales.orders SET status = 'closed';".length(),
            "data.sales.orders");
        Assert.assertFalse(dataDmlHints.toString(), dataDmlHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L039")));
        Assert.assertFalse(dataDmlHints.toString(), dataDmlHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L040")));
        Assert.assertFalse(dataDmlHints.toString(), dataDmlHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L041")));
        Assert.assertFalse(dataDmlHints.toString(), dataDmlHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L042")));
        Assert.assertFalse(dataDmlHints.toString(), dataDmlHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L043")));

        List<String> dmlCollectionPathHints = ScratchBirdValidationBridge.lintHintsFor(
            "UPDATE data.sales SET status = 'closed';",
            "UPDATE data.sales SET status = 'closed';".length());
        Assert.assertTrue(dmlCollectionPathHints.toString(), dmlCollectionPathHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L041")));
        Assert.assertTrue(dmlCollectionPathHints.toString(), dmlCollectionPathHints.stream().anyMatch(hint -> hint.contains("data namespace collection surface")));
        Assert.assertTrue(dmlCollectionPathHints.toString(), dmlCollectionPathHints.stream().anyMatch(hint -> hint.contains("concrete mutable child object path")));

        List<String> dmlCollectionTargetHints = ScratchBirdValidationBridge.lintHintsFor(
            "UPDATE orders SET status = 'closed';",
            "UPDATE orders SET status = 'closed';".length(),
            "data.sales");
        Assert.assertTrue(dmlCollectionTargetHints.toString(), dmlCollectionTargetHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L042")));
        Assert.assertTrue(dmlCollectionTargetHints.toString(), dmlCollectionTargetHints.stream().anyMatch(hint -> hint.contains("selected target data.sales")));
        Assert.assertTrue(dmlCollectionTargetHints.toString(), dmlCollectionTargetHints.stream().anyMatch(hint -> hint.contains("fully qualify the DML target")));

        List<String> unrootedDmlHints = ScratchBirdValidationBridge.lintHintsFor(
            "UPDATE orders SET status = 'closed';",
            "UPDATE orders SET status = 'closed';".length());
        Assert.assertTrue(unrootedDmlHints.toString(), unrootedDmlHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L043")));
        Assert.assertTrue(unrootedDmlHints.toString(), unrootedDmlHints.stream().anyMatch(hint -> hint.contains("rooted ScratchBird DML target")));
        Assert.assertTrue(unrootedDmlHints.toString(), unrootedDmlHints.stream().anyMatch(hint -> hint.contains("no selected object context")));

        List<String> selectedObjectDmlHints = ScratchBirdValidationBridge.lintHintsFor(
            "UPDATE orders SET status = 'closed';",
            "UPDATE orders SET status = 'closed';".length(),
            "data.sales.orders");
        Assert.assertFalse(selectedObjectDmlHints.toString(), selectedObjectDmlHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L043")));
        Assert.assertFalse(selectedObjectDmlHints.toString(), selectedObjectDmlHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L042")));

        List<String> unrootedDclHints = ScratchBirdValidationBridge.lintHintsFor(
            "GRANT SELECT ON orders TO analyst;",
            "GRANT SELECT ON orders TO analyst;".length());
        Assert.assertTrue(unrootedDclHints.toString(), unrootedDclHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L044")));
        Assert.assertTrue(unrootedDclHints.toString(), unrootedDclHints.stream().anyMatch(hint -> hint.contains("rooted ScratchBird securable object")));
        Assert.assertTrue(unrootedDclHints.toString(), unrootedDclHints.stream().anyMatch(hint -> hint.contains("sys.security")));

        List<String> rootedDclHints = ScratchBirdValidationBridge.lintHintsFor(
            "GRANT SELECT ON data.sales.orders TO analyst;",
            "GRANT SELECT ON data.sales.orders TO analyst;".length());
        Assert.assertFalse(rootedDclHints.toString(), rootedDclHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L044")));
        Assert.assertFalse(rootedDclHints.toString(), rootedDclHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L045")));
        Assert.assertFalse(rootedDclHints.toString(), rootedDclHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L047")));

        List<String> rootedDclPrincipalPathHints = ScratchBirdValidationBridge.lintHintsFor(
            "GRANT SELECT ON data.sales.orders TO users.public;",
            "GRANT SELECT ON data.sales.orders TO users.public;".length());
        Assert.assertFalse(rootedDclPrincipalPathHints.toString(), rootedDclPrincipalPathHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L044")));
        Assert.assertFalse(rootedDclPrincipalPathHints.toString(), rootedDclPrincipalPathHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L045")));
        Assert.assertFalse(rootedDclPrincipalPathHints.toString(), rootedDclPrincipalPathHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L047")));
        Assert.assertTrue(rootedDclPrincipalPathHints.toString(), rootedDclPrincipalPathHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L049")));
        Assert.assertTrue(rootedDclPrincipalPathHints.toString(), rootedDclPrincipalPathHints.stream().anyMatch(hint -> hint.contains("public principal collection surface")));

        List<String> unrootedDclWithRootedPrincipalHints = ScratchBirdValidationBridge.lintHintsFor(
            "GRANT SELECT ON orders TO users.public;",
            "GRANT SELECT ON orders TO users.public;".length());
        Assert.assertTrue(unrootedDclWithRootedPrincipalHints.toString(), unrootedDclWithRootedPrincipalHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L044")));
        Assert.assertTrue(unrootedDclWithRootedPrincipalHints.toString(), unrootedDclWithRootedPrincipalHints.stream().anyMatch(hint -> hint.contains("rooted ScratchBird securable object")));
        Assert.assertFalse(unrootedDclWithRootedPrincipalHints.toString(), unrootedDclWithRootedPrincipalHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L047")));
        Assert.assertTrue(unrootedDclWithRootedPrincipalHints.toString(), unrootedDclWithRootedPrincipalHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L049")));

        List<String> selectedDclPrincipalPathHints = ScratchBirdValidationBridge.lintHintsFor(
            "GRANT SELECT ON data.sales.orders TO users.public;",
            "GRANT SELECT ON data.sales.orders TO users.public;".length(),
            "data.sales.orders");
        Assert.assertFalse(selectedDclPrincipalPathHints.toString(), selectedDclPrincipalPathHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L025")));
        Assert.assertFalse(selectedDclPrincipalPathHints.toString(), selectedDclPrincipalPathHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L046")));
        Assert.assertFalse(selectedDclPrincipalPathHints.toString(), selectedDclPrincipalPathHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L048")));
        Assert.assertTrue(selectedDclPrincipalPathHints.toString(), selectedDclPrincipalPathHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L049")));

        List<String> dclBroadPrincipalTargetHints = ScratchBirdValidationBridge.lintHintsFor(
            "GRANT SELECT ON data.sales.orders TO analyst;",
            "GRANT SELECT ON data.sales.orders TO analyst;".length(),
            "users.public");
        Assert.assertTrue(dclBroadPrincipalTargetHints.toString(), dclBroadPrincipalTargetHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L050")));
        Assert.assertTrue(dclBroadPrincipalTargetHints.toString(), dclBroadPrincipalTargetHints.stream().anyMatch(hint -> hint.contains("selected target users.public")));
        Assert.assertTrue(dclBroadPrincipalTargetHints.toString(), dclBroadPrincipalTargetHints.stream().anyMatch(hint -> hint.contains("SBDV-FRM-110")));

        List<String> roleGrantBroadPrincipalHints = ScratchBirdValidationBridge.lintHintsFor(
            "GRANT analyst TO users.groups;",
            "GRANT analyst TO users.groups;".length());
        Assert.assertTrue(roleGrantBroadPrincipalHints.toString(), roleGrantBroadPrincipalHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L049")));
        Assert.assertTrue(roleGrantBroadPrincipalHints.toString(), roleGrantBroadPrincipalHints.stream().anyMatch(hint -> hint.contains("group principal collection surface")));
        Assert.assertFalse(roleGrantBroadPrincipalHints.toString(), roleGrantBroadPrincipalHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L044")));

        String schemaSecurityTransactionBatch = """
            BEGIN;
            CREATE TABLE data.sales.orders (id UUID PRIMARY KEY);
            GRANT SELECT ON data.sales.orders TO analyst;
            COMMIT;
            """;
        List<String> metadataTransactionHints = ScratchBirdValidationBridge.lintHintsFor(
            schemaSecurityTransactionBatch,
            schemaSecurityTransactionBatch.length());
        Assert.assertTrue(metadataTransactionHints.toString(), metadataTransactionHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L051")));
        Assert.assertTrue(metadataTransactionHints.toString(), metadataTransactionHints.stream().anyMatch(hint -> hint.contains("MGA commit-bound")));
        Assert.assertTrue(metadataTransactionHints.toString(), metadataTransactionHints.stream().anyMatch(hint -> hint.contains("parser-assist deltas are committed-only")));
        Assert.assertTrue(metadataTransactionHints.toString(), metadataTransactionHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L052")));
        Assert.assertTrue(metadataTransactionHints.toString(), metadataTransactionHints.stream().anyMatch(hint -> hint.contains("schema epoch and security policy epoch")));
        Assert.assertTrue(metadataTransactionHints.toString(), metadataTransactionHints.stream().anyMatch(hint -> hint.contains("SBDV-FRM-110")));

        String uncommittedMetadataReadBatch = """
            BEGIN;
            CREATE TABLE data.sales.orders (id UUID PRIMARY KEY);
            SELECT * FROM sys.catalog.object_resolver;
            COMMIT;
            """;
        List<String> uncommittedMetadataReadHints = ScratchBirdValidationBridge.lintHintsFor(
            uncommittedMetadataReadBatch,
            uncommittedMetadataReadBatch.length());
        Assert.assertTrue(uncommittedMetadataReadHints.toString(), uncommittedMetadataReadHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L053")));
        Assert.assertTrue(uncommittedMetadataReadHints.toString(), uncommittedMetadataReadHints.stream().anyMatch(hint -> hint.contains("committed catalog baseline")));
        Assert.assertTrue(uncommittedMetadataReadHints.toString(), uncommittedMetadataReadHints.stream().anyMatch(hint -> hint.contains("transaction-local overlays")));
        Assert.assertTrue(uncommittedMetadataReadHints.toString(), uncommittedMetadataReadHints.stream().anyMatch(hint -> hint.contains("live server results")));

        List<String> selectedObjectDclHints = ScratchBirdValidationBridge.lintHintsFor(
            "GRANT SELECT ON orders TO analyst;",
            "GRANT SELECT ON orders TO analyst;".length(),
            "data.sales.orders");
        Assert.assertFalse(selectedObjectDclHints.toString(), selectedObjectDclHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L044")));
        Assert.assertFalse(selectedObjectDclHints.toString(), selectedObjectDclHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L046")));
        Assert.assertFalse(selectedObjectDclHints.toString(), selectedObjectDclHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L048")));

        List<String> dclCollectionPathHints = ScratchBirdValidationBridge.lintHintsFor(
            "GRANT SELECT ON data.sales TO analyst;",
            "GRANT SELECT ON data.sales TO analyst;".length());
        Assert.assertTrue(dclCollectionPathHints.toString(), dclCollectionPathHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L045")));
        Assert.assertTrue(dclCollectionPathHints.toString(), dclCollectionPathHints.stream().anyMatch(hint -> hint.contains("securable collection surface")));
        Assert.assertTrue(dclCollectionPathHints.toString(), dclCollectionPathHints.stream().anyMatch(hint -> hint.contains("concrete securable child object")));

        List<String> dclCollectionTargetHints = ScratchBirdValidationBridge.lintHintsFor(
            "GRANT SELECT ON orders TO analyst;",
            "GRANT SELECT ON orders TO analyst;".length(),
            "data.sales");
        Assert.assertTrue(dclCollectionTargetHints.toString(), dclCollectionTargetHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L046")));
        Assert.assertTrue(dclCollectionTargetHints.toString(), dclCollectionTargetHints.stream().anyMatch(hint -> hint.contains("selected target data.sales")));
        Assert.assertTrue(dclCollectionTargetHints.toString(), dclCollectionTargetHints.stream().anyMatch(hint -> hint.contains("fully qualify the ON target")));

        List<String> rootedDclFromCollectionTargetHints = ScratchBirdValidationBridge.lintHintsFor(
            "GRANT SELECT ON data.sales.orders TO analyst;",
            "GRANT SELECT ON data.sales.orders TO analyst;".length(),
            "data.sales");
        Assert.assertFalse(rootedDclFromCollectionTargetHints.toString(), rootedDclFromCollectionTargetHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L046")));

        List<String> dclBroadSurfacePathHints = ScratchBirdValidationBridge.lintHintsFor(
            "GRANT SELECT ON sys.catalog TO auditor;",
            "GRANT SELECT ON sys.catalog TO auditor;".length());
        Assert.assertTrue(dclBroadSurfacePathHints.toString(), dclBroadSurfacePathHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L047")));
        Assert.assertTrue(dclBroadSurfacePathHints.toString(), dclBroadSurfacePathHints.stream().anyMatch(hint -> hint.contains("administrative collection surface")));
        Assert.assertTrue(dclBroadSurfacePathHints.toString(), dclBroadSurfacePathHints.stream().anyMatch(hint -> hint.contains("SBDV-FRM-110")));

        List<String> dclMetricsSurfaceHints = ScratchBirdValidationBridge.lintHintsFor(
            "GRANT SELECT ON metrics.latency TO analyst;",
            "GRANT SELECT ON metrics.latency TO analyst;".length());
        Assert.assertTrue(dclMetricsSurfaceHints.toString(), dclMetricsSurfaceHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L047")));
        Assert.assertTrue(dclMetricsSurfaceHints.toString(), dclMetricsSurfaceHints.stream().anyMatch(hint -> hint.contains("client-only metrics report surface")));

        List<String> dclBroadSurfaceTargetHints = ScratchBirdValidationBridge.lintHintsFor(
            "GRANT SELECT ON object_resolver TO auditor;",
            "GRANT SELECT ON object_resolver TO auditor;".length(),
            "sys.catalog");
        Assert.assertTrue(dclBroadSurfaceTargetHints.toString(), dclBroadSurfaceTargetHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L048")));
        Assert.assertTrue(dclBroadSurfaceTargetHints.toString(), dclBroadSurfaceTargetHints.stream().anyMatch(hint -> hint.contains("selected target sys.catalog")));
        Assert.assertTrue(dclBroadSurfaceTargetHints.toString(), dclBroadSurfaceTargetHints.stream().anyMatch(hint -> hint.contains("fully qualify the ON target")));

        List<String> rootedDclFromBroadTargetHints = ScratchBirdValidationBridge.lintHintsFor(
            "GRANT SELECT ON sys.catalog.object_resolver TO auditor;",
            "GRANT SELECT ON sys.catalog.object_resolver TO auditor;".length(),
            "sys.catalog");
        Assert.assertFalse(rootedDclFromBroadTargetHints.toString(), rootedDclFromBroadTargetHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L048")));

        List<String> targetObjectFamilyMismatchHints = ScratchBirdValidationBridge.lintHintsFor(
            "CREATE TABLE scratch_tmp (id UUID PRIMARY KEY);",
            "CREATE TABLE scratch_tmp (id UUID PRIMARY KEY);".length(),
            "users.home.alice.links");
        Assert.assertTrue(targetObjectFamilyMismatchHints.toString(), targetObjectFamilyMismatchHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L030")));
        Assert.assertTrue(targetObjectFamilyMismatchHints.toString(), targetObjectFamilyMismatchHints.stream().anyMatch(hint -> hint.contains("SBBP-USER-LINKS")));
        Assert.assertTrue(targetObjectFamilyMismatchHints.toString(), targetObjectFamilyMismatchHints.stream().anyMatch(hint -> hint.contains("users.home.alice.links")));

        List<String> pathObjectFamilyMismatchHints = ScratchBirdValidationBridge.lintHintsFor(
            "ALTER TABLE sys.jobs.cleanup_job ADD COLUMN scratch_col INT;",
            "ALTER TABLE sys.jobs.cleanup_job ADD COLUMN scratch_col INT;".length());
        Assert.assertTrue(pathObjectFamilyMismatchHints.toString(), pathObjectFamilyMismatchHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L031")));
        Assert.assertTrue(pathObjectFamilyMismatchHints.toString(), pathObjectFamilyMismatchHints.stream().anyMatch(hint -> hint.contains("SBBP-SYS-JOBS")));
        Assert.assertTrue(pathObjectFamilyMismatchHints.toString(), pathObjectFamilyMismatchHints.stream().anyMatch(hint -> hint.contains("job definition")));

        List<String> specialSurfacePathHints = ScratchBirdValidationBridge.lintHintsFor(
            "ALTER TABLE users.home.alice.scratch ADD COLUMN scratch_col INT;",
            "ALTER TABLE users.home.alice.scratch ADD COLUMN scratch_col INT;".length());
        Assert.assertTrue(specialSurfacePathHints.toString(), specialSurfacePathHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L032")));
        Assert.assertTrue(specialSurfacePathHints.toString(), specialSurfacePathHints.stream().anyMatch(hint -> hint.contains("scratch workspace")));
        Assert.assertTrue(specialSurfacePathHints.toString(), specialSurfacePathHints.stream().anyMatch(hint -> hint.contains("users.home.alice.scratch")));

        List<String> specialSurfaceTargetHints = ScratchBirdValidationBridge.lintHintsFor(
            "DROP VIEW users.groups.team.public.orders;",
            "DROP VIEW users.groups.team.public.orders;".length(),
            "users.groups.team.public");
        Assert.assertTrue(specialSurfaceTargetHints.toString(), specialSurfaceTargetHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L033")));
        Assert.assertTrue(specialSurfaceTargetHints.toString(), specialSurfaceTargetHints.stream().anyMatch(hint -> hint.contains("group public")));
        Assert.assertTrue(specialSurfaceTargetHints.toString(), specialSurfaceTargetHints.stream().anyMatch(hint -> hint.contains("users.groups.team.public")));

        List<String> dataNamespacePathHints = ScratchBirdValidationBridge.lintHintsFor(
            "ALTER TABLE data.sales ADD COLUMN scratch_col INT;",
            "ALTER TABLE data.sales ADD COLUMN scratch_col INT;".length());
        Assert.assertTrue(dataNamespacePathHints.toString(), dataNamespacePathHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L035")));
        Assert.assertTrue(dataNamespacePathHints.toString(), dataNamespacePathHints.stream().anyMatch(hint -> hint.contains("data namespace")));
        Assert.assertTrue(dataNamespacePathHints.toString(), dataNamespacePathHints.stream().anyMatch(hint -> hint.contains("data.sales")));

        List<String> dataNamespaceTargetHints = ScratchBirdValidationBridge.lintHintsFor(
            "ALTER TABLE sales ADD COLUMN scratch_col INT;",
            "ALTER TABLE sales ADD COLUMN scratch_col INT;".length(),
            "data.sales");
        Assert.assertTrue(dataNamespaceTargetHints.toString(), dataNamespaceTargetHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L036")));
        Assert.assertTrue(dataNamespaceTargetHints.toString(), dataNamespaceTargetHints.stream().anyMatch(hint -> hint.contains("data namespace")));
        Assert.assertTrue(dataNamespaceTargetHints.toString(), dataNamespaceTargetHints.stream().anyMatch(hint -> hint.contains("data.sales")));

        List<String> parentObjectTargetHints = ScratchBirdValidationBridge.lintHintsFor(
            "ALTER TABLE data.sales.orders ADD COLUMN scratch_col INT;",
            "ALTER TABLE data.sales.orders ADD COLUMN scratch_col INT;".length(),
            "data.sales");
        Assert.assertTrue(parentObjectTargetHints.toString(), parentObjectTargetHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_L034")));
        Assert.assertTrue(parentObjectTargetHints.toString(), parentObjectTargetHints.stream().anyMatch(hint -> hint.contains("data.sales.orders")));
        Assert.assertTrue(parentObjectTargetHints.toString(), parentObjectTargetHints.stream().anyMatch(hint -> hint.contains("parent surface data.sales")));

        List<String> contextHints = ScratchBirdValidationBridge.contextHintsFor(
            "SHOW MANAGEMENT LISTENERS;",
            "SHOW MANAGEMENT LISTENERS;".length());
        Assert.assertTrue(contextHints.stream().anyMatch(hint -> hint.contains("SBDV-FRM-106")));
        Assert.assertTrue(contextHints.stream().anyMatch(hint -> hint.contains("Run Authz Probe")));
        Assert.assertTrue(contextHints.toString(), contextHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_H031")));
        Assert.assertTrue(contextHints.toString(), contextHints.stream().anyMatch(hint -> hint.contains("sys.monitoring")));

        List<String> objectFamilyContextHints = ScratchBirdValidationBridge.contextHintsFor(
            "CREATE TABLE users.home.alice.scratch.tmp (id UUID PRIMARY KEY);",
            "CREATE TABLE users.home.alice.scratch.tmp (id UUID PRIMARY KEY);".length(),
            "users.home.alice.scratch");
        Assert.assertTrue(objectFamilyContextHints.toString(), objectFamilyContextHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_H032")));
        Assert.assertTrue(objectFamilyContextHints.toString(), objectFamilyContextHints.stream().anyMatch(hint -> hint.contains("workspace object")));
        Assert.assertTrue(objectFamilyContextHints.toString(), objectFamilyContextHints.stream().anyMatch(hint -> hint.contains("SBBP-USER-SCRATCH")));

        List<String> targetContextHints = ScratchBirdValidationBridge.contextHintsFor(
            "CREATE DOMAIN sys.domains.email AS VARCHAR(320);",
            "CREATE DOMAIN sys.domains.email AS VARCHAR(320);".length(),
            "sys.domains");
        Assert.assertTrue(targetContextHints.stream().anyMatch(hint -> hint.contains("SBDV-FRM-611")));
        Assert.assertTrue(targetContextHints.stream().anyMatch(hint -> hint.contains("SBBP-SYS-DOMAINS")));

        List<String> serverProbeHints = ScratchBirdValidationBridge.serverProbeHintsFor(
            "SHOW MANAGEMENT LISTENERS;",
            "SHOW MANAGEMENT LISTENERS;".length());
        Assert.assertTrue(serverProbeHints.stream().anyMatch(hint -> hint.contains("sys.monitoring")));
        Assert.assertTrue(serverProbeHints.stream().anyMatch(hint -> hint.contains("sys.server_capabilities")));
        Assert.assertTrue(serverProbeHints.stream().anyMatch(hint -> hint.contains("SHOW MANAGEMENT LISTENERS")));

        List<String> domainProbeHints = ScratchBirdValidationBridge.serverProbeHintsFor(
            "CREATE DOMAIN data.app.email AS VARCHAR(320);",
            "CREATE DOMAIN data.app.email AS VARCHAR(320);".length());
        Assert.assertTrue(domainProbeHints.stream().anyMatch(hint -> hint.contains("sys.domains")));
        Assert.assertTrue(domainProbeHints.stream().anyMatch(hint -> hint.contains("SBDV-FRM-611")));

        List<String> securityProbeHints = ScratchBirdValidationBridge.serverProbeHintsFor(
            "CREATE ROLE analyst;",
            "CREATE ROLE analyst;".length());
        Assert.assertTrue(securityProbeHints.toString(), securityProbeHints.stream().anyMatch(hint -> hint.contains("sys.security")));
        Assert.assertTrue(securityProbeHints.toString(), securityProbeHints.stream().anyMatch(hint -> hint.contains("SBDV-FRM-103")));

        List<String> relationalTableProbeHints = ScratchBirdValidationBridge.serverProbeHintsFor(
            "CREATE TABLE data.sales.orders (id UUID PRIMARY KEY);",
            "CREATE TABLE data.sales.orders (id UUID PRIMARY KEY);".length());
        Assert.assertTrue(relationalTableProbeHints.toString(), relationalTableProbeHints.stream().anyMatch(hint -> hint.contains("SBDV-FRM-601")));
        Assert.assertTrue(relationalTableProbeHints.toString(), relationalTableProbeHints.stream().anyMatch(hint -> hint.contains("CREATE TABLE data.sales.orders")));

        List<String> formHints = ScratchBirdValidationBridge.formHintsFor(
            "SHOW MANAGEMENT LISTENERS;",
            "SHOW MANAGEMENT LISTENERS;".length());
        Assert.assertTrue(formHints.toString(), formHints.stream().anyMatch(hint -> hint.contains("SBDV-FRM-106")));
        Assert.assertTrue(formHints.toString(), formHints.stream().anyMatch(hint -> hint.contains("admin-surface")));
        Assert.assertTrue(formHints.toString(), formHints.stream().anyMatch(hint -> hint.contains("SBBP-SYS-MONITORING")));
        Assert.assertTrue(formHints.toString(), formHints.stream().anyMatch(hint -> hint.contains("process scope")));

        List<String> domainFormHints = ScratchBirdValidationBridge.formHintsFor(
            "CREATE DOMAIN data.app.email AS VARCHAR(320);",
            "CREATE DOMAIN data.app.email AS VARCHAR(320);".length());
        Assert.assertTrue(domainFormHints.toString(), domainFormHints.stream().anyMatch(hint -> hint.contains("SBDV-FRM-611")));
        Assert.assertTrue(domainFormHints.toString(), domainFormHints.stream().anyMatch(hint -> hint.contains("SBDV-FRM-015")));
        Assert.assertTrue(domainFormHints.toString(), domainFormHints.stream().anyMatch(hint -> hint.contains("SBBP-SYS-DOMAINS")));
        Assert.assertTrue(domainFormHints.toString(), domainFormHints.stream().anyMatch(hint -> hint.contains("datatype family")));
        Assert.assertTrue(domainFormHints.toString(), domainFormHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_F005")));
        Assert.assertTrue(domainFormHints.toString(), domainFormHints.stream().anyMatch(hint -> hint.contains("domain or datatype definition")));
        Assert.assertTrue(domainFormHints.toString(), domainFormHints.stream().anyMatch(hint -> hint.contains("catalog identity source sys.catalog.object_resolver")));

        List<String> nonRelationalTableFormHints = ScratchBirdValidationBridge.formHintsFor(
            "CREATE TABLE data.docs.events (id UUID PRIMARY KEY, payload JSONB);",
            "CREATE TABLE data.docs.events (id UUID PRIMARY KEY, payload JSONB);".length());
        Assert.assertTrue(nonRelationalTableFormHints.toString(), nonRelationalTableFormHints.stream().anyMatch(hint -> hint.contains("SBDV-FRM-602")));
        Assert.assertTrue(nonRelationalTableFormHints.toString(), nonRelationalTableFormHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_F004")));
        Assert.assertTrue(nonRelationalTableFormHints.toString(), nonRelationalTableFormHints.stream().anyMatch(hint -> hint.contains("data.docs.events")));
        Assert.assertTrue(nonRelationalTableFormHints.toString(), nonRelationalTableFormHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_F005")));
        Assert.assertTrue(nonRelationalTableFormHints.toString(), nonRelationalTableFormHints.stream().anyMatch(hint -> hint.contains("non-relational table")));
        Assert.assertTrue(nonRelationalTableFormHints.toString(), nonRelationalTableFormHints.stream().anyMatch(hint -> hint.contains("parent path data.docs")));
        Assert.assertTrue(nonRelationalTableFormHints.toString(), nonRelationalTableFormHints.stream().anyMatch(hint -> hint.contains("PRS_JV3_F006")));
        Assert.assertTrue(nonRelationalTableFormHints.toString(), nonRelationalTableFormHints.stream().anyMatch(hint -> hint.contains("live object evidence")));
        Assert.assertTrue(nonRelationalTableFormHints.toString(), nonRelationalTableFormHints.stream().anyMatch(hint -> hint.contains("Read-only surrogate probe")));
    }

    @Test
    public void scratchBirdProviderHonorsExplicitUrlConfiguration() {
        DBPDriver driver = (DBPDriver) Proxy.newProxyInstance(
            DBPDriver.class.getClassLoader(),
            new Class<?>[] {DBPDriver.class},
            (proxy, method, args) -> {
                if (method.getDeclaringClass() == Object.class) {
                    return switch (method.getName()) {
                        case "toString" -> "URLModeDriverProxy";
                        case "hashCode" -> System.identityHashCode(proxy);
                        case "equals" -> proxy == args[0];
                        default -> null;
                    };
                }
                throw new AssertionError("URL-mode provider should not consult driver metadata: " + method.getName());
            });

        DBPConnectionConfiguration connectionInfo = new DBPConnectionConfiguration();
        connectionInfo.setConfigurationType(DBPDriverConfigurationType.URL);
        connectionInfo.setUrl("jdbc:scratchbird://127.0.0.1:3092/main?sslmode=require");

        ScratchBirdDataSourceProvider provider = new ScratchBirdDataSourceProvider();
        Assert.assertEquals(connectionInfo.getUrl(), provider.getConnectionURL(driver, connectionInfo));
    }

    @Test
    public void scratchBirdValueProfilesGenerateCastSafeLiteralsAndTypedRoutes() throws Exception {
        ScratchBirdValueProfile jsonProfile = ScratchBirdValueProfile.fromTypeName("JSONB");
        Assert.assertEquals(ScratchBirdValueProfile.Family.JSON, jsonProfile.family());
        Assert.assertEquals(ScratchBirdValueProfile.HandlerRoute.STRUCTURED_CONTENT, jsonProfile.handlerRoute());
        Assert.assertEquals("application/json", jsonProfile.contentType());

        ScratchBirdValueProfile uuidProfile = ScratchBirdValueProfile.fromTypeName("UUID_V7");
        Assert.assertEquals(ScratchBirdValueProfile.Family.UUID, uuidProfile.family());
        Assert.assertEquals(ScratchBirdValueProfile.HandlerRoute.UUID, uuidProfile.handlerRoute());

        ScratchBirdValueProfile vectorProfile = ScratchBirdValueProfile.fromTypeName("VECTOR(1536)");
        Assert.assertEquals(ScratchBirdValueProfile.Family.VECTOR, vectorProfile.family());
        Assert.assertEquals(ScratchBirdValueProfile.HandlerRoute.STRUCTURED_TEXT, vectorProfile.handlerRoute());

        Assert.assertEquals("TRUE", ScratchBirdValueBinding.toSqlLiteral(true, "BOOLEAN"));
        Assert.assertEquals("42", ScratchBirdValueBinding.toSqlLiteral(42, "INTEGER"));
        Assert.assertEquals(
            "CAST('{\"sample\":true}' AS JSONB)",
            ScratchBirdValueBinding.exampleLiteralForType("JSONB"));
        Assert.assertEquals(
            "CAST('123e4567-e89b-12d3-a456-426614174000' AS UUID)",
            ScratchBirdValueBinding.toSqlLiteral("123e4567-e89b-12d3-a456-426614174000", "UUID"));
        Assert.assertEquals(
            "CAST('0x000fff' AS BYTEA)",
            ScratchBirdValueBinding.toSqlLiteral(new byte[]{0x00, 0x0f, (byte) 0xff}, "BYTEA"));
        Assert.assertEquals(
            "CAST('{\"k\":\"v\"}' AS JSONB)",
            ScratchBirdValueBinding.toSqlLiteral("{\"k\":\"v\"}", "JSONB"));
        Assert.assertEquals(
            "CAST('[1,2,3]' AS VECTOR)",
            ScratchBirdValueBinding.toSqlLiteral("[1,2,3]", "VECTOR"));

        Class<?> providerClass = Class.forName("org.jkiss.dbeaver.ext.scratchbird.model.data.ScratchBirdValueHandlerProvider");
        Object provider = providerClass.getConstructor().newInstance();
        Method getValueHandler = providerClass.getMethod(
            "getValueHandler",
            org.jkiss.dbeaver.model.DBPDataSource.class,
            org.jkiss.dbeaver.model.data.DBDFormatSettings.class,
            org.jkiss.dbeaver.model.struct.DBSTypedObject.class);

        Object jsonHandler = getValueHandler.invoke(provider, null, null, typedObject("JSONB"));
        Object vectorHandler = getValueHandler.invoke(provider, null, null, typedObject("VECTOR"));
        Object binaryHandler = getValueHandler.invoke(provider, null, null, typedObject("BYTEA"));
        Object uuidHandler = getValueHandler.invoke(provider, null, null, typedObject("UUID"));

        Assert.assertEquals(
            "org.jkiss.dbeaver.ext.scratchbird.model.data.ScratchBirdStructuredContentValueHandler",
            jsonHandler.getClass().getName());
        Assert.assertEquals(
            "org.jkiss.dbeaver.ext.scratchbird.model.data.ScratchBirdStructuredTextValueHandler",
            vectorHandler.getClass().getName());
        Assert.assertEquals(
            "org.jkiss.dbeaver.ext.scratchbird.model.data.ScratchBirdBinaryValueHandler",
            binaryHandler.getClass().getName());
        Assert.assertEquals(
            "org.jkiss.dbeaver.model.impl.jdbc.data.handlers.JDBCUUIDValueHandler",
            uuidHandler.getClass().getName());
    }

    @Test
    public void objectManagerSourcesCoverScratchBirdSpecificLifecycleDefaults() throws Exception {
        String supportSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/edit/ScratchBirdManagerSupport.java");
        Assert.assertTrue(supportSource.contains("defaultProcedureSource"));
        Assert.assertTrue(supportSource.contains("CREATE PROCEDURE "));
        Assert.assertTrue(supportSource.contains("defaultTableTriggerSource"));
        Assert.assertTrue(supportSource.contains("CREATE TRIGGER "));
        Assert.assertTrue(supportSource.contains("BEFORE INSERT ON "));
        Assert.assertTrue(supportSource.contains("isDomainPath"));
        Assert.assertTrue(supportSource.contains("isMetricsPath"));

        String domainTypeSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdDomainDataType.java");
        Assert.assertTrue(domainTypeSource.contains("implements DBPSaveableObject"));
        Assert.assertTrue(domainTypeSource.contains("private String baseTypeName;"));
        Assert.assertTrue(domainTypeSource.contains("private boolean required;"));
        Assert.assertTrue(domainTypeSource.contains("private String defaultExpression;"));
        Assert.assertTrue(domainTypeSource.contains("setPersisted(boolean persisted)"));

        String domainManagerSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/edit/ScratchBirdDomainManager.java");
        Assert.assertTrue(domainManagerSource.contains("CREATE DOMAIN "));
        Assert.assertTrue(domainManagerSource.contains("DROP DOMAIN "));
        Assert.assertTrue(domainManagerSource.contains("ScratchBird domains can only be created inside sys.domains."));

        String draftConstraintSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/edit/ScratchBirdDraftCheckConstraint.java");
        Assert.assertTrue(draftConstraintSource.contains("implements DBSTableCheckConstraint"));
        Assert.assertTrue(draftConstraintSource.contains("DBSEntityConstraintType.CHECK"));
        Assert.assertTrue(draftConstraintSource.contains("checkConstraintDefinition"));

        String constraintManagerSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/edit/ScratchBirdConstraintManager.java");
        Assert.assertTrue(constraintManagerSource.contains("ScratchBirdDraftCheckConstraint"));
        Assert.assertTrue(constraintManagerSource.contains("canCreateConstraint(container)"));

        String foreignKeyManagerSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/edit/ScratchBirdForeignKeyManager.java");
        Assert.assertTrue(foreignKeyManagerSource.contains("canCreateForeignKey(container)"));
        Assert.assertTrue(foreignKeyManagerSource.contains("defaultReferenceConstraint"));
        Assert.assertTrue(foreignKeyManagerSource.contains("GenericTableForeignKeyColumnTable"));

        String indexManagerSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/edit/ScratchBirdIndexManager.java");
        Assert.assertTrue(indexManagerSource.contains("canCreateIndex(container)"));
        Assert.assertTrue(indexManagerSource.contains("new_index"));
        Assert.assertTrue(indexManagerSource.contains("GenericTableIndexColumn"));

        String columnManagerSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/edit/ScratchBirdTableColumnManager.java");
        Assert.assertTrue(columnManagerSource.contains("JSONB"));
        Assert.assertTrue(columnManagerSource.contains("ScratchBirdManagerSupport.isNonRelational(tableBase)"));
        Assert.assertTrue(columnManagerSource.contains("createTableColumnImpl"));

        String sequenceManagerSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/edit/ScratchBirdSequenceManager.java");
        Assert.assertTrue(sequenceManagerSource.contains("CREATE SEQUENCE "));
        Assert.assertTrue(sequenceManagerSource.contains("START WITH "));
        Assert.assertTrue(sequenceManagerSource.contains("INCREMENT BY "));

        String procedureManagerSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/edit/ScratchBirdProcedureManager.java");
        Assert.assertTrue(procedureManagerSource.contains("DBSProcedureType.PROCEDURE"));
        Assert.assertTrue(procedureManagerSource.contains("Create procedure"));
        Assert.assertTrue(procedureManagerSource.contains("defaultProcedureSource(procedure)"));

        String triggerManagerSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/edit/ScratchBirdTableTriggerManager.java");
        Assert.assertTrue(triggerManagerSource.contains("ScratchBirdDraftTableTrigger"));
        Assert.assertTrue(triggerManagerSource.contains("defaultTableTriggerSource(trigger)"));
        Assert.assertTrue(triggerManagerSource.contains("Create trigger"));

        String viewManagerSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/edit/ScratchBirdViewManager.java");
        Assert.assertTrue(viewManagerSource.contains("CREATE VIEW "));
        Assert.assertTrue(viewManagerSource.contains("AS SELECT 1 AS sample"));
    }

    @Test
    public void schemaTreeBuilderBuildsHierarchyAndPreservesParentScopedUniqueness() {
        List<ScratchBirdSchemaTreeBuilder.Node> roots = ScratchBirdSchemaTreeBuilder.build(Arrays.asList(
            "sys",
            "users.alice.dev",
            "users.alice.prod",
            "users.bob.dev",
            "users.bob.dev",
            "analytics.dev",
            "analytics.prod"));

        Assert.assertEquals(4, roots.size());

        ScratchBirdSchemaTreeBuilder.Node users = findNodeByName(roots, "users");
        Assert.assertNotNull(users);

        ScratchBirdSchemaTreeBuilder.Node alice = findNodeByName(users.getChildren(), "alice");
        ScratchBirdSchemaTreeBuilder.Node bob = findNodeByName(users.getChildren(), "bob");
        Assert.assertNotNull(alice);
        Assert.assertNotNull(bob);

        Assert.assertNotNull(findNodeByName(alice.getChildren(), "dev"));
        Assert.assertNotNull(findNodeByName(alice.getChildren(), "prod"));
        Assert.assertNotNull(findNodeByName(bob.getChildren(), "dev"));
        Assert.assertEquals(1, bob.getChildren().size());

        ScratchBirdSchemaTreeBuilder.Node sys = findNodeByName(roots, "sys");
        Assert.assertNotNull(sys);
        Assert.assertTrue(sys.isTerminal());
        Assert.assertTrue(sys.getChildren().isEmpty());

        ScratchBirdSchemaTreeBuilder.Node metrics = findNodeByName(roots, "metrics");
        Assert.assertNull(metrics);
    }

    @Test
    public void schemaTreeBuilderCarriesCatalogUuidIdentityAndInfersParentage() {
        List<ScratchBirdSchemaTreeBuilder.Node> roots = ScratchBirdSchemaTreeBuilder.buildFromCatalog(List.of(
            ScratchBirdCatalogObjectReference.schema(
                "db-uuid",
                "sys-uuid",
                null,
                "sys",
                "sys"),
            ScratchBirdCatalogObjectReference.schema(
                "db-uuid",
                "users-uuid",
                null,
                "users",
                "users"),
            ScratchBirdCatalogObjectReference.schema(
                "db-uuid",
                "public-uuid",
                null,
                "users.public",
                "public"),
            ScratchBirdCatalogObjectReference.schema(
                "db-uuid",
                "data-uuid",
                null,
                "data",
                "data"),
            ScratchBirdCatalogObjectReference.schema(
                "db-uuid",
                "app-uuid",
                "data-uuid",
                "data.application",
                "application")));

        ScratchBirdSchemaTreeBuilder.Node sys = findNodeByName(roots, "sys");
        Assert.assertNotNull(sys);
        Assert.assertEquals("sys-uuid", sys.getObjectPath().objectUuid());
        Assert.assertEquals("db-uuid", sys.getObjectPath().parentUuid());

        ScratchBirdSchemaTreeBuilder.Node users = findNodeByName(roots, "users");
        Assert.assertNotNull(users);
        ScratchBirdSchemaTreeBuilder.Node publicNode = findNodeByName(users.getChildren(), "public");
        Assert.assertNotNull(publicNode);
        Assert.assertEquals("public-uuid", publicNode.getObjectPath().objectUuid());
        Assert.assertEquals("users-uuid", publicNode.getObjectPath().parentUuid());

        ScratchBirdSchemaTreeBuilder.Node data = findNodeByName(roots, "data");
        Assert.assertNotNull(data);
        ScratchBirdSchemaTreeBuilder.Node app = findNodeByName(data.getChildren(), "application");
        Assert.assertNotNull(app);
        Assert.assertEquals("app-uuid", app.getObjectPath().objectUuid());
        Assert.assertEquals("data-uuid", app.getObjectPath().parentUuid());
    }

    @Test
    public void schemaTreeBuilderOrdersRootsAndChildrenDeterministically() {
        List<ScratchBirdSchemaTreeBuilder.Node> roots = ScratchBirdSchemaTreeBuilder.build(Arrays.asList(
            "users.zeta",
            "analytics.prod",
            "users.alpha",
            "cluster.cluster",
            "connections.connections",
            "remote.links",
            "sys.security",
            "analytics.dev"));

        Assert.assertEquals(
            Arrays.asList("sys", "users", "cluster", "remote", "analytics", "connections"),
            roots.stream().map(ScratchBirdSchemaTreeBuilder.Node::getName).toList());

        ScratchBirdSchemaTreeBuilder.Node analytics = findNodeByName(roots, "analytics");
        Assert.assertNotNull(analytics);
        Assert.assertEquals(
            Arrays.asList("dev", "prod"),
            analytics.getChildren().stream().map(ScratchBirdSchemaTreeBuilder.Node::getName).toList());

        ScratchBirdSchemaTreeBuilder.Node users = findNodeByName(roots, "users");
        Assert.assertNotNull(users);
        Assert.assertEquals(
            Arrays.asList("alpha", "zeta"),
            users.getChildren().stream().map(ScratchBirdSchemaTreeBuilder.Node::getName).toList());

        Assert.assertNull(findNodeByName(roots, "metrics"));
    }

    @Test
    public void namespaceSemanticsIdentifySystemRootsAndDepth() {
        Assert.assertTrue(ScratchBirdNamespaceSemantics.isSystemPath("sys"));
        Assert.assertTrue(ScratchBirdNamespaceSemantics.isSystemPath("sys.security.users"));
        Assert.assertTrue(ScratchBirdNamespaceSemantics.isManagementPath("remote.links"));
        Assert.assertTrue(ScratchBirdNamespaceSemantics.isManagementPath("cluster.cluster"));
        Assert.assertFalse(ScratchBirdNamespaceSemantics.isSystemPath("users.public"));
        Assert.assertFalse(ScratchBirdNamespaceSemantics.isManagementPath("users.public"));

        Assert.assertEquals("users", ScratchBirdNamespaceSemantics.getRootSegment("users.public"));
        Assert.assertEquals(3, ScratchBirdNamespaceSemantics.getPathDepth("sys.security.users"));
        Assert.assertTrue(ScratchBirdNamespaceSemantics.comparePaths("sys.security", "users.public") < 0);
        Assert.assertTrue(ScratchBirdNamespaceSemantics.comparePaths("users.public", "cluster.cluster") < 0);
        Assert.assertTrue(ScratchBirdNamespaceSemantics.comparePaths("metrics.alerts", "analytics.dev") < 0);
    }

    @Test
    public void navigatorActionRegistryRoutesMetricsDomainsAndCatalogBranches() {
        List<ScratchBirdSchemaTreeBuilder.Node> roots = ScratchBirdSchemaTreeBuilder.build(List.of("sys.domains", "data.app"), true);

        ScratchBirdSchemaTreeBuilder.Node metrics = findNodeByName(roots, "metrics");
        Assert.assertNotNull(metrics);
        Assert.assertEquals(
            Arrays.asList(
                ScratchBirdNavigatorActionRegistry.Action.OPEN,
                ScratchBirdNavigatorActionRegistry.Action.PROPERTIES,
                ScratchBirdNavigatorActionRegistry.Action.REPORTS,
                ScratchBirdNavigatorActionRegistry.Action.REFRESH,
                ScratchBirdNavigatorActionRegistry.Action.SOURCE_STATUS),
            ScratchBirdNavigatorActionRegistry.actionsFor(metrics));

        ScratchBirdSchemaTreeBuilder.Node sys = findNodeByName(roots, "sys");
        Assert.assertNotNull(sys);
        ScratchBirdSchemaTreeBuilder.Node domains = findNodeByName(sys.getChildren(), "domains");
        Assert.assertNotNull(domains);
        Assert.assertEquals(
            Arrays.asList(
                ScratchBirdNavigatorActionRegistry.Action.NEW,
                ScratchBirdNavigatorActionRegistry.Action.PROPERTIES,
                ScratchBirdNavigatorActionRegistry.Action.ALTER,
                ScratchBirdNavigatorActionRegistry.Action.DELETE,
                ScratchBirdNavigatorActionRegistry.Action.REFRESH),
            ScratchBirdNavigatorActionRegistry.actionsFor(domains));

        ScratchBirdSchemaTreeBuilder.Node data = findNodeByName(roots, "data");
        Assert.assertNotNull(data);
        Assert.assertTrue(ScratchBirdNavigatorActionRegistry.actionsFor(data)
            .contains(ScratchBirdNavigatorActionRegistry.Action.REPORTS));

        List<ScratchBirdNavigatorActionRegistry.Action> catalogActions = ScratchBirdNavigatorActionRegistry.actionsForPath(
            "sys.catalog.object_resolver",
            false,
            true);
        Assert.assertFalse(catalogActions.contains(ScratchBirdNavigatorActionRegistry.Action.NEW));
        Assert.assertFalse(catalogActions.contains(ScratchBirdNavigatorActionRegistry.Action.ALTER));
        Assert.assertTrue(catalogActions.contains(ScratchBirdNavigatorActionRegistry.Action.TASKS));
        Assert.assertTrue(catalogActions.contains(ScratchBirdNavigatorActionRegistry.Action.SOURCE_STATUS));

        List<ScratchBirdNavigatorActionRegistry.Action> userCollectionActions = ScratchBirdNavigatorActionRegistry.actionsForPath(
            "users.home",
            false,
            true);
        Assert.assertTrue(userCollectionActions.contains(ScratchBirdNavigatorActionRegistry.Action.NEW));
        Assert.assertFalse(userCollectionActions.contains(ScratchBirdNavigatorActionRegistry.Action.ALTER));

        List<ScratchBirdNavigatorActionRegistry.Action> userEntityActions = ScratchBirdNavigatorActionRegistry.actionsForPath(
            "users.home.alice",
            false,
            true);
        Assert.assertTrue(userEntityActions.contains(ScratchBirdNavigatorActionRegistry.Action.ALTER));
        Assert.assertTrue(userEntityActions.contains(ScratchBirdNavigatorActionRegistry.Action.DELETE));
        Assert.assertTrue(userEntityActions.contains(ScratchBirdNavigatorActionRegistry.Action.TASKS));

        List<ScratchBirdNavigatorActionRegistry.Action> remoteLinkActions = ScratchBirdNavigatorActionRegistry.actionsForPath(
            "remote.links",
            false,
            true);
        Assert.assertTrue(remoteLinkActions.contains(ScratchBirdNavigatorActionRegistry.Action.NEW));
        Assert.assertTrue(remoteLinkActions.contains(ScratchBirdNavigatorActionRegistry.Action.TASKS));
    }

    @Test
    public void branchProfilesRouteAdministrativeSurfaces() {
        ScratchBirdBranchProfile sysCatalog = ScratchBirdBranchProfile.forPath("sys.catalog.object_resolver");
        Assert.assertEquals("SBDV-FRM-107", sysCatalog.formId());
        Assert.assertTrue(sysCatalog.inspectOnly());
        Assert.assertFalse(sysCatalog.createAllowed());

        ScratchBirdBranchProfile sysDomains = ScratchBirdBranchProfile.forPath("sys.domains");
        Assert.assertEquals("SBDV-FRM-611", sysDomains.formId());
        Assert.assertTrue(sysDomains.createAllowed());
        Assert.assertTrue(sysDomains.deleteAllowed());

        ScratchBirdBranchProfile userProfile = ScratchBirdBranchProfile.forPath("users.home.alice.profiles.local");
        Assert.assertEquals("SBDV-FRM-202", userProfile.formId());
        Assert.assertTrue(userProfile.focusFields().contains("profile scope"));

        ScratchBirdBranchProfile remotePolicy = ScratchBirdBranchProfile.forPath("remote.postgresql.erp.mapping");
        Assert.assertEquals("SBDV-FRM-503", remotePolicy.formId());
        Assert.assertTrue(remotePolicy.createAllowed());
        Assert.assertTrue(remotePolicy.taskAllowed());
    }

    @Test
    public void formRegistryCoversLifecycleReportsAndServerGuardedExecution() {
        Assert.assertTrue(ScratchBirdFormRegistry.allForms().size() >= 58);

        ScratchBirdFormDefinition namespaceForm = ScratchBirdFormRegistry.require("SBDV-FRM-001");
        Assert.assertTrue(namespaceForm.mustFields().contains("database UUID"));
        Assert.assertTrue(namespaceForm.mustFields().contains("parent UUID"));
        Assert.assertTrue(namespaceForm.childForms().contains("SBDV-FRM-014"));

        ScratchBirdFormDefinition domainForm = ScratchBirdFormRegistry.resolveForPath(
            "sys.domains",
            ScratchBirdNavigatorActionRegistry.Action.NEW);
        Assert.assertEquals("SBDV-FRM-611", domainForm.id());
        Assert.assertTrue(domainForm.mustFields().contains("parent schema UUID"));
        Assert.assertTrue(domainForm.childForms().contains("SBDV-FRM-015"));

        ScratchBirdFormDefinition deleteForm = ScratchBirdFormRegistry.resolveForPath(
            "data.application",
            ScratchBirdNavigatorActionRegistry.Action.DELETE);
        Assert.assertEquals("SBDV-FRM-014", deleteForm.id());

        ScratchBirdFormDefinition catalogForm = ScratchBirdFormRegistry.resolveForPath(
            "sys.catalog.object_resolver",
            ScratchBirdNavigatorActionRegistry.Action.PROPERTIES);
        Assert.assertEquals("SBDV-FRM-107", catalogForm.id());

        ScratchBirdRefusalModel rejectedDelete = ScratchBirdPermissionProbe.probe(
            domainForm,
            ScratchBirdFormMode.DELETE,
            "sys.domains");
        Assert.assertEquals(ScratchBirdRefusalModel.Kind.CLIENT_GATED, rejectedDelete.kind());

        ScratchBirdRefusalModel admittedDelete = ScratchBirdPermissionProbe.probe(
            deleteForm,
            ScratchBirdFormMode.DELETE,
            "data.application");
        Assert.assertEquals(ScratchBirdRefusalModel.Kind.SERVER_ADMISSION_REQUIRED, admittedDelete.kind());
        Assert.assertTrue(admittedDelete.allowsServerProbe());

        ScratchBirdRefusalModel deniedCatalogCreate = ScratchBirdPermissionProbe.probe(
            catalogForm,
            ScratchBirdFormMode.CREATE,
            "sys.catalog.object_resolver");
        Assert.assertEquals(ScratchBirdRefusalModel.Kind.PERMISSION_DENIED, deniedCatalogCreate.kind());

        ScratchBirdFormDefinition taskForm = ScratchBirdFormRegistry.resolveForPath(
            "sys.jobs",
            ScratchBirdNavigatorActionRegistry.Action.TASKS);
        Assert.assertEquals("SBDV-FRM-016", taskForm.id());

        ScratchBirdRefusalModel deniedInternalsTask = ScratchBirdPermissionProbe.probe(
            taskForm,
            ScratchBirdFormMode.TASK,
            "sys.internals");
        Assert.assertEquals(ScratchBirdRefusalModel.Kind.PERMISSION_DENIED, deniedInternalsTask.kind());

        ScratchBirdRefusalModel metricsTask = ScratchBirdPermissionProbe.probe(
            taskForm,
            ScratchBirdFormMode.TASK,
            "metrics.alerts");
        Assert.assertEquals(ScratchBirdRefusalModel.Kind.UNSUPPORTED, metricsTask.kind());

        ScratchBirdRefusalModel futureReport = ScratchBirdPermissionProbe.probe(
            ScratchBirdFormRegistry.require("SBDV-FRM-903"),
            ScratchBirdFormMode.REPORT,
            "metrics.future-gated.SBDV-RPT-FUTURE-001");
        Assert.assertEquals(ScratchBirdRefusalModel.Kind.MISSING_SOURCE, futureReport.kind());

        ScratchBirdAdminExecutor.ExecutionPlan createPlan = ScratchBirdAdminExecutor.plan(
            domainForm,
            ScratchBirdFormMode.CREATE,
            "sys.domains");
        Assert.assertTrue(createPlan.executable());
        Assert.assertFalse(createPlan.destructive());
        Assert.assertTrue(createPlan.commandText().contains("CREATE DOMAIN sys.domains.new_domain"));

        ScratchBirdAdminExecutor.ExecutionPlan deletePlan = ScratchBirdAdminExecutor.plan(
            deleteForm,
            ScratchBirdFormMode.DELETE,
            "data.application");
        Assert.assertTrue(deletePlan.executable());
        Assert.assertTrue(deletePlan.destructive());
        Assert.assertTrue(deletePlan.commandText().contains("DROP SCHEMA data.application"));

        ScratchBirdDestructivePlan destructivePlan = ScratchBirdDestructivePlan.forTarget(
            "data.application",
            deletePlan.commandText());
        Assert.assertEquals("DELETE data.application", destructivePlan.confirmationPhrase());
        Assert.assertTrue(destructivePlan.dependencyPreview().get(0).contains("sys.catalog.object_resolver"));
        Assert.assertTrue(destructivePlan.dryRunCommands().get(0).contains("sys.server_capabilities"));
        Assert.assertFalse(destructivePlan.dryRunCommands().get(0).contains("SHOW MANAGEMENT CAPABILITIES"));

        List<ScratchBirdTaskDefinition> tasks = ScratchBirdTaskCatalog.tasksFor("sys.jobs");
        Assert.assertEquals(2, tasks.size());
        Assert.assertEquals("Scheduler Health Snapshot", tasks.get(0).title());
        Assert.assertTrue(tasks.get(0).precheckCommand().contains("sys.server_capabilities"));
        Assert.assertFalse(tasks.get(0).precheckCommand().contains("SHOW MANAGEMENT CAPABILITIES"));
        Assert.assertTrue(tasks.get(0).commandMatrix().stream().anyMatch(line -> line.contains("SELECT * FROM sys.jobs")));

        ScratchBirdAdminExecutor.ExecutionPlan taskPlan = ScratchBirdAdminExecutor.plan(
            taskForm,
            ScratchBirdFormMode.TASK,
            "sys.jobs");
        Assert.assertTrue(taskPlan.executable());
        Assert.assertTrue(taskPlan.commandText().contains("SELECT * FROM sys.job_runs ORDER BY started_at DESC LIMIT 50"));

        ScratchBirdAdminExecutor.ExecutionPlan jobsCreatePlan = ScratchBirdAdminExecutor.plan(
            ScratchBirdFormRegistry.require("SBDV-FRM-102"),
            ScratchBirdFormMode.CREATE,
            "sys.jobs");
        Assert.assertTrue(jobsCreatePlan.commandText(), ScratchBirdV3Parser.parse(jobsCreatePlan.commandText()).success());

        ScratchBirdAdminExecutor.ExecutionPlan clusterInspectPlan = ScratchBirdAdminExecutor.plan(
            ScratchBirdFormRegistry.require("SBDV-FRM-104"),
            ScratchBirdFormMode.INSPECT,
            "sys.cluster");
        Assert.assertTrue(clusterInspectPlan.commandText().contains("SHOW CLUSTER ADMISSION STATUS"));
        Assert.assertTrue(clusterInspectPlan.commandText(), ScratchBirdV3Parser.parse(clusterInspectPlan.commandText()).success());

        ScratchBirdAdminExecutor.ExecutionPlan remoteCreatePlan = ScratchBirdAdminExecutor.plan(
            ScratchBirdFormRegistry.require("SBDV-FRM-500"),
            ScratchBirdFormMode.CREATE,
            "remote.links");
        Assert.assertTrue(remoteCreatePlan.commandText(), ScratchBirdV3Parser.parse(remoteCreatePlan.commandText()).success());

        ScratchBirdAdminExecutor.ExecutionPlan indexCreatePlan = ScratchBirdAdminExecutor.plan(
            ScratchBirdFormRegistry.require("SBDV-FRM-607"),
            ScratchBirdFormMode.CREATE,
            "data.application.orders");
        Assert.assertTrue(indexCreatePlan.commandText(), indexCreatePlan.commandText().contains("CREATE INDEX idx_orders ON data.application.orders"));

        ScratchBirdAdminExecutor.ExecutionPlan triggerAlterPlan = ScratchBirdAdminExecutor.plan(
            ScratchBirdFormRegistry.require("SBDV-FRM-610"),
            ScratchBirdFormMode.ALTER,
            "data.application.orders_bi");
        Assert.assertTrue(triggerAlterPlan.commandText(), ScratchBirdV3Parser.parse(triggerAlterPlan.commandText()).success());
    }

    @Test
    public void formPanelCatalogBuildsContextSpecificAdminLifecycleAndDatatypePanels() {
        ScratchBirdFormDefinition jobsForm = ScratchBirdFormRegistry.require("SBDV-FRM-102");
        List<ScratchBirdTaskDefinition> jobsTasks = ScratchBirdTaskCatalog.tasksFor("sys.jobs");
        ScratchBirdAdminExecutor.ExecutionPlan jobsPlan = ScratchBirdAdminExecutor.plan(
            jobsForm,
            ScratchBirdFormMode.ALTER,
            "sys.jobs");
        ScratchBirdLiveProbe.ProbePlan jobsAuthzProbe = ScratchBirdPermissionProbe.planServerAuthorization(
            jobsForm,
            ScratchBirdFormMode.ALTER,
            "sys.jobs",
            jobsPlan,
            jobsTasks,
            null);
        ScratchBirdLiveProbe.ProbePlan jobsLiveProbe = ScratchBirdLiveProbe.plan(
            jobsForm,
            ScratchBirdFormMode.ALTER,
            "sys.jobs",
            jobsPlan,
            jobsTasks,
            null);

        List<ScratchBirdFormPanelCatalog.Panel> jobsPanels = ScratchBirdFormPanelCatalog.panelsFor(
            jobsForm,
            ScratchBirdFormMode.ALTER,
            "sys.jobs",
            ScratchBirdPermissionProbe.probe(jobsForm, ScratchBirdFormMode.ALTER, "sys.jobs"),
            jobsPlan,
            jobsAuthzProbe,
            jobsLiveProbe,
            jobsTasks,
            null,
            null);

        ScratchBirdFormPanelCatalog.Panel adminPanel = requirePanel(jobsPanels, "admin-surface");
        Assert.assertEquals("Admin", adminPanel.tabLabel());
        Assert.assertTrue(panelValue(adminPanel, "Branch profile").contains("SBBP-SYS-JOBS"));
        Assert.assertTrue(panelValue(adminPanel, "Action set").contains("TASKS"));
        Assert.assertTrue(panelValue(adminPanel, "Server authz probe").contains("ready"));

        ScratchBirdFormPanelCatalog.Panel toolingPanel = requirePanel(jobsPanels, "admin-tooling");
        Assert.assertTrue(panelValue(toolingPanel, "Primary task").contains("Scheduler Health Snapshot"));
        Assert.assertTrue(panelValue(toolingPanel, "Primary task result surfaces").contains("sys.jobs"));
        Assert.assertTrue(panelValue(toolingPanel, "Default preview").contains("ALTER JOB"));

        ScratchBirdFormDefinition fieldForm = ScratchBirdFormRegistry.require("SBDV-FRM-605");
        List<ScratchBirdTaskDefinition> fieldTasks = ScratchBirdTaskCatalog.tasksFor("data.application.orders.payload");
        ScratchBirdAdminExecutor.ExecutionPlan fieldPlan = ScratchBirdAdminExecutor.plan(
            fieldForm,
            ScratchBirdFormMode.ALTER,
            "data.application.orders.payload");
        ScratchBirdLiveProbe.ProbePlan fieldAuthzProbe = ScratchBirdPermissionProbe.planServerAuthorization(
            fieldForm,
            ScratchBirdFormMode.ALTER,
            "data.application.orders.payload",
            fieldPlan,
            fieldTasks,
            null);
        ScratchBirdLiveProbe.ProbePlan fieldLiveProbe = ScratchBirdLiveProbe.plan(
            fieldForm,
            ScratchBirdFormMode.ALTER,
            "data.application.orders.payload",
            fieldPlan,
            fieldTasks,
            null);

        List<ScratchBirdFormPanelCatalog.Panel> fieldPanels = ScratchBirdFormPanelCatalog.panelsFor(
            fieldForm,
            ScratchBirdFormMode.ALTER,
            "data.application.orders.payload",
            ScratchBirdPermissionProbe.probe(fieldForm, ScratchBirdFormMode.ALTER, "data.application.orders.payload"),
            fieldPlan,
            fieldAuthzProbe,
            fieldLiveProbe,
            fieldTasks,
            null,
            typedObject("JSONB"));

        ScratchBirdFormPanelCatalog.Panel lifecyclePanel = requirePanel(fieldPanels, "lifecycle");
        Assert.assertEquals("Lifecycle", lifecyclePanel.tabLabel());
        Assert.assertTrue(panelValue(lifecyclePanel, "Object family").contains("column or field"));
        Assert.assertTrue(panelValue(lifecyclePanel, "Validation attachment").contains("SBDV-FRM-015"));
        Assert.assertTrue(panelValue(lifecyclePanel, "Delete attachment").contains("SBDV-FRM-014"));

        ScratchBirdFormPanelCatalog.Panel datatypePanel = requirePanel(fieldPanels, "datatype");
        Assert.assertEquals("Datatype", datatypePanel.tabLabel());
        Assert.assertTrue(panelValue(datatypePanel, "Widget strategy").contains("Structured payload editor"));
        Assert.assertTrue(panelValue(datatypePanel, "Must controls").contains("structured payload field"));
        Assert.assertTrue(panelValue(datatypePanel, "Should controls").contains("pretty formatter"));
        Assert.assertTrue(panelValue(datatypePanel, "Example literal").contains("CAST('{\"sample\":true}' AS JSONB)"));
    }

    @Test
    public void editorPageCatalogCoversEveryRegisteredFormWithSpecificPages() throws Exception {
        for (ScratchBirdFormDefinition form : ScratchBirdFormRegistry.allForms()) {
            ScratchBirdFormMode mode = form.modes().iterator().next();
            ScratchBirdEditorPageCatalog.EditorPlan plan = ScratchBirdEditorPageCatalog.planFor(
                form,
                mode,
                sampleTargetPathFor(form));
            Assert.assertFalse(form.id(), plan.pages().isEmpty());
            Assert.assertTrue(form.id(), plan.pages().stream().anyMatch(page -> page.id().equals("identity")));
            Assert.assertTrue(form.id(), plan.pages().stream().anyMatch(page -> page.id().startsWith("primary-")));
            Assert.assertTrue(form.id(), plan.pages().stream().anyMatch(page -> page.id().startsWith("validation-")));
            Assert.assertTrue(form.id(), plan.pages().stream().anyMatch(page -> page.id().startsWith("authority-")));
        }

        ScratchBirdEditorPageCatalog.EditorPlan relationalTablePlan = ScratchBirdEditorPageCatalog.planFor(
            ScratchBirdFormRegistry.require("SBDV-FRM-601"),
            ScratchBirdFormMode.ALTER,
            "data.sales.orders");
        Assert.assertTrue(relationalTablePlan.pages().stream().anyMatch(page -> page.title().contains("Relational table Lifecycle Page")));
        Assert.assertTrue(relationalTablePlan.pages().stream().flatMap(page -> page.controls().stream()).anyMatch(control -> control.contains("definition editor")));

        ScratchBirdEditorPageCatalog.EditorPlan jobsPlan = ScratchBirdEditorPageCatalog.planFor(
            ScratchBirdFormRegistry.require("SBDV-FRM-102"),
            ScratchBirdFormMode.CREATE,
            "sys.jobs");
        Assert.assertTrue(jobsPlan.pages().stream().anyMatch(page -> page.evidenceAnchors().contains("sys.security.permission_probe")));
        Assert.assertTrue(jobsPlan.pages().stream().flatMap(page -> page.validationWidgets().stream()).anyMatch(widget -> widget.contains("mutation admission probe")));

        ScratchBirdEditorPageCatalog.EditorPlan metricsPlan = ScratchBirdEditorPageCatalog.planFor(
            ScratchBirdFormRegistry.require("SBDV-FRM-903"),
            ScratchBirdFormMode.REPORT,
            "metrics.workload-and-sql.SBDV-RPT-CORE-003");
        Assert.assertTrue(metricsPlan.pages().stream().anyMatch(page -> page.title().contains("Metrics, Report, And Alert Page")));
        Assert.assertTrue(metricsPlan.pages().stream().flatMap(page -> page.controls().stream()).anyMatch(control -> control.contains("chart intent")));

        ScratchBirdEditorPageCatalog.EditorPlan valuePlan = ScratchBirdEditorPageCatalog.planFor(
            ScratchBirdFormRegistry.require("SBDV-FRM-613"),
            ScratchBirdFormMode.ALTER,
            "data.sales.orders.payload");
        Assert.assertTrue(valuePlan.pages().stream().anyMatch(page -> page.title().contains("Datatype And Value Widget Page")));
        Assert.assertTrue(valuePlan.pages().stream().flatMap(page -> page.validationWidgets().stream()).anyMatch(widget -> widget.contains("roundtrip status")));

        String pageCatalogSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdEditorPageCatalog.java");
        Assert.assertTrue(pageCatalogSource.contains("record EditorPage"));
        Assert.assertTrue(pageCatalogSource.contains("Object Identity Page"));
        Assert.assertTrue(pageCatalogSource.contains("mutation admission probe"));
        Assert.assertTrue(pageCatalogSource.contains("Metrics, Report, And Alert Page"));

        String managementDialogSource = readUiSource("org/jkiss/dbeaver/ext/scratchbird/ui/ScratchBirdManagementDialog.java");
        Assert.assertTrue(managementDialogSource.contains("ScratchBirdEditorPageCatalog.planFor"));
        Assert.assertTrue(managementDialogSource.contains("createEditorPages"));
        Assert.assertTrue(managementDialogSource.contains("appendEditorPages"));
        Assert.assertTrue(managementDialogSource.contains("Object-specific editor pages"));
    }

    @Test
    public void reportCatalogBuildsMetricsTreeLeavesAndSourceStatus() {
        Assert.assertEquals(52, ScratchBirdReportCatalog.allReports().size());
        Assert.assertEquals(
            1 + ScratchBirdReportCatalog.METRICS_BRANCHES.size() + ScratchBirdReportCatalog.allReports().size(),
            ScratchBirdReportCatalog.metricTreePaths().size());
        Assert.assertTrue(ScratchBirdReportCatalog.metricTreePaths()
            .contains("metrics.health-scorecards.SBDV-RPT-CORE-001"));
        Assert.assertTrue(ScratchBirdReportCatalog.metricTreePaths()
            .contains("metrics.alerts.SBDV-ALERT-016"));
        Assert.assertTrue(ScratchBirdReportCatalog.metricTreePaths()
            .contains("metrics.future-gated.SBDV-RPT-FUTURE-007"));
        ScratchBirdReportDefinition health = ScratchBirdReportCatalog.findByNavigatorPath(
            "metrics.health-scorecards.SBDV-RPT-CORE-001");
        Assert.assertNotNull(health);
        ScratchBirdReportPlan healthPlan = ScratchBirdReportPlan.forReport(health);
        Assert.assertTrue(healthPlan.scriptPreview().contains("SHOW METRICS"));
        Assert.assertFalse(healthPlan.scriptPreview().contains("SHOW METRICS WHERE"));
        Assert.assertFalse(healthPlan.scriptPreview().contains("SHOW STATUS LIKE"));
        Assert.assertFalse(healthPlan.scriptPreview().contains("sys.performance"));

        ScratchBirdReportDefinition latency = ScratchBirdReportCatalog.findByNavigatorPath(
            "metrics.workload-and-sql.SBDV-RPT-CORE-003");
        Assert.assertNotNull(latency);
        Assert.assertEquals("SBDV-FRM-903", latency.parentForm());
        Assert.assertEquals(
            "SBDV-FRM-903",
            ScratchBirdFormRegistry.resolveForPath(latency.navigatorPath(), ScratchBirdNavigatorActionRegistry.Action.REPORTS).id());
        ScratchBirdRefusalModel latencyStatus = ScratchBirdMetricSourceResolver.sourceStatus(latency);
        Assert.assertEquals(ScratchBirdRefusalModel.Kind.ADMITTED, latencyStatus.kind());
        Assert.assertTrue(latencyStatus.message().contains("raw histogram"));
        ScratchBirdReportPlan latencyPlan = ScratchBirdReportPlan.forReport(latency);
        Assert.assertTrue(latencyPlan.scriptPreview().contains("SHOW METRICS"));
        Assert.assertEquals(1, countOccurrences(latencyPlan.scriptPreview(), "SHOW METRICS"));
        Assert.assertTrue(latencyPlan.drilldownFields().contains("percentile selector"));
        Assert.assertTrue(latencyPlan.alertExpressionStarter().contains("query latency breach"));
        Assert.assertEquals(
            5,
            ScratchBirdReportCatalog.reportsForNavigatorPath("metrics.workload-and-sql").stream()
                .filter(report -> report.branch().equals("workload-and-sql"))
                .count());
        Assert.assertEquals(16, ScratchBirdReportCatalog.reportsForNavigatorPath("metrics.alerts").size());
        ScratchBirdReportDefinition latencyAlert = ScratchBirdReportCatalog.findByNavigatorPath(
            "metrics.alerts.SBDV-ALERT-001");
        Assert.assertNotNull(latencyAlert);
        Assert.assertEquals("Query Latency Breach", latencyAlert.title());
        ScratchBirdReportPlan alertPlan = ScratchBirdReportPlan.forReport(latencyAlert);
        Assert.assertTrue(alertPlan.scriptPreview().contains("SHOW METRICS"));
        Assert.assertFalse(alertPlan.scriptPreview().contains("SHOW METRICS WHERE"));
        Assert.assertTrue(alertPlan.summaryLines().stream().anyMatch(line -> line.contains("threshold")));

        ScratchBirdReportDefinition future = ScratchBirdReportCatalog.findByNavigatorPath(
            "metrics.future-gated.SBDV-RPT-FUTURE-001");
        Assert.assertNotNull(future);
        ScratchBirdRefusalModel futureStatus = ScratchBirdMetricSourceResolver.sourceStatus(future);
        Assert.assertEquals(ScratchBirdRefusalModel.Kind.MISSING_SOURCE, futureStatus.kind());
        Assert.assertTrue(futureStatus.message().contains("Future-gated"));
        Assert.assertTrue(ScratchBirdReportPlan.forReport(future).scriptPreview().contains("Future gated"));
    }

    @Test
    public void schemaTreeBuilderPublishesReportLeavesUnderClientOnlyMetricsRoot() {
        List<ScratchBirdSchemaTreeBuilder.Node> roots = ScratchBirdSchemaTreeBuilder.build(List.of(
            "sys",
            "data.application"), true);

        ScratchBirdSchemaTreeBuilder.Node metrics = findNodeByName(roots, "metrics");
        Assert.assertNotNull(metrics);
        Assert.assertTrue(metrics.isClientOnly());
        Assert.assertFalse(metrics.isCatalogBacked());

        ScratchBirdSchemaTreeBuilder.Node health = findNodeByName(metrics.getChildren(), "health-scorecards");
        Assert.assertNotNull(health);
        Assert.assertTrue(health.isClientOnly());
        Assert.assertFalse(health.isCatalogBacked());
        ScratchBirdSchemaTreeBuilder.Node executive = findNodeByName(health.getChildren(), "SBDV-RPT-CORE-001");
        Assert.assertNotNull(executive);
        Assert.assertTrue(executive.isTerminal());
        Assert.assertTrue(executive.isClientOnly());
        Assert.assertFalse(executive.isCatalogBacked());

        ScratchBirdSchemaTreeBuilder.Node futureGated = findNodeByName(metrics.getChildren(), "future-gated");
        Assert.assertNotNull(futureGated);
        Assert.assertNotNull(findNodeByName(futureGated.getChildren(), "SBDV-RPT-FUTURE-001"));
    }

    @Test
    public void uiPluginDeclaresContextMenuHandlersAndFeatureBundle() throws IOException {
        String uiPluginXml = readUiPluginXml();
        Assert.assertTrue(uiPluginXml.contains("org.jkiss.dbeaver.ext.scratchbird.ui.open"));
        Assert.assertTrue(uiPluginXml.contains("org.jkiss.dbeaver.ext.scratchbird.ui.new"));
        Assert.assertTrue(uiPluginXml.contains("org.jkiss.dbeaver.ext.scratchbird.ui.alter"));
        Assert.assertTrue(uiPluginXml.contains("org.jkiss.dbeaver.ext.scratchbird.ui.delete"));
        Assert.assertTrue(uiPluginXml.contains("org.jkiss.dbeaver.ext.scratchbird.ui.tasks"));
        Assert.assertTrue(uiPluginXml.contains("org.jkiss.dbeaver.ext.scratchbird.ui.reports"));
        Assert.assertTrue(uiPluginXml.contains("org.jkiss.dbeaver.ext.scratchbird.ui.sourceStatus"));
        Assert.assertTrue(uiPluginXml.contains("org.jkiss.dbeaver.ext.scratchbird.ui.validateSql"));
        Assert.assertTrue(uiPluginXml.contains("org.eclipse.ui.edit.text.contentAssist.proposals"));
        Assert.assertTrue(uiPluginXml.contains("ScratchBirdSqlContentAssistHandler"));
        Assert.assertTrue(uiPluginXml.contains("popup:org.eclipse.ui.popup.any?after=additions"));
        Assert.assertTrue(uiPluginXml.contains("popup:org.jkiss.dbeaver.ui.editors.sql.SQLEditor.EditorContext?after=sql.additions"));
        Assert.assertTrue(uiPluginXml.contains("label=\"ScratchBird\""));
        Assert.assertTrue(uiPluginXml.contains("ScratchBirdUiPropertyTester"));
        Assert.assertTrue(uiPluginXml.contains("org.jkiss.dbeaver.sql.editorAddIns"));
        Assert.assertTrue(uiPluginXml.contains("ScratchBirdSqlEditorAddIn"));
        Assert.assertTrue(uiPluginXml.contains("org.jkiss.dbeaver.sql.quickFixProcessors"));
        Assert.assertTrue(uiPluginXml.contains("ScratchBirdSqlQuickAssistProcessor"));
        Assert.assertTrue(uiPluginXml.contains("org.jkiss.dbeaver.ext.scratchbird.ui.v3Problem"));
        Assert.assertTrue(uiPluginXml.contains("textStylePreferenceValue=\"SQUIGGLES\""));
        Assert.assertTrue(uiPluginXml.contains("canManage,canOpen,canNew,canAlter,canDelete,canTasks,canReports,canSourceStatus,canValidateSql"));
        Assert.assertTrue(uiPluginXml.contains("property=\"org.jkiss.dbeaver.ext.scratchbird.ui.canOpen\""));
        Assert.assertTrue(uiPluginXml.contains("property=\"org.jkiss.dbeaver.ext.scratchbird.ui.canNew\""));
        Assert.assertTrue(uiPluginXml.contains("property=\"org.jkiss.dbeaver.ext.scratchbird.ui.canAlter\""));
        Assert.assertTrue(uiPluginXml.contains("property=\"org.jkiss.dbeaver.ext.scratchbird.ui.canDelete\""));
        Assert.assertTrue(uiPluginXml.contains("property=\"org.jkiss.dbeaver.ext.scratchbird.ui.canTasks\""));
        Assert.assertTrue(uiPluginXml.contains("property=\"org.jkiss.dbeaver.ext.scratchbird.ui.canReports\""));
        Assert.assertTrue(uiPluginXml.contains("property=\"org.jkiss.dbeaver.ext.scratchbird.ui.canSourceStatus\""));
        Assert.assertTrue(uiPluginXml.contains("property=\"org.jkiss.dbeaver.ext.scratchbird.ui.canValidateSql\""));
        Assert.assertTrue(uiPluginXml.contains("ScratchBirdValidateSqlHandler"));
        Assert.assertTrue(uiPluginXml.contains("org.jkiss.dbeaver.ui.editors.sql.SQLEditor"));
        Assert.assertEquals(9, countOccurrences(uiPluginXml, "<visibleWhen"));
        Assert.assertEquals(8, countOccurrences(uiPluginXml, "<count value=\"1\"/>"));

        String commandHandlerSource = readUiSource("org/jkiss/dbeaver/ext/scratchbird/ui/handlers/ScratchBirdNavigatorCommandHandler.java");
        Assert.assertTrue(commandHandlerSource.contains("ScratchBirdSelectionUtils.supportsAction(object, action)"));
        Assert.assertTrue(commandHandlerSource.contains("does not support the"));

        String propertyTesterSource = readUiSource("org/jkiss/dbeaver/ext/scratchbird/ui/ScratchBirdUiPropertyTester.java");
        Assert.assertTrue(propertyTesterSource.contains("case \"canValidateSql\""));
        Assert.assertTrue(propertyTesterSource.contains("\"scratchbird_jdbc\".equalsIgnoreCase"));

        String validationHandlerSource = readUiSource("org/jkiss/dbeaver/ext/scratchbird/ui/handlers/ScratchBirdValidateSqlHandler.java");
        Assert.assertTrue(validationHandlerSource.contains("editor.extractActiveQuery()"));
        Assert.assertTrue(validationHandlerSource.contains("ScratchBirdValidationBridge.diagnosticsFor(target.sql())"));
        Assert.assertTrue(validationHandlerSource.contains("ScratchBirdValidationBridge.statementSummaryFor(target.sql())"));
        Assert.assertTrue(validationHandlerSource.contains("ScratchBirdValidationBridge.lintHintsFor(target.sql(), target.caretOffset())"));
        Assert.assertTrue(validationHandlerSource.contains("ScratchBirdValidationBridge.contextHintsFor(target.sql(), target.caretOffset())"));
        Assert.assertTrue(validationHandlerSource.contains("ScratchBirdValidationBridge.serverProbeHintsFor(target.sql(), target.caretOffset())"));
        Assert.assertTrue(validationHandlerSource.contains("ScratchBirdValidationBridge.formHintsFor(target.sql(), target.caretOffset())"));
        Assert.assertTrue(validationHandlerSource.contains("ScratchBirdV3Parser.completionsAt(target.sql(), target.caretOffset())"));
        Assert.assertTrue(validationHandlerSource.contains("Copy Report"));

        String contentAssistHandlerSource = readUiSource("org/jkiss/dbeaver/ext/scratchbird/ui/handlers/ScratchBirdSqlContentAssistHandler.java");
        Assert.assertTrue(contentAssistHandlerSource.contains("ScratchBirdSqlEditorAddIn.openParserCompletion(editor)"));
        Assert.assertTrue(contentAssistHandlerSource.contains("SourceViewer.CONTENTASSIST_PROPOSALS"));

        String editorAddInSource = readUiSource("org/jkiss/dbeaver/ext/scratchbird/ui/ScratchBirdSqlEditorAddIn.java");
        Assert.assertTrue(editorAddInSource.contains("implements SQLEditorAddIn"));
        Assert.assertTrue(editorAddInSource.contains("document.addDocumentListener(documentListener)"));
        Assert.assertTrue(editorAddInSource.contains("selectionProvider.addSelectionChangedListener(selectionListener)"));
        Assert.assertTrue(editorAddInSource.contains("ScratchBirdV3Parser.parse(sql)"));
        Assert.assertTrue(editorAddInSource.contains("ScratchBirdSqlPromptPlanner.plan"));
        Assert.assertTrue(editorAddInSource.contains("ActionBars.extractStatusLineManager"));
        Assert.assertTrue(editorAddInSource.contains("getSuggestionTextPainter"));
        Assert.assertTrue(editorAddInSource.contains("ContentProposalAdapter"));
        Assert.assertTrue(editorAddInSource.contains("openParserCompletion"));
        Assert.assertTrue(editorAddInSource.contains("openProposalPopup"));
        Assert.assertTrue(editorAddInSource.contains("ScratchBirdSqlPromptPlanner.proposalInsertion"));
        Assert.assertTrue(editorAddInSource.contains("extension.replaceAnnotations(previousAnnotations, newAnnotations)"));
        Assert.assertTrue(editorAddInSource.contains("\"scratchbird_jdbc\".equalsIgnoreCase"));

        String promptPlannerSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdSqlPromptPlanner.java");
        Assert.assertTrue(promptPlannerSource.contains("record PromptPlan"));
        Assert.assertTrue(promptPlannerSource.contains("ScratchBirdV3Parser.completionsAt"));
        Assert.assertTrue(promptPlannerSource.contains("completionCandidates"));
        Assert.assertTrue(promptPlannerSource.contains("proposalInsertion"));
        Assert.assertTrue(promptPlannerSource.contains("Tab/Right accepts the inline prompt."));
        Assert.assertTrue(promptPlannerSource.contains("no parser completions for the current cursor"));

        String validationBridgeSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdValidationBridge.java");
        Assert.assertTrue(validationBridgeSource.contains("statementSummaryFor"));
        Assert.assertTrue(validationBridgeSource.contains("lintHintsFor"));
        Assert.assertTrue(validationBridgeSource.contains("contextHintsFor"));
        Assert.assertTrue(validationBridgeSource.contains("serverProbeHintsFor"));
        Assert.assertTrue(validationBridgeSource.contains("formHintsFor"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L001"));
        Assert.assertTrue(validationBridgeSource.contains("server-controlled surface"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_H001"));
        Assert.assertTrue(validationBridgeSource.contains("Run Authz Probe"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L021"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_P001"));
        Assert.assertTrue(validationBridgeSource.contains("server authz probe route"));
        Assert.assertTrue(validationBridgeSource.contains("derivedProbePath"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_F001"));
        Assert.assertTrue(validationBridgeSource.contains("panel surfaces"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_F003"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_F004"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_H031"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_H032"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L028"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L029"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L030"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L031"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L032"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L033"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L034"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L035"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L036"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L037"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L038"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L039"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L040"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L041"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L042"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L043"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L044"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L045"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L046"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L047"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L048"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L049"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L050"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L051"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L052"));
        Assert.assertTrue(validationBridgeSource.contains("PRS_JV3_L053"));
        Assert.assertTrue(validationBridgeSource.contains("canonical ScratchBird domains live under sys.domains"));
        Assert.assertTrue(validationBridgeSource.contains("domain hub collection surface"));
        Assert.assertTrue(validationBridgeSource.contains("DML mutation against"));
        Assert.assertTrue(validationBridgeSource.contains("client-only metrics surface"));
        Assert.assertTrue(validationBridgeSource.contains("data root collection surface"));
        Assert.assertTrue(validationBridgeSource.contains("fully qualify the DML target"));
        Assert.assertTrue(validationBridgeSource.contains("fully qualify the ON target"));
        Assert.assertTrue(validationBridgeSource.contains("rooted ScratchBird DML target"));
        Assert.assertTrue(validationBridgeSource.contains("rooted ScratchBird securable object"));
        Assert.assertTrue(validationBridgeSource.contains("concrete securable child object"));
        Assert.assertTrue(validationBridgeSource.contains("client-only metrics report surface"));
        Assert.assertTrue(validationBridgeSource.contains("route broad grant posture through sys.security"));
        Assert.assertTrue(validationBridgeSource.contains("dclObjectPaths"));
        Assert.assertTrue(validationBridgeSource.contains("dclPrincipalPaths"));
        Assert.assertTrue(validationBridgeSource.contains("addBatchSemanticLintHints"));
        Assert.assertTrue(validationBridgeSource.contains("isMetadataReadStatement"));
        Assert.assertTrue(validationBridgeSource.contains("isDclObjectClauseBoundary"));
        Assert.assertTrue(validationBridgeSource.contains("public principal collection surface"));
        Assert.assertTrue(validationBridgeSource.contains("MGA commit-bound"));
        Assert.assertTrue(validationBridgeSource.contains("committed catalog baseline"));
        Assert.assertTrue(validationBridgeSource.contains("rooted ScratchBird path"));
        Assert.assertTrue(validationBridgeSource.contains("parser-derived object route preview"));
        Assert.assertTrue(validationBridgeSource.contains("canonical review scope"));
        Assert.assertTrue(validationBridgeSource.contains("admits that route through"));
        Assert.assertTrue(validationBridgeSource.contains("scratch workspace"));
        Assert.assertTrue(validationBridgeSource.contains("group public"));
        Assert.assertTrue(validationBridgeSource.contains("data namespace"));
        Assert.assertTrue(validationBridgeSource.contains("parent surface"));
        Assert.assertTrue(validationBridgeSource.contains("selected target"));

        String quickAssistSource = readUiSource("org/jkiss/dbeaver/ext/scratchbird/ui/ScratchBirdSqlQuickAssistProcessor.java");
        Assert.assertTrue(quickAssistSource.contains("implements IQuickAssistProcessor"));
        Assert.assertTrue(quickAssistSource.contains("ScratchBirdSqlEditorAddIn.ANNOTATION_TYPE.equals"));
        Assert.assertTrue(quickAssistSource.contains("\"DESCRIBE\""));
        Assert.assertTrue(quickAssistSource.contains("\"EXECUTE\""));
        Assert.assertTrue(quickAssistSource.contains("EXECUTE BLOCK AS BEGIN"));
        Assert.assertTrue(quickAssistSource.contains("SWEEP DATABASE"));
        Assert.assertTrue(quickAssistSource.contains("DOC PATH FILTER"));
        Assert.assertTrue(quickAssistSource.contains("TS BUCKET AGG"));
        Assert.assertTrue(quickAssistSource.contains("VECTOR ANN QUERY"));
        Assert.assertTrue(quickAssistSource.contains("ScratchBirdV3Parser.completionsAt"));
        Assert.assertTrue(quickAssistSource.contains("ScratchBird v3: "));
        Assert.assertTrue(quickAssistSource.contains("findReplacementTarget"));

        String managementDialogSource = readUiSource("org/jkiss/dbeaver/ext/scratchbird/ui/ScratchBirdManagementDialog.java");
        Assert.assertTrue(managementDialogSource.contains("new TabFolder(area, SWT.TOP)"));
        Assert.assertTrue(managementDialogSource.contains("createOverviewTab"));
        Assert.assertTrue(managementDialogSource.contains("createWorkflowTab"));
        Assert.assertTrue(managementDialogSource.contains("createScratchBirdPanels"));
        Assert.assertTrue(managementDialogSource.contains("createObjectContextTab"));
        Assert.assertTrue(managementDialogSource.contains("createFieldMatrixTab"));
        Assert.assertTrue(managementDialogSource.contains("createValueTab"));
        Assert.assertTrue(managementDialogSource.contains("createExecutionTab"));
        Assert.assertTrue(managementDialogSource.contains("createMonitoringTab"));
        Assert.assertTrue(managementDialogSource.contains("createObjectGraphTab"));
        Assert.assertTrue(managementDialogSource.contains("createValidationTab"));
        Assert.assertTrue(managementDialogSource.contains("createReportTab"));
        Assert.assertTrue(managementDialogSource.contains("COPY_SCRIPT_ID"));
        Assert.assertTrue(managementDialogSource.contains("Copy Form Packet"));
        Assert.assertTrue(managementDialogSource.contains("Validate Preview"));
        Assert.assertTrue(managementDialogSource.contains("Refresh Server Status"));
        Assert.assertTrue(managementDialogSource.contains("Apply Requires Admission"));
        Assert.assertTrue(managementDialogSource.contains("Apply Admitted Preview"));
        Assert.assertTrue(managementDialogSource.contains("refreshServerStatus"));
        Assert.assertTrue(managementDialogSource.contains("applyAfterAdmission"));
        Assert.assertTrue(managementDialogSource.contains("ScratchBirdMutationApplyExecutor.apply"));
        Assert.assertTrue(managementDialogSource.contains("ScratchBirdProbeHistory.recordApply"));
        Assert.assertTrue(managementDialogSource.contains("ScratchBirdManagementWorkflow.previewIdentity(plan)"));
        Assert.assertTrue(managementDialogSource.contains("ScratchBirdManagementWorkflow.applyGateSummary"));
        Assert.assertTrue(managementDialogSource.contains("ScratchBirdManagementWorkflow.monitoringDashboardLines"));
        Assert.assertTrue(managementDialogSource.contains("ScratchBirdManagementWorkflow.objectGraphLines"));
        Assert.assertTrue(managementDialogSource.contains("setAccessibleName"));
        Assert.assertTrue(managementDialogSource.contains("PROOF_DATA_KEY"));
        Assert.assertTrue(managementDialogSource.contains("ScratchBirdValidationBridge.diagnosticsFor(plan.commandText())"));
        Assert.assertTrue(managementDialogSource.contains("Statement inventory"));
        Assert.assertTrue(managementDialogSource.contains("ScratchBirdValidationBridge.statementSummaryFor(plan.commandText())"));
        Assert.assertTrue(managementDialogSource.contains("ScratchBirdValidationBridge.lintHintsFor(plan.commandText(), plan.commandText().length(), targetPath)"));
        Assert.assertTrue(managementDialogSource.contains("Context hints"));
        Assert.assertTrue(managementDialogSource.contains("ScratchBirdValidationBridge.contextHintsFor(plan.commandText(), plan.commandText().length(), targetPath)"));
        Assert.assertTrue(managementDialogSource.contains("ScratchBirdValidationBridge.serverProbeHintsFor(plan.commandText(), plan.commandText().length(), targetPath)"));
        Assert.assertTrue(managementDialogSource.contains("ScratchBirdValidationBridge.formHintsFor(plan.commandText(), plan.commandText().length(), targetPath)"));
        Assert.assertTrue(managementDialogSource.contains("ScratchBirdValidationBridge.completionHintsFor(plan.commandText(), plan.commandText().length())"));
        Assert.assertTrue(managementDialogSource.contains("ScratchBirdObjectFormContext.fieldsFor(targetObject)"));
        Assert.assertTrue(managementDialogSource.contains("ScratchBirdFormPanelCatalog.panelsFor"));
        Assert.assertTrue(managementDialogSource.contains("ScratchBirdValueBinding.exampleLiteralForType"));
        Assert.assertTrue(managementDialogSource.contains("SBDV-FRM-613"));
        Assert.assertTrue(managementDialogSource.contains("\"Object context\""));
        Assert.assertTrue(managementDialogSource.contains("\"Panel\""));
        Assert.assertTrue(managementDialogSource.contains("ScratchBirdReportPlan.forReport(report)"));
        Assert.assertTrue(managementDialogSource.contains("Report source previews"));
        Assert.assertTrue(managementDialogSource.contains("Report drilldown fields"));
        Assert.assertTrue(managementDialogSource.contains("reviewPacket"));
        Assert.assertTrue(managementDialogSource.contains("appendFormPanels"));
        Assert.assertTrue(managementDialogSource.contains("childFormSummaries"));

        String workflowSource = readUiSource("org/jkiss/dbeaver/ext/scratchbird/ui/ScratchBirdManagementWorkflow.java");
        Assert.assertTrue(workflowSource.contains("DBEAVER-MGMT-015"));
        Assert.assertTrue(workflowSource.contains("DBEAVER-MGMT-022"));
        Assert.assertTrue(workflowSource.contains("DBEAVER-MGMT-032"));
        Assert.assertTrue(workflowSource.contains("DBEAVER-MGMT-038"));
        Assert.assertTrue(workflowSource.contains("DBEAVER-MGMT-040"));
        Assert.assertTrue(workflowSource.contains("DBEAVER-MGMT-042"));
        Assert.assertTrue(workflowSource.contains("DBEAVER-MGMT-045"));
        Assert.assertTrue(workflowSource.contains("preview_sha256"));
        Assert.assertTrue(workflowSource.contains("command_sha256"));
        Assert.assertTrue(workflowSource.contains("applied_operation_sha256=requires_server_admission"));
        Assert.assertTrue(workflowSource.contains("PENDING_SERVER_ADMISSION"));
        Assert.assertTrue(workflowSource.contains("READY_TO_APPLY"));
        Assert.assertTrue(workflowSource.contains("APPLIED_SERVER_VALIDATED"));
        Assert.assertTrue(workflowSource.contains("REFUSED_APPLY"));
        Assert.assertTrue(workflowSource.contains("VERIFY_APPLY_REFRESHED"));
        Assert.assertTrue(workflowSource.contains("applyButtonEnabled("));
        Assert.assertTrue(workflowSource.contains("ScratchBirdMutationApplyExecutor.applyReadiness"));
        Assert.assertTrue(workflowSource.contains("RUN_IN_PROGRESS_SERVICE"));
        Assert.assertTrue(workflowSource.contains("CANCEL_SAFE"));
        Assert.assertTrue(workflowSource.contains("Dashboard refresh failures remain server refusals"));
        Assert.assertTrue(workflowSource.contains("Metadata invalidation refreshes from server truth"));

        String reportPlanSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdReportPlan.java");
        Assert.assertTrue(reportPlanSource.contains("record ScratchBirdReportPlan"));
        Assert.assertTrue(reportPlanSource.contains("scriptPreview"));
        Assert.assertTrue(reportPlanSource.contains("SHOW METRICS"));
        Assert.assertTrue(reportPlanSource.contains(".distinct()"));

        String formRegistrySource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdFormRegistry.java");
        Assert.assertTrue(formRegistrySource.contains("GenericTableForeignKey"));
        Assert.assertTrue(formRegistrySource.contains("GenericProcedure"));
        Assert.assertTrue(formRegistrySource.contains("GenericTrigger"));
        Assert.assertTrue(formRegistrySource.contains("GenericDataType"));
        Assert.assertTrue(formRegistrySource.contains("SBDV-FRM-613"));
        Assert.assertTrue(formRegistrySource.contains("isLikelyNonRelationalTable(table)"));

        String formContextSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdObjectFormContext.java");
        Assert.assertTrue(formContextSource.contains("record Field"));
        Assert.assertTrue(formContextSource.contains("DBPQualifiedObject"));
        Assert.assertTrue(formContextSource.contains("ScratchBird model family"));
        Assert.assertTrue(formContextSource.contains("ScratchBird datatype family"));
        Assert.assertTrue(formContextSource.contains("Value literal example"));
        Assert.assertTrue(formContextSource.contains("Trigger table"));

        String formPanelSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdFormPanelCatalog.java");
        Assert.assertTrue(formPanelSource.contains("record Panel"));
        Assert.assertTrue(formPanelSource.contains("ScratchBird administrative surface"));
        Assert.assertTrue(formPanelSource.contains("ScratchBird lifecycle route"));
        Assert.assertTrue(formPanelSource.contains("ScratchBird datatype and value authoring"));
        Assert.assertTrue(formPanelSource.contains("Structured payload editor with raw-text mirror and literal preview"));

        String featureXml = readFeatureXml();
        Assert.assertTrue(featureXml.contains("id=\"org.jkiss.dbeaver.ext.scratchbird\""));
        Assert.assertTrue(featureXml.contains("id=\"org.jkiss.dbeaver.ext.scratchbird.ui\""));

        String uiManifest = readUiManifest();
        Assert.assertTrue(uiManifest.contains("org.jkiss.dbeaver.ui.editors.sql"));
    }

    private static String readHostPluginXml() throws IOException {
        Bundle bundle = getHostBundle();
        if (bundle != null) {
            URL pluginXml = bundle.getEntry("plugin.xml");
            if (pluginXml != null) {
                try (InputStream stream = pluginXml.openStream()) {
                    byte[] bytes = stream.readAllBytes();
                    return new String(bytes, StandardCharsets.UTF_8);
                }
            }
        }

        Path fallbackPath = Path.of(System.getProperty("user.dir"))
            .resolve("../../plugins/org.jkiss.dbeaver.ext.scratchbird/plugin.xml")
            .normalize();
        Assert.assertTrue("ScratchBird plugin.xml not found: " + fallbackPath, Files.exists(fallbackPath));
        return Files.readString(fallbackPath, StandardCharsets.UTF_8);
    }

    private static String readUiPluginXml() throws IOException {
        Path fallbackPath = Path.of(System.getProperty("user.dir"))
            .resolve("../../plugins/org.jkiss.dbeaver.ext.scratchbird.ui/plugin.xml")
            .normalize();
        Assert.assertTrue("ScratchBird UI plugin.xml not found: " + fallbackPath, Files.exists(fallbackPath));
        return Files.readString(fallbackPath, StandardCharsets.UTF_8);
    }

    private static String readUiManifest() throws IOException {
        Path fallbackPath = Path.of(System.getProperty("user.dir"))
            .resolve("../../plugins/org.jkiss.dbeaver.ext.scratchbird.ui/META-INF/MANIFEST.MF")
            .normalize();
        Assert.assertTrue("ScratchBird UI manifest not found: " + fallbackPath, Files.exists(fallbackPath));
        return Files.readString(fallbackPath, StandardCharsets.UTF_8);
    }

    @Test
    public void liveProbePlannerUsesSafeServerBackedReadOnlyCommands() throws Exception {
        ScratchBirdFormDefinition taskForm = ScratchBirdFormRegistry.require("SBDV-FRM-016");
        ScratchBirdLiveProbe.ProbePlan taskProbe = ScratchBirdLiveProbe.plan(
            taskForm,
            ScratchBirdFormMode.TASK,
            "sys.monitoring",
            ScratchBirdAdminExecutor.plan(taskForm, ScratchBirdFormMode.TASK, "sys.monitoring"),
            ScratchBirdTaskCatalog.tasksFor("sys.monitoring"),
            null);
        Assert.assertTrue(taskProbe.executable());
        Assert.assertFalse(taskProbe.commandText().contains("SHOW MANAGEMENT CAPABILITIES"));
        Assert.assertTrue(taskProbe.commandText().contains("SHOW MANAGEMENT LISTENERS"));
        assertSafeProbeCommands(taskProbe.commands());

        ScratchBirdFormDefinition createForm = ScratchBirdFormRegistry.require("SBDV-FRM-101");
        ScratchBirdLiveProbe.ProbePlan createProbe = ScratchBirdLiveProbe.plan(
            createForm,
            ScratchBirdFormMode.CREATE,
            "sys.config",
            ScratchBirdAdminExecutor.plan(createForm, ScratchBirdFormMode.CREATE, "sys.config"),
            List.of(),
            null);
        Assert.assertTrue(createProbe.executable());
        Assert.assertTrue(createProbe.surrogate());
        Assert.assertFalse(createProbe.commandText().contains("CREATE "));
        Assert.assertTrue(createProbe.commandText().contains("SHOW MANAGEMENT MANAGER"));
        assertSafeProbeCommands(createProbe.commands());

        ScratchBirdFormDefinition deleteForm = ScratchBirdFormRegistry.require("SBDV-FRM-014");
        ScratchBirdAdminExecutor.ExecutionPlan deleteExecutionPlan = ScratchBirdAdminExecutor.plan(
            deleteForm,
            ScratchBirdFormMode.DELETE,
            "sys.jobs.cleanup_job");
        ScratchBirdLiveProbe.ProbePlan deleteProbe = ScratchBirdLiveProbe.plan(
            deleteForm,
            ScratchBirdFormMode.DELETE,
            "sys.jobs.cleanup_job",
            deleteExecutionPlan,
            List.of(),
            ScratchBirdDestructivePlan.forTarget("sys.jobs.cleanup_job", deleteExecutionPlan.commandText()));
        Assert.assertTrue(deleteProbe.executable());
        Assert.assertTrue(deleteProbe.surrogate());
        Assert.assertFalse(deleteProbe.commandText().contains("DROP JOB"));
        Assert.assertTrue(deleteProbe.commandText().contains("sys.catalog.object_resolver"));
        assertSafeProbeCommands(deleteProbe.commands());

        ScratchBirdTaskDefinition monitoringTask = ScratchBirdTaskCatalog.tasksFor("sys.monitoring").get(0);
        ScratchBirdLiveProbe.ProbePlan taskPreviewProbe = ScratchBirdLiveProbe.planForTask(
            monitoringTask,
            ScratchBirdLiveProbe.TaskProbePhase.PREVIEW,
            "server-authoritative test");
        Assert.assertTrue(taskPreviewProbe.executable());
        Assert.assertFalse(taskPreviewProbe.commandText().contains("SHOW MANAGEMENT CAPABILITIES"));
        Assert.assertTrue(taskPreviewProbe.commandText().contains("SHOW MANAGEMENT LISTENERS"));
        assertSafeProbeCommands(taskPreviewProbe.commands());

        ScratchBirdLiveProbe.ProbePlan taskExecuteProbe = ScratchBirdLiveProbe.planForTask(
            monitoringTask,
            ScratchBirdLiveProbe.TaskProbePhase.EXECUTE,
            "server-authoritative test");
        Assert.assertTrue(taskExecuteProbe.executable());
        Assert.assertTrue(taskExecuteProbe.commandText().contains("SHOW MANAGEMENT PARSER POOL FOR LISTENER"));
        assertSafeProbeCommands(taskExecuteProbe.commands());

        String uiDialogSource = readUiSource("org/jkiss/dbeaver/ext/scratchbird/ui/ScratchBirdManagementDialog.java");
        Assert.assertTrue(uiDialogSource.contains("Run Live Probe"));
        Assert.assertTrue(uiDialogSource.contains("Run Authz Probe"));
        Assert.assertTrue(uiDialogSource.contains("createAuthzTab"));
        Assert.assertTrue(uiDialogSource.contains("createLiveTab"));
        Assert.assertTrue(uiDialogSource.contains("ScratchBirdLiveProbe.execute"));
        Assert.assertTrue(uiDialogSource.contains("Run Task Preview"));
        Assert.assertTrue(uiDialogSource.contains("Run Task Validate"));
        Assert.assertTrue(uiDialogSource.contains("Run Task Execute"));
        Assert.assertTrue(uiDialogSource.contains("appendSelectedTaskProbe"));
        Assert.assertTrue(uiDialogSource.contains("runTaskProbe"));
        Assert.assertTrue(uiDialogSource.contains("createHistoryTab"));
        Assert.assertTrue(uiDialogSource.contains("Clear Local History"));
        Assert.assertTrue(uiDialogSource.contains("Store location"));
        Assert.assertTrue(uiDialogSource.contains("ScratchBirdProbeHistory.recordAuthorizationProbe"));
        Assert.assertTrue(uiDialogSource.contains("ScratchBirdProbeHistory.recordTaskProbe"));
        Assert.assertTrue(uiDialogSource.contains("ScratchBirdProbeHistory.recordLiveProbe"));
        Assert.assertTrue(uiDialogSource.contains("ScratchBirdProbeHistory.storeLocationText"));
        Assert.assertTrue(uiDialogSource.contains("appendProbeHistory"));
    }

    @Test
    public void serverAuthorizationPlannerUsesCapabilityInventoryAndRepresentativeReadOnlyCommands() throws Exception {
        ScratchBirdFormDefinition monitoringForm = ScratchBirdFormRegistry.require("SBDV-FRM-106");
        ScratchBirdAdminExecutor.ExecutionPlan monitoringInspectPlan = ScratchBirdAdminExecutor.plan(
            monitoringForm,
            ScratchBirdFormMode.INSPECT,
            "sys.monitoring");
        ScratchBirdLiveProbe.ProbePlan monitoringAuthzProbe = ScratchBirdPermissionProbe.planServerAuthorization(
            monitoringForm,
            ScratchBirdFormMode.INSPECT,
            "sys.monitoring",
            monitoringInspectPlan,
            ScratchBirdTaskCatalog.tasksFor("sys.monitoring"),
            null);
        Assert.assertTrue(monitoringAuthzProbe.executable());
        Assert.assertFalse(monitoringAuthzProbe.surrogate());
        Assert.assertTrue(monitoringAuthzProbe.commandText().contains("SELECT capability, enabled FROM sys.server_capabilities ORDER BY capability"));
        Assert.assertTrue(monitoringAuthzProbe.commandText().contains("SHOW MANAGEMENT LISTENERS"));
        Assert.assertFalse(monitoringAuthzProbe.commandText().contains("SHOW MANAGEMENT CAPABILITIES"));
        assertSafeProbeCommands(monitoringAuthzProbe.commands());

        ScratchBirdFormDefinition jobsForm = ScratchBirdFormRegistry.require("SBDV-FRM-102");
        ScratchBirdAdminExecutor.ExecutionPlan jobsCreatePlan = ScratchBirdAdminExecutor.plan(
            jobsForm,
            ScratchBirdFormMode.CREATE,
            "sys.jobs");
        ScratchBirdLiveProbe.ProbePlan jobsAuthzProbe = ScratchBirdPermissionProbe.planServerAuthorization(
            jobsForm,
            ScratchBirdFormMode.CREATE,
            "sys.jobs",
            jobsCreatePlan,
            List.of(),
            null);
        Assert.assertTrue(jobsAuthzProbe.executable());
        Assert.assertTrue(jobsAuthzProbe.surrogate());
        Assert.assertTrue(jobsAuthzProbe.commandText().contains("sys.server_capabilities"));
        Assert.assertTrue(jobsAuthzProbe.commandText().contains("sys.security.permission_probe"));
        Assert.assertTrue(jobsAuthzProbe.commandText().contains("form_id = 'SBDV-FRM-102'"));
        Assert.assertTrue(jobsAuthzProbe.commandText().contains("preview_hash = '"));
        Assert.assertTrue(jobsAuthzProbe.commandText().contains("command_hash = '"));
        Assert.assertTrue(jobsAuthzProbe.summary().contains("server mutation-admission read probe"));
        Assert.assertTrue(jobsAuthzProbe.commandText().contains("SELECT * FROM sys.jobs"));
        Assert.assertFalse(jobsAuthzProbe.commandText().contains("CREATE JOB"));
        assertSafeProbeCommands(jobsAuthzProbe.commands());

        ScratchBirdFormDefinition taskForm = ScratchBirdFormRegistry.require("SBDV-FRM-016");
        ScratchBirdLiveProbe.ProbePlan deniedAuthzProbe = ScratchBirdPermissionProbe.planServerAuthorization(
            taskForm,
            ScratchBirdFormMode.TASK,
            "sys.internals",
            ScratchBirdAdminExecutor.plan(taskForm, ScratchBirdFormMode.TASK, "sys.internals"),
            ScratchBirdTaskCatalog.tasksFor("sys.internals"),
            null);
        Assert.assertFalse(deniedAuthzProbe.executable());
        Assert.assertTrue(deniedAuthzProbe.summary().contains("Static client posture blocks"));

        String permissionProbeSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdPermissionProbe.java");
        Assert.assertTrue(permissionProbeSource.contains("planServerAuthorization"));
        Assert.assertTrue(permissionProbeSource.contains("sys.server_capabilities"));
        Assert.assertTrue(permissionProbeSource.contains("sys.security.permission_probe"));
        Assert.assertTrue(permissionProbeSource.contains("preview_hash"));
        Assert.assertTrue(permissionProbeSource.contains("command_hash"));
        Assert.assertTrue(permissionProbeSource.contains("mutationPermissionProbeCommand"));
        Assert.assertTrue(permissionProbeSource.contains("serverAuthorizationScript"));

        String adminExecutorSource = readHostSource("org/jkiss/dbeaver/ext/scratchbird/model/ScratchBirdAdminExecutor.java");
        Assert.assertTrue(adminExecutorSource.contains("ScratchBirdPermissionProbe.serverAuthorizationScript(targetPath)"));
    }

    @Test
    public void managementActionEnvelopeBindsPreviewSessionFeatureAndNetworkPolicy() {
        ScratchBirdFormDefinition jobsForm = ScratchBirdFormRegistry.require("SBDV-FRM-102");
        ScratchBirdAdminExecutor.ExecutionPlan createPlan = ScratchBirdAdminExecutor.plan(
            jobsForm,
            ScratchBirdFormMode.CREATE,
            "sys.jobs");
        ScratchBirdManagementActionEnvelope envelope = ScratchBirdManagementActionEnvelope.forPlan(
            jobsForm,
            ScratchBirdFormMode.CREATE,
            "sys.jobs",
            createPlan);

        Assert.assertTrue(envelope.envelopeId().startsWith("sbdv-action:"));
        Assert.assertEquals(64, envelope.previewHash().length());
        Assert.assertTrue(envelope.admissionProbeCommand().contains("sys.security.permission_probe"));
        Assert.assertTrue(envelope.admissionProbeCommand().contains("preview_hash"));
        Assert.assertTrue(envelope.admissionProbeCommand().contains("command_hash"));
        Assert.assertEquals("MGA_SERVER_OWNED_ALWAYS_ACTIVE_SESSION", envelope.transactionAuthority());
        Assert.assertTrue(envelope.sblrUuidPolicy().contains("server_must_revalidate_sblr_uuid"));
        Assert.assertEquals(
            ScratchBirdFeatureBoundary.Availability.SERVER_AUTHORIZATION_REQUIRED,
            envelope.featureBoundary().availability());
        Assert.assertFalse(envelope.applyAllowedByClientPosture());
        Assert.assertTrue(envelope.reviewLines().stream().anyMatch(line -> line.contains("principal, role, group")));
        Assert.assertTrue(envelope.sessionScope().summaryLines().stream().anyMatch(line -> line.contains("Isolation rule")));
        Assert.assertTrue(envelope.networkPolicy().deniesEndpointClass("plugin-telemetry"));
        Assert.assertEquals("<redacted>", envelope.networkPolicy().redactProperty("auth_token", "secret"));
        Assert.assertEquals("public", envelope.networkPolicy().redactProperty("schema", "public"));

        ScratchBirdFeatureBoundary clusterBoundary = ScratchBirdFeatureBoundary.forTarget(
            "cluster.nodes.node01",
            ScratchBirdFormMode.ALTER);
        Assert.assertEquals(ScratchBirdFeatureBoundary.Availability.CLOSED_PROVIDER_REQUIRED, clusterBoundary.availability());
        Assert.assertFalse(clusterBoundary.applyAllowed());

        ScratchBirdSessionScope alice = ScratchBirdSessionScope.forConnection(
            "conn-a",
            "alice",
            "developer",
            "engineering",
            "fr-CA");
        ScratchBirdSessionScope bob = ScratchBirdSessionScope.forConnection(
            "conn-a",
            "bob",
            "developer",
            "engineering",
            "fr-CA");
        ScratchBirdSessionScope aliceSpanish = ScratchBirdSessionScope.forConnection(
            "conn-a",
            "alice",
            "developer",
            "engineering",
            "es-ES");
        Assert.assertFalse(alice.cacheCompatibleWith(bob));
        Assert.assertFalse(alice.cacheCompatibleWith(aliceSpanish));
        Assert.assertTrue(alice.cacheCompatibleWith(ScratchBirdSessionScope.forConnection(
            "conn-a",
            "alice",
            "developer",
            "engineering",
            "fr-CA")));
    }

    @Test
    public void dataEditorTransferAndObjectGraphContractsExposeServerRevalidationProof() {
        ScratchBirdDataEditorContract.EditorPlan insertPlan = ScratchBirdDataEditorContract.plan(
            ScratchBirdDataEditorContract.Operation.INSERT,
            "data.sales.orders");
        Assert.assertTrue(insertPlan.previewCommand().contains("INSERT INTO data.sales.orders"));
        Assert.assertTrue(insertPlan.admissionProbeCommand().contains("SBDV-DATA-EDITOR"));
        Assert.assertTrue(insertPlan.transactionProof().stream().anyMatch(line -> line.contains("MGA transaction")));
        Assert.assertTrue(insertPlan.typeProof().stream().anyMatch(line -> line.contains("ScratchBirdValueProfile")));
        Assert.assertTrue(insertPlan.refusalProof().stream().anyMatch(line -> line.contains("server before mutation")));

        ScratchBirdDataEditorContract.EditorPlan refreshPlan = ScratchBirdDataEditorContract.plan(
            ScratchBirdDataEditorContract.Operation.REFRESH,
            "data.sales.orders");
        Assert.assertTrue(refreshPlan.previewCommand().contains("FETCH FIRST 200 ROWS ONLY"));
        Assert.assertTrue(refreshPlan.transactionProof().stream().anyMatch(line -> line.contains("MGA snapshot")));

        ScratchBirdDataTransferContract.TransferPlan exportPlan = ScratchBirdDataTransferContract.plan(
            ScratchBirdDataTransferContract.Direction.EXPORT,
            "data.sales.orders");
        Assert.assertTrue(exportPlan.previewCommand().contains("COPY data.sales.orders TO STDOUT"));
        Assert.assertTrue(exportPlan.authorizationProbe().contains("SBDV-DATA-TRANSFER"));
        Assert.assertTrue(exportPlan.batchingRules().stream().anyMatch(line -> line.contains("server-admitted memory")));
        Assert.assertTrue(exportPlan.encodingRules().stream().anyMatch(line -> line.contains("Language resource hashes")));

        ScratchBirdObjectGraphContract.GraphPlan graphPlan = ScratchBirdObjectGraphContract.plan("data.sales.orders");
        Assert.assertTrue(graphPlan.dependencyQuery().contains("sys.catalog.object_dependencies"));
        Assert.assertTrue(graphPlan.searchQuery().contains("sys.catalog.object_resolver"));
        Assert.assertTrue(graphPlan.ddlPreviewQuery().contains("sys.catalog.generated_ddl"));
        Assert.assertTrue(graphPlan.sbsqlPreviewQuery().contains("sys.catalog.generated_sbsql"));
        Assert.assertTrue(graphPlan.explainQuery().startsWith("EXPLAIN SELECT"));
        Assert.assertTrue(graphPlan.visibilityRules().stream().anyMatch(line -> line.contains("Hidden objects")));

        String uiDialogSource = readUiSource("org/jkiss/dbeaver/ext/scratchbird/ui/ScratchBirdManagementDialog.java");
        Assert.assertTrue(uiDialogSource.contains("Action envelope"));
        Assert.assertTrue(uiDialogSource.contains("Data editor insert contract"));
        Assert.assertTrue(uiDialogSource.contains("Data transfer export contract"));
        Assert.assertTrue(uiDialogSource.contains("Object graph contract"));
    }

    @Test
    public void mutationAdmissionStatusBindsPermissionProbeToPreviewAndCommandHash() {
        ScratchBirdFormDefinition jobsForm = ScratchBirdFormRegistry.require("SBDV-FRM-102");
        ScratchBirdAdminExecutor.ExecutionPlan createPlan = ScratchBirdAdminExecutor.plan(
            jobsForm,
            ScratchBirdFormMode.CREATE,
            "sys.jobs");
        String previewHash = ScratchBirdManagementActionEnvelope.previewHashFor(
            jobsForm,
            ScratchBirdFormMode.CREATE,
            "sys.jobs",
            createPlan);
        String commandHash = ScratchBirdManagementActionEnvelope.commandHashFor(createPlan);
        ScratchBirdLiveProbe.ProbePlan authzPlan = ScratchBirdPermissionProbe.planServerAuthorization(
            jobsForm,
            ScratchBirdFormMode.CREATE,
            "sys.jobs",
            createPlan,
            List.of(),
            null);

        ScratchBirdLiveProbe.ProbeResult admittedResult = new ScratchBirdLiveProbe.ProbeResult(
            authzPlan,
            ScratchBirdRefusalModel.admitted("Read-only ScratchBird server probe completed successfully."),
            List.of(new ScratchBirdLiveProbe.StatementResult(
                "SELECT admitted, refusal_code, refusal_message, preview_hash, command_hash FROM sys.security.permission_probe",
                true,
                1,
                0,
                List.of("admitted", "refusal_code", "refusal_message", "preview_hash", "command_hash"),
                List.of(List.of("true", "", "", previewHash, commandHash)))));
        ScratchBirdRefusalModel admitted = ScratchBirdLiveProbe.mutationAdmissionStatus(
            admittedResult,
            previewHash,
            commandHash);
        Assert.assertTrue(admitted.isAdmitted());
        Assert.assertTrue(ScratchBirdMutationApplyExecutor.applyReadiness(
            createPlan,
            admittedResult,
            previewHash,
            commandHash).isAdmitted());

        ScratchBirdLiveProbe.ProbeResult mismatchedResult = new ScratchBirdLiveProbe.ProbeResult(
            authzPlan,
            ScratchBirdRefusalModel.admitted("Read-only ScratchBird server probe completed successfully."),
            List.of(new ScratchBirdLiveProbe.StatementResult(
                "SELECT admitted, refusal_code, refusal_message, preview_hash, command_hash FROM sys.security.permission_probe",
                true,
                1,
                0,
                List.of("admitted", "refusal_code", "refusal_message", "preview_hash", "command_hash"),
                List.of(List.of("true", "", "", previewHash, "sha256:mismatch")))));
        ScratchBirdRefusalModel mismatched = ScratchBirdLiveProbe.mutationAdmissionStatus(
            mismatchedResult,
            previewHash,
            commandHash);
        Assert.assertEquals(ScratchBirdRefusalModel.Kind.PERMISSION_DENIED, mismatched.kind());

        ScratchBirdMutationApplyExecutor.ApplyResult refused = ScratchBirdMutationApplyExecutor.refuse(
            createPlan,
            mismatched,
            previewHash,
            commandHash);
        Assert.assertFalse(refused.applied());
        Assert.assertEquals("none", refused.appliedOperationHash());
        Assert.assertTrue(refused.previewText().contains("PERMISSION_DENIED"));
    }

    @Test
    public void probeHistoryRetainsPerScopeResultLogs() {
        ScratchBirdProbeHistory.clearAll();

        ScratchBirdFormDefinition form = ScratchBirdFormRegistry.require("SBDV-FRM-016");
        ScratchBirdTaskDefinition taskDefinition = ScratchBirdTaskCatalog.tasksFor("sys.monitoring").get(0);
        String scopeKey = "test-scope::sys.monitoring";

        ScratchBirdLiveProbe.ProbePlan livePlan = new ScratchBirdLiveProbe.ProbePlan(
            "Live preview execution",
            "Execute the generated read-only preview against the connected ScratchBird server.",
            "server-authoritative",
            true,
            false,
            List.of("SHOW MANAGEMENT LISTENERS"));
        ScratchBirdLiveProbe.ProbeResult liveResult = new ScratchBirdLiveProbe.ProbeResult(
            livePlan,
            ScratchBirdRefusalModel.admitted("Read-only ScratchBird server probe completed successfully."),
            List.of(new ScratchBirdLiveProbe.StatementResult(
                "SHOW MANAGEMENT LISTENERS",
                true,
                1,
                0,
                List.of("protocol"),
                List.of(List.of("scratchbird")))));
        ScratchBirdProbeHistory.recordLiveProbe(scopeKey, "sys.monitoring", form, liveResult);

        ScratchBirdLiveProbe.ProbePlan taskPlan = ScratchBirdLiveProbe.planForTask(
            taskDefinition,
            ScratchBirdLiveProbe.TaskProbePhase.PREVIEW,
            "server-authoritative");
        ScratchBirdLiveProbe.ProbeResult taskResult = new ScratchBirdLiveProbe.ProbeResult(
            taskPlan,
            ScratchBirdRefusalModel.admitted("Read-only ScratchBird server probe completed successfully."),
            List.of(new ScratchBirdLiveProbe.StatementResult(
                taskPlan.commands().get(0),
                true,
                1,
                0,
                List.of("protocol"),
                List.of(List.of("scratchbird")))));
        ScratchBirdProbeHistory.recordTaskProbe(
            scopeKey,
            "sys.monitoring",
            form,
            taskDefinition,
            ScratchBirdLiveProbe.TaskProbePhase.PREVIEW,
            taskResult);

        ScratchBirdLiveProbe.ProbePlan authzPlan = new ScratchBirdLiveProbe.ProbePlan(
            "Server authz probe",
            "Execute capability inventory plus branch-specific read-only commands against the connected ScratchBird server.",
            "server-authoritative; capability inventory via sys.server_capabilities",
            true,
            false,
            List.of(
                "SELECT capability, enabled FROM sys.server_capabilities ORDER BY capability",
                "SHOW MANAGEMENT LISTENERS"));
        ScratchBirdLiveProbe.ProbeResult authzResult = new ScratchBirdLiveProbe.ProbeResult(
            authzPlan,
            ScratchBirdRefusalModel.admitted("Read-only ScratchBird server probe completed successfully."),
            List.of(new ScratchBirdLiveProbe.StatementResult(
                authzPlan.commands().get(0),
                true,
                6,
                0,
                List.of("capability", "enabled"),
                List.of(List.of("compression", "true")))));
        ScratchBirdProbeHistory.recordAuthorizationProbe(scopeKey, "sys.monitoring", form, authzResult);

        ScratchBirdMutationApplyExecutor.ApplyResult applyResult = ScratchBirdMutationApplyExecutor.refuse(
            ScratchBirdAdminExecutor.plan(form, ScratchBirdFormMode.TASK, "sys.monitoring"),
            ScratchBirdRefusalModel.serverAdmissionRequired(
                "No server permission probe has been executed for this mutation preview.",
                "sys.security.permission_probe"),
            "preview-hash",
            "command-hash");
        ScratchBirdProbeHistory.recordApply(scopeKey, "sys.monitoring", form, applyResult);

        List<ScratchBirdProbeHistory.HistoryEntry> entries = ScratchBirdProbeHistory.historyFor(scopeKey);
        Assert.assertEquals(4, entries.size());
        Assert.assertEquals(ScratchBirdProbeHistory.EntryKind.APPLY, entries.get(0).kind());
        Assert.assertEquals(ScratchBirdProbeHistory.EntryKind.AUTHZ_PROBE, entries.get(1).kind());
        Assert.assertEquals(ScratchBirdProbeHistory.EntryKind.TASK_PREVIEW, entries.get(2).kind());
        Assert.assertEquals(ScratchBirdProbeHistory.EntryKind.LIVE_PROBE, entries.get(3).kind());
        Assert.assertEquals("SBDV-TSK-301", entries.get(2).taskId());
        Assert.assertTrue(entries.get(0).displayLabel().contains("Apply"));
        Assert.assertTrue(entries.get(0).previewText().contains("SERVER_ADMISSION_REQUIRED"));
        Assert.assertTrue(entries.get(1).displayLabel().contains("Authz probe"));
        Assert.assertTrue(entries.get(1).previewText().contains("sys.server_capabilities"));
        Assert.assertTrue(entries.get(2).displayLabel().contains("Task preview"));
        Assert.assertTrue(entries.get(2).previewText().contains("Statements executed: 1"));
        Assert.assertTrue(entries.get(2).summaryLines().stream().anyMatch(line -> line.contains("SBDV-FRM-016")));
        Assert.assertTrue(entries.get(3).commandText().contains("SHOW MANAGEMENT LISTENERS"));
        Assert.assertTrue(Files.exists(ScratchBirdProbeHistory.storeFileForTests()));

        ScratchBirdProbeHistory.resetCache();
        List<ScratchBirdProbeHistory.HistoryEntry> reloadedEntries = ScratchBirdProbeHistory.historyFor(scopeKey);
        Assert.assertEquals(4, reloadedEntries.size());
        Assert.assertEquals(ScratchBirdProbeHistory.EntryKind.AUTHZ_PROBE, reloadedEntries.get(0).kind());
        Assert.assertEquals(ScratchBirdProbeHistory.EntryKind.TASK_PREVIEW, reloadedEntries.get(1).kind());
        Assert.assertEquals("SBDV-TSK-301", reloadedEntries.get(1).taskId());
        Assert.assertTrue(reloadedEntries.get(0).commandText().contains("sys.server_capabilities"));
        Assert.assertTrue(reloadedEntries.get(2).commandText().contains("SHOW MANAGEMENT LISTENERS"));

        ScratchBirdProbeHistory.clear(scopeKey);
        Assert.assertTrue(ScratchBirdProbeHistory.historyFor(scopeKey).isEmpty());
        ScratchBirdProbeHistory.clearAll();
        Assert.assertFalse(Files.exists(ScratchBirdProbeHistory.storeFileForTests()));
    }

    private static String readFeatureXml() throws IOException {
        Path fallbackPath = Path.of(System.getProperty("user.dir"))
            .resolve("../../features/org.jkiss.dbeaver.ext.scratchbird.feature/feature.xml")
            .normalize();
        if (!Files.exists(fallbackPath)) {
            fallbackPath = Path.of(System.getProperty("user.dir"))
                .resolve("../../features/org.jkiss.dbeaver.db.feature/feature.xml")
                .normalize();
        }
        Assert.assertTrue("ScratchBird feature.xml not found: " + fallbackPath, Files.exists(fallbackPath));
        return Files.readString(fallbackPath, StandardCharsets.UTF_8);
    }

    private static String readHostSource(String relativePath) throws IOException {
        Path fallbackPath = Path.of(System.getProperty("user.dir"))
            .resolve("../../plugins/org.jkiss.dbeaver.ext.scratchbird/src")
            .resolve(relativePath)
            .normalize();
        Assert.assertTrue("ScratchBird source not found: " + fallbackPath, Files.exists(fallbackPath));
        return Files.readString(fallbackPath, StandardCharsets.UTF_8);
    }

    private static String readUiSource(String relativePath) throws IOException {
        Path fallbackPath = Path.of(System.getProperty("user.dir"))
            .resolve("../../plugins/org.jkiss.dbeaver.ext.scratchbird.ui/src")
            .resolve(relativePath)
            .normalize();
        Assert.assertTrue("ScratchBird UI source not found: " + fallbackPath, Files.exists(fallbackPath));
        return Files.readString(fallbackPath, StandardCharsets.UTF_8);
    }

    private static Bundle getHostBundle() {
        return Platform.getBundle("org.jkiss.dbeaver.ext.scratchbird");
    }

    private static int countOccurrences(String source, String needle) {
        int count = 0;
        int offset = 0;
        while ((offset = source.indexOf(needle, offset)) >= 0) {
            count++;
            offset += needle.length();
        }
        return count;
    }

    private static void assertSafeProbeCommands(List<String> commands) {
        Assert.assertFalse(commands.isEmpty());
        for (String command : commands) {
            ScratchBirdV3ParseResult result = ScratchBirdV3Parser.parse(command + ";");
            Assert.assertTrue(command + " => " + result.diagnostics(), result.success());
            for (ScratchBirdV3Statement statement : result.statements()) {
                Assert.assertTrue(command, ScratchBirdLiveProbe.isSafeForExecution(statement));
            }
        }
    }

    private static org.jkiss.dbeaver.model.struct.DBSTypedObject typedObject(String typeName) {
        return (org.jkiss.dbeaver.model.struct.DBSTypedObject) Proxy.newProxyInstance(
            org.jkiss.dbeaver.model.struct.DBSTypedObject.class.getClassLoader(),
            new Class<?>[] {org.jkiss.dbeaver.model.struct.DBSTypedObject.class},
            (proxy, method, args) -> {
                if (method.getDeclaringClass() == Object.class) {
                    return switch (method.getName()) {
                        case "toString" -> "TypedObjectProxy(" + typeName + ")";
                        case "hashCode" -> System.identityHashCode(proxy);
                        case "equals" -> proxy == args[0];
                        default -> null;
                    };
                }
                return switch (method.getName()) {
                    case "getTypeName", "getFullTypeName" -> typeName;
                    case "getTypeID" -> java.sql.Types.OTHER;
                    case "getDataKind" -> DBPDataKind.STRING;
                    case "getScale", "getPrecision" -> null;
                    case "getMaxLength", "getTypeModifiers" -> 0L;
                    default -> defaultValue(method.getReturnType());
                };
            });
    }

    private static Object defaultValue(Class<?> returnType) {
        if (!returnType.isPrimitive()) {
            return null;
        }
        if (returnType == boolean.class) {
            return false;
        }
        if (returnType == byte.class) {
            return (byte) 0;
        }
        if (returnType == short.class) {
            return (short) 0;
        }
        if (returnType == int.class) {
            return 0;
        }
        if (returnType == long.class) {
            return 0L;
        }
        if (returnType == float.class) {
            return 0F;
        }
        if (returnType == double.class) {
            return 0D;
        }
        if (returnType == char.class) {
            return '\0';
        }
        return null;
    }

    private static ScratchBirdFormPanelCatalog.Panel requirePanel(
        List<ScratchBirdFormPanelCatalog.Panel> panels,
        String id
    ) {
        return panels.stream()
            .filter(panel -> panel.id().equals(id))
            .findFirst()
            .orElseThrow(() -> new AssertionError("Panel not found: " + id + " in " + panels));
    }

    private static String panelValue(
        ScratchBirdFormPanelCatalog.Panel panel,
        String label
    ) {
        return panel.entries().stream()
            .filter(entry -> entry.label().equals(label))
            .map(ScratchBirdFormPanelCatalog.Entry::value)
            .findFirst()
            .orElseThrow(() -> new AssertionError("Panel entry not found: " + label + " in " + panel));
    }

    private static String sampleTargetPathFor(ScratchBirdFormDefinition form) {
        return switch (form.id()) {
            case "SBDV-FRM-010", "SBDV-FRM-011", "SBDV-FRM-012", "SBDV-FRM-013" -> "main";
            case "SBDV-FRM-014" -> "data.sales.orders";
            case "SBDV-FRM-016" -> "sys.monitoring";
            case "SBDV-FRM-101" -> "sys.config";
            case "SBDV-FRM-102" -> "sys.jobs";
            case "SBDV-FRM-103", "SBDV-FRM-110" -> "sys.security";
            case "SBDV-FRM-104" -> "sys.cluster";
            case "SBDV-FRM-105" -> "sys.emulation";
            case "SBDV-FRM-106" -> "sys.monitoring";
            case "SBDV-FRM-107" -> "sys.catalog";
            case "SBDV-FRM-108" -> "sys.structure";
            case "SBDV-FRM-109" -> "sys.internals";
            case "SBDV-FRM-200" -> "users.home.alice";
            case "SBDV-FRM-201" -> "users.groups.analytics";
            case "SBDV-FRM-202" -> "users.home.alice.profiles.local";
            case "SBDV-FRM-203" -> "users.home.alice.links";
            case "SBDV-FRM-204" -> "users.home.alice.scratch";
            case "SBDV-FRM-205" -> "users.groups.analytics.users";
            case "SBDV-FRM-206" -> "users.groups.analytics.public";
            case "SBDV-FRM-207" -> "users.groups.analytics.data";
            case "SBDV-FRM-300" -> "cluster.users.alice";
            case "SBDV-FRM-301" -> "cluster.groups.analytics";
            case "SBDV-FRM-302" -> "cluster.domains.default";
            case "SBDV-FRM-303" -> "cluster.locations.primary";
            case "SBDV-FRM-304" -> "cluster.nodes.node01";
            case "SBDV-FRM-305" -> "cluster.shared.cache";
            case "SBDV-FRM-400" -> "emulated.postgresql";
            case "SBDV-FRM-401" -> "emulated.postgresql.server01";
            case "SBDV-FRM-402" -> "emulated.postgresql.server01.public";
            case "SBDV-FRM-500" -> "remote.links.erp";
            case "SBDV-FRM-501" -> "remote.fdw.postgresql";
            case "SBDV-FRM-502" -> "remote.postgresql.erp";
            case "SBDV-FRM-503" -> "remote.postgresql.erp.mapping";
            case "SBDV-FRM-601", "SBDV-FRM-603" -> "data.sales.orders";
            case "SBDV-FRM-602", "SBDV-FRM-604", "SBDV-FRM-612" -> "data.docs.events";
            case "SBDV-FRM-605", "SBDV-FRM-613" -> "data.sales.orders.payload";
            case "SBDV-FRM-606" -> "data.sales.orders.orders_pk";
            case "SBDV-FRM-607" -> "data.sales.orders.idx_orders";
            case "SBDV-FRM-608" -> "data.sales.order_seq";
            case "SBDV-FRM-609" -> "data.sales.reconcile_orders";
            case "SBDV-FRM-610" -> "data.sales.orders_bi";
            case "SBDV-FRM-611" -> "sys.domains.email";
            case "SBDV-FRM-900" -> "metrics";
            case "SBDV-FRM-901" -> "metrics.health-scorecards.SBDV-RPT-CORE-001";
            case "SBDV-FRM-902" -> "metrics.storage-buffer-cache.SBDV-RPT-CORE-008";
            case "SBDV-FRM-903" -> "metrics.workload-and-sql.SBDV-RPT-CORE-003";
            case "SBDV-FRM-904" -> "metrics.listener-and-parser.SBDV-RPT-CORE-013";
            default -> "data.sales";
        };
    }

    private static ScratchBirdSchemaTreeBuilder.Node findNodeByName(
        Collection<ScratchBirdSchemaTreeBuilder.Node> nodes,
        String name
    ) {
        for (ScratchBirdSchemaTreeBuilder.Node node : nodes) {
            if (name.equals(node.getName())) {
                return node;
            }
        }
        return null;
    }
}
