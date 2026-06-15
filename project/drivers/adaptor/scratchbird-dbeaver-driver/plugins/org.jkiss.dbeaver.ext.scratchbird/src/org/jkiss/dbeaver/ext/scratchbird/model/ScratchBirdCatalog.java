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
import org.jkiss.dbeaver.DBException;
import org.jkiss.dbeaver.Log;
import org.jkiss.dbeaver.ext.generic.model.GenericCatalog;
import org.jkiss.dbeaver.ext.generic.model.GenericDataSource;
import org.jkiss.dbeaver.ext.generic.model.GenericSchema;
import org.jkiss.dbeaver.ext.generic.model.GenericTable;
import org.jkiss.dbeaver.model.DBUtils;
import org.jkiss.dbeaver.model.exec.jdbc.JDBCPreparedStatement;
import org.jkiss.dbeaver.model.exec.jdbc.JDBCResultSet;
import org.jkiss.dbeaver.model.exec.jdbc.JDBCSession;
import org.jkiss.dbeaver.model.impl.jdbc.JDBCUtils;
import org.jkiss.dbeaver.model.meta.Association;
import org.jkiss.dbeaver.model.meta.Property;
import org.jkiss.dbeaver.model.runtime.DBRProgressMonitor;
import org.jkiss.dbeaver.model.struct.DBSObject;
import org.jkiss.utils.CommonUtils;

import java.sql.ResultSet;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

public class ScratchBirdCatalog extends GenericCatalog {

    private static final Log log = Log.getLog(ScratchBirdCatalog.class);

    private List<ScratchBirdSchemaNode> schemaTree;
    @Nullable
    private String databaseUuid;
    @Nullable
    private String databaseDisplayName;

    public ScratchBirdCatalog(@NotNull GenericDataSource dataSource, @NotNull String catalogName) {
        super(dataSource, catalogName);
    }

    @NotNull
    @Override
    public String getName() {
        return CommonUtils.isEmpty(databaseDisplayName) ? super.getName() : databaseDisplayName;
    }

    @Association
    public synchronized Collection<ScratchBirdSchemaNode> getSchemaTree(@NotNull DBRProgressMonitor monitor) throws DBException {
        if (schemaTree == null && !monitor.isForceCacheUsage()) {
            buildSchemaTree(monitor);
        }
        return schemaTree == null ? Collections.emptyList() : schemaTree;
    }

    private void buildSchemaTree(@NotNull DBRProgressMonitor monitor) throws DBException {
        List<ScratchBirdCatalogObjectReference> catalogReferences = loadCatalogObjectReferences(monitor);
        if (catalogReferences.isEmpty()) {
            schemaTree = Collections.emptyList();
            return;
        }

        Collection<GenericSchema> schemas = getSchemas(monitor);

        List<String> schemaPaths = new ArrayList<>();
        Map<String, ScratchBirdSchema> schemasByPath = new LinkedHashMap<>();

        if (!CommonUtils.isEmpty(schemas)) {
            for (GenericSchema schema : schemas) {
                String fullPath = schema.getName();
                if (CommonUtils.isEmpty(fullPath)) {
                    continue;
                }
                schemaPaths.add(fullPath);
                if (schema instanceof ScratchBirdSchema scratchBirdSchema) {
                    schemasByPath.put(fullPath, scratchBirdSchema);
                } else {
                    ScratchBirdSchema wrappedSchema = new ScratchBirdSchema(getDataSource(), this, fullPath);
                    if (schema.isVirtual()) {
                        wrappedSchema.setVirtual(true);
                    }
                    schemasByPath.put(fullPath, wrappedSchema);
                }
            }
        }

        if (schemaPaths.isEmpty()) {
            loadMetadataSchemas(monitor, schemaPaths, schemasByPath);
        }

        List<ScratchBirdSchemaTreeBuilder.Node> tree = ScratchBirdSchemaTreeBuilder.buildFromCatalog(catalogReferences);
        List<ScratchBirdSchemaNode> roots = new ArrayList<>();
        for (ScratchBirdSchemaTreeBuilder.Node root : tree) {
            roots.add(inflateTree(root, null, schemasByPath));
        }
        schemaTree = roots;
    }

    private void loadMetadataSchemas(
        @NotNull DBRProgressMonitor monitor,
        @NotNull List<String> schemaPaths,
        @NotNull Map<String, ScratchBirdSchema> schemasByPath
    ) {
        try (JDBCSession session = DBUtils.openMetaSession(monitor, this, "Load ScratchBird JDBC metadata schemas");
             ResultSet resultSet = session.getMetaData().getSchemas(null, "%")) {
            while (resultSet.next()) {
                String schemaPath = resultSet.getString("TABLE_SCHEM");
                if (CommonUtils.isEmpty(schemaPath) || ScratchBirdNamespaceSemantics.isMetricsPath(schemaPath)) {
                    continue;
                }
                if (schemasByPath.containsKey(schemaPath)) {
                    continue;
                }
                schemaPaths.add(schemaPath);
                schemasByPath.put(schemaPath, new ScratchBirdSchema(getDataSource(), this, schemaPath));
            }
        } catch (SQLException | DBException e) {
            log.debug("ScratchBird JDBC metadata schemas are not available for navigator fallback", e);
        }
    }

    @NotNull
    @Property(viewable = true, optional = true, order = 2)
    public String getIdentityStatus() {
        return CommonUtils.isEmpty(databaseUuid)
            ? "database UUID not published by current JDBC metadata surface"
            : "database UUID " + databaseUuid;
    }

    @Nullable
    @Property(viewable = true, optional = true, order = 3)
    public String getDatabaseUuid() {
        return databaseUuid;
    }

    @NotNull
    private List<ScratchBirdCatalogObjectReference> loadCatalogObjectReferences(
        @NotNull DBRProgressMonitor monitor
    ) {
        List<ScratchBirdCatalogObjectReference> references = new ArrayList<>();
        try {
            executeNavigatorTreeQuery(monitor, references);
            return references;
        } catch (SQLException | DBException e) {
            log.warn("ScratchBird readable navigator tree is required for first-release navigation", e);
        }
        return references;
    }

    private void executeNavigatorTreeQuery(
        @NotNull DBRProgressMonitor monitor,
        @NotNull List<ScratchBirdCatalogObjectReference> references
    ) throws SQLException, DBException {
        String query = "SELECT node_id, parent_node_id, object_id, parent_object_id, node_path, node_name, " +
            "node_role, object_kind, object_path, schema_path " +
            "FROM sys.catalog_readable.navigator_tree";
        try (JDBCSession session = DBUtils.openMetaSession(monitor, this, "Load ScratchBird navigator tree");
             JDBCPreparedStatement statement = session.prepareStatement(query);
             JDBCResultSet resultSet = statement.executeQuery()) {
            while (resultSet.next()) {
                String nodeRole = JDBCUtils.safeGetString(resultSet, "node_role");
                String nodeName = JDBCUtils.safeGetString(resultSet, "node_name");
                String objectKind = JDBCUtils.safeGetString(resultSet, "object_kind");
                if ("database".equalsIgnoreCase(nodeRole)) {
                    if (isUsableDatabaseDisplayName(nodeName)) {
                        databaseDisplayName = nodeName;
                    }
                    continue;
                }
                if (!isNavigatorDisplayRole(nodeRole, objectKind)) {
                    continue;
                }

                String fullPath = navigatorDisplayPath(nodeRole, objectKind, nodeName, resultSet);
                if (CommonUtils.isEmpty(fullPath) || ScratchBirdNamespaceSemantics.isMetricsPath(fullPath)) {
                    continue;
                }

                ScratchBirdCatalogObjectReference reference;
                String objectType = navigatorObjectType(nodeRole, objectKind);
                String objectId = JDBCUtils.safeGetString(resultSet, "object_id");
                String authorityPath = navigatorAuthorityPath(fullPath, resultSet);
                if (CommonUtils.isNotEmpty(objectId) && !isNavigatorFolder(objectKind)) {
                    reference = ScratchBirdCatalogObjectReference.catalogObject(
                        databaseUuid,
                        objectId,
                        JDBCUtils.safeGetString(resultSet, "parent_object_id"),
                        objectType,
                        fullPath,
                        authorityPath,
                        CommonUtils.isEmpty(nodeName) ? leafName(fullPath) : nodeName);
                } else {
                    reference = ScratchBirdCatalogObjectReference.clientOnly(
                        fullPath,
                        objectType);
                }
                if (reference.hasCatalogIdentity()) {
                    cacheAuthorizedReference(reference);
                }
                references.add(reference);
            }
        }
    }

    private static boolean isNavigatorDisplayRole(@Nullable String nodeRole, @Nullable String objectKind) {
        if ("database".equalsIgnoreCase(nodeRole)) {
            return false;
        }
        return CommonUtils.isNotEmpty(nodeRole) || CommonUtils.isNotEmpty(objectKind);
    }

    private static boolean isUsableDatabaseDisplayName(@Nullable String value) {
        if (CommonUtils.isEmpty(value)) {
            return false;
        }
        return !value.toLowerCase(Locale.ENGLISH)
            .matches("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}");
    }

    @NotNull
    private static String navigatorDisplayPath(
        @Nullable String nodeRole,
        @Nullable String objectKind,
        @Nullable String nodeName,
        @NotNull JDBCResultSet resultSet
    ) {
        if ("schema".equalsIgnoreCase(objectKind)) {
            String nodePath = navigatorPathFromNodePath(JDBCUtils.safeGetString(resultSet, "node_path"));
            if (CommonUtils.isNotEmpty(nodePath)) {
                return nodePath;
            }
            String objectPath = JDBCUtils.safeGetString(resultSet, "object_path");
            if (CommonUtils.isNotEmpty(objectPath)) {
                return objectPath;
            }
            String schemaPath = JDBCUtils.safeGetString(resultSet, "schema_path");
            if (CommonUtils.isNotEmpty(schemaPath)) {
                return schemaPath;
            }
            return CommonUtils.isEmpty(nodeName) ? "" : nodeName;
        }
        if (nodeRole != null && nodeRole.startsWith("database.")) {
            String nodePath = navigatorPathFromNodePath(JDBCUtils.safeGetString(resultSet, "node_path"));
            if (CommonUtils.isNotEmpty(nodePath)) {
                return nodePath;
            }
            return CommonUtils.isEmpty(nodeName) ? "" : nodeName;
        }
        if (nodeRole != null && nodeRole.startsWith("security.")) {
            String nodePath = navigatorPathFromNodePath(JDBCUtils.safeGetString(resultSet, "node_path"));
            if (CommonUtils.isNotEmpty(nodePath)) {
                return nodePath;
            }
            return "Security." + (CommonUtils.isEmpty(nodeName) ? "" : nodeName);
        }
        String nodePath = navigatorPathFromNodePath(JDBCUtils.safeGetString(resultSet, "node_path"));
        if (CommonUtils.isNotEmpty(nodePath)) {
            return nodePath;
        }
        return CommonUtils.isEmpty(nodeName) ? "" : nodeName;
    }

    @NotNull
    private static String navigatorObjectType(@Nullable String nodeRole, @Nullable String objectKind) {
        if (CommonUtils.isNotEmpty(objectKind) && !isNavigatorFolder(objectKind)) {
            return objectKind.toUpperCase(Locale.ENGLISH);
        }
        return CommonUtils.isEmpty(nodeRole)
            ? "NAVIGATOR"
            : nodeRole.toUpperCase(Locale.ENGLISH);
    }

    private static boolean isNavigatorFolder(@Nullable String objectKind) {
        return "folder".equalsIgnoreCase(objectKind);
    }

    @NotNull
    private static String navigatorAuthorityPath(@NotNull String fullPath, @NotNull JDBCResultSet resultSet) {
        String objectPath = JDBCUtils.safeGetString(resultSet, "object_path");
        if (CommonUtils.isNotEmpty(objectPath)) {
            return objectPath;
        }
        String schemaPath = JDBCUtils.safeGetString(resultSet, "schema_path");
        if (CommonUtils.isNotEmpty(schemaPath)) {
            return schemaPath;
        }
        return fullPath;
    }

    @NotNull
    private static String navigatorPathFromNodePath(@Nullable String nodePath) {
        if (CommonUtils.isEmpty(nodePath)) {
            return "";
        }
        String trimmed = nodePath.trim();
        int firstSeparator = trimmed.indexOf('/');
        if (firstSeparator >= 0) {
            trimmed = trimmed.substring(firstSeparator + 1);
        }
        return trimmed.replace('/', '.');
    }

    private void executeCatalogObjectQuery(
        @NotNull DBRProgressMonitor monitor,
        @NotNull String query,
        @NotNull List<ScratchBirdCatalogObjectReference> references
    ) throws SQLException, DBException {
        try (JDBCSession session = DBUtils.openMetaSession(monitor, this, "Load ScratchBird catalog UUID tree");
             JDBCPreparedStatement statement = session.prepareStatement(query);
             JDBCResultSet resultSet = statement.executeQuery()) {
            while (resultSet.next()) {
                String fullPath = JDBCUtils.safeGetString(resultSet, "full_path");
                if (CommonUtils.isEmpty(fullPath)) {
                    fullPath = JDBCUtils.safeGetString(resultSet, "object_path");
                }
                if (CommonUtils.isEmpty(fullPath)) {
                    fullPath = JDBCUtils.safeGetString(resultSet, "schema_path");
                }
                if (CommonUtils.isEmpty(fullPath) || ScratchBirdNamespaceSemantics.isMetricsPath(fullPath)) {
                    continue;
                }
                String objectType = JDBCUtils.safeGetString(resultSet, "object_type");
                if (CommonUtils.isEmpty(objectType)) {
                    objectType = JDBCUtils.safeGetString(resultSet, "object_kind");
                }
                if (CommonUtils.isNotEmpty(objectType) && !"schema".equalsIgnoreCase(objectType)) {
                    continue;
                }
                String objectName = JDBCUtils.safeGetString(resultSet, "object_name");
                if (CommonUtils.isEmpty(databaseUuid)) {
                    databaseUuid = JDBCUtils.safeGetString(resultSet, "database_id");
                }
                ScratchBirdCatalogObjectReference reference = ScratchBirdCatalogObjectReference.schema(
                    databaseUuid,
                    JDBCUtils.safeGetString(resultSet, "object_id"),
                    JDBCUtils.safeGetString(resultSet, "parent_object_id"),
                    fullPath,
                    CommonUtils.isEmpty(objectName) ? leafName(fullPath) : objectName);
                references.add(reference);
                cacheAuthorizedReference(reference);
            }
        }
    }

    private void cacheAuthorizedReference(@NotNull ScratchBirdCatalogObjectReference reference) {
        if (!(getDataSource() instanceof ScratchBirdDataSource scratchBirdDataSource)) {
            return;
        }
        ScratchBirdSessionScope sessionScope = scratchBirdDataSource.getScratchBirdSessionScope();
        sessionScope.resolverCache().putAuthorizedReference(
            sessionScope.authorizationContext(),
            reference,
            CommonUtils.notEmpty(reference.parentUuid()),
            "server_filtered");
    }

    @NotNull
    private static String leafName(@NotNull String fullPath) {
        int separator = fullPath.lastIndexOf('.');
        return separator < 0 ? fullPath : fullPath.substring(separator + 1);
    }

    @NotNull
    private ScratchBirdSchemaNode inflateTree(
        @NotNull ScratchBirdSchemaTreeBuilder.Node source,
        @Nullable ScratchBirdSchemaNode parent,
        @NotNull Map<String, ScratchBirdSchema> schemasByPath
    ) {
        ScratchBirdSchemaNode node = new ScratchBirdSchemaNode(
            getDataSource(),
            this,
            parent,
            source.getName(),
            source.getFullPath(),
            source.isCatalogBacked(),
            source.isClientOnly(),
            source.getObjectPath(),
            source.getObjectType());
        ScratchBirdSchema backing = schemasByPath.get(source.getFullPath());
        if (backing == null) {
            backing = schemasByPath.get(source.getObjectPath().authorityPath());
        }
        if ("SCHEMA".equalsIgnoreCase(source.getObjectType()) &&
                source.isCatalogBacked() &&
                (backing != null || source.isTerminal())) {
            node.setBackingSchema(backing == null
                ? new ScratchBirdSchema(getDataSource(), this, source.getObjectPath().authorityPath())
                : backing);
        }
        for (ScratchBirdSchemaTreeBuilder.Node child : source.getChildren()) {
            if (isNavigatorPhysicalTable(child)) {
                node.addNavigatorPhysicalTable(
                    child.getName(),
                    navigatorRelationType(child.getObjectType(), "TABLE"),
                    child.getObjectPath());
                continue;
            }
            if (isNavigatorView(child)) {
                node.addNavigatorView(
                    child.getName(),
                    navigatorRelationType(child.getObjectType(), "VIEW"),
                    child.getObjectPath());
                continue;
            }
            node.addChild(inflateTree(child, node, schemasByPath));
        }
        return node;
    }

    private static boolean isNavigatorPhysicalTable(@NotNull ScratchBirdSchemaTreeBuilder.Node source) {
        return "TABLE".equalsIgnoreCase(source.getObjectType()) ||
            "SYSTEM TABLE".equalsIgnoreCase(source.getObjectType());
    }

    private static boolean isNavigatorView(@NotNull ScratchBirdSchemaTreeBuilder.Node source) {
        return "VIEW".equalsIgnoreCase(source.getObjectType()) ||
            "SYSTEM VIEW".equalsIgnoreCase(source.getObjectType());
    }

    @NotNull
    private static String navigatorRelationType(@Nullable String objectType, @NotNull String fallback) {
        return CommonUtils.isEmpty(objectType) ? fallback : objectType.toUpperCase(Locale.ENGLISH);
    }

    @Nullable
    @Override
    public Collection<? extends DBSObject> getChildren(@NotNull DBRProgressMonitor monitor) throws DBException {
        Collection<ScratchBirdSchemaNode> tree = getSchemaTree(monitor);
        if (!tree.isEmpty()) {
            return tree;
        }
        return super.getChildren(monitor);
    }

    @Override
    public DBSObject getChild(@NotNull DBRProgressMonitor monitor, @NotNull String childName) throws DBException {
        for (ScratchBirdSchemaNode node : getSchemaTree(monitor)) {
            if (childName.equals(node.getName())) {
                return node;
            }
        }
        return super.getChild(monitor, childName);
    }

    @NotNull
    @Override
    public Class<? extends DBSObject> getPrimaryChildType(@Nullable DBRProgressMonitor monitor) throws DBException {
        if (monitor != null && !getSchemaTree(monitor).isEmpty()) {
            return ScratchBirdSchemaNode.class;
        }
        return GenericTable.class;
    }

    @Override
    public DBSObject refreshObject(@NotNull DBRProgressMonitor monitor) throws DBException {
        schemaTree = null;
        return super.refreshObject(monitor);
    }
}
