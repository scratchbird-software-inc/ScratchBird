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
import org.jkiss.dbeaver.ext.generic.model.GenericObjectContainer;
import org.jkiss.dbeaver.ext.generic.model.GenericStructContainer;
import org.jkiss.dbeaver.ext.generic.model.GenericTable;
import org.jkiss.dbeaver.ext.generic.model.GenericTableBase;
import org.jkiss.dbeaver.ext.generic.model.GenericTableIndex;
import org.jkiss.dbeaver.ext.generic.model.GenericUniqueKey;
import org.jkiss.dbeaver.ext.generic.model.GenericView;
import org.jkiss.dbeaver.model.DBPSystemObject;
import org.jkiss.dbeaver.model.DBUtils;
import org.jkiss.dbeaver.model.exec.jdbc.JDBCSession;
import org.jkiss.dbeaver.model.meta.Association;
import org.jkiss.dbeaver.model.meta.Property;
import org.jkiss.dbeaver.model.runtime.DBRProgressMonitor;
import org.jkiss.dbeaver.model.struct.DBSDataType;
import org.jkiss.dbeaver.model.struct.DBSEntity;
import org.jkiss.dbeaver.model.struct.DBSObject;
import org.jkiss.dbeaver.model.struct.rdb.DBSSchema;

import java.sql.ResultSet;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public class ScratchBirdSchemaNode extends GenericObjectContainer implements DBSSchema, DBPSystemObject {

    private static final Log log = Log.getLog(ScratchBirdSchemaNode.class);
    private static final String[] PHYSICAL_TABLE_TYPES = {"TABLE", "SYSTEM TABLE"};
    private static final String[] VIEW_TYPES = {"VIEW", "SYSTEM VIEW"};

    private final ScratchBirdCatalog ownerCatalog;
    @Nullable
    private final ScratchBirdSchemaNode parentSchema;
    @NotNull
    private final String name;
    @NotNull
    private final String fullPath;
    @NotNull
    private ScratchBirdSchema querySchema;
    private final boolean catalogBacked;
    private final boolean clientOnly;
    @NotNull
    private final ScratchBirdObjectPath objectPath;
    @NotNull
    private final String objectType;
    @NotNull
    private final Map<String, ScratchBirdSchemaNode> childSchemas = new LinkedHashMap<>();

    public ScratchBirdSchemaNode(
        @NotNull GenericDataSource dataSource,
        @NotNull ScratchBirdCatalog ownerCatalog,
        @Nullable ScratchBirdSchemaNode parentSchema,
        @NotNull String name,
        @NotNull String fullPath
    ) {
        this(dataSource, ownerCatalog, parentSchema, name, fullPath, true, false,
            ScratchBirdObjectPath.fromDisplayPath(fullPath, false), "SCHEMA");
    }

    public ScratchBirdSchemaNode(
        @NotNull GenericDataSource dataSource,
        @NotNull ScratchBirdCatalog ownerCatalog,
        @Nullable ScratchBirdSchemaNode parentSchema,
        @NotNull String name,
        @NotNull String fullPath,
        boolean catalogBacked,
        boolean clientOnly
    ) {
        this(dataSource, ownerCatalog, parentSchema, name, fullPath, catalogBacked, clientOnly,
            ScratchBirdObjectPath.fromDisplayPath(fullPath, clientOnly), "SCHEMA");
    }

    public ScratchBirdSchemaNode(
        @NotNull GenericDataSource dataSource,
        @NotNull ScratchBirdCatalog ownerCatalog,
        @Nullable ScratchBirdSchemaNode parentSchema,
        @NotNull String name,
        @NotNull String fullPath,
        boolean catalogBacked,
        boolean clientOnly,
        @NotNull ScratchBirdObjectPath objectPath,
        @NotNull String objectType
    ) {
        super(dataSource);
        this.ownerCatalog = ownerCatalog;
        this.parentSchema = parentSchema;
        this.name = name;
        this.fullPath = fullPath;
        this.catalogBacked = catalogBacked;
        this.clientOnly = clientOnly;
        this.objectPath = objectPath;
        this.objectType = objectType;
        this.querySchema = new ScratchBirdSchema(dataSource, ownerCatalog, fullPath);
    }

    void addChild(@NotNull ScratchBirdSchemaNode child) {
        childSchemas.putIfAbsent(child.getName(), child);
    }

    void setBackingSchema(@NotNull ScratchBirdSchema backingSchema) {
        this.querySchema = backingSchema;
    }

    @Association
    public Collection<ScratchBirdSchemaNode> getChildSchemas(@NotNull DBRProgressMonitor monitor) {
        return childSchemas.values();
    }

    @NotNull
    public String getFullPath() {
        return fullPath;
    }

    @Property(viewable = true, order = 2)
    public boolean isCatalogBacked() {
        return catalogBacked;
    }

    @Property(viewable = true, order = 3)
    public boolean isClientOnly() {
        return clientOnly;
    }

    @Property(viewable = true, order = 4)
    public boolean isMetricsBranch() {
        return ScratchBirdNamespaceSemantics.isMetricsPath(fullPath);
    }

    @Property(viewable = true, order = 5)
    public boolean isDomainBranch() {
        return ScratchBirdNamespaceSemantics.isDomainPath(fullPath);
    }

    @Property(viewable = true, order = 6)
    public boolean isScratchBirdSystemPath() {
        return ScratchBirdNamespaceSemantics.isSystemPath(fullPath);
    }

    @Property(viewable = true, order = 7)
    public boolean isObjectFoldersVisible() {
        return isObjectContainerBranch();
    }

    @Property(viewable = true, order = 8)
    public boolean isTableFoldersVisible() {
        return isObjectContainerBranch() && !isDomainBranch();
    }

    @Property(viewable = true, order = 9)
    public boolean isViewFoldersVisible() {
        return isTableFoldersVisible();
    }

    @Property(viewable = true, order = 10)
    public boolean isConstraintFoldersVisible() {
        return isTableFoldersVisible();
    }

    @Property(viewable = true, order = 11)
    public boolean isIndexFoldersVisible() {
        return isTableFoldersVisible();
    }

    @Property(viewable = true, order = 12)
    public boolean isSequenceFoldersVisible() {
        return isTableFoldersVisible();
    }

    @Property(viewable = true, order = 13)
    public boolean isDataTypesFolderVisible() {
        return isDomainBranch();
    }

    @Nullable
    @Property(viewable = true, optional = true, order = 14)
    public String getDatabaseUuid() {
        return objectPath.databaseUuid();
    }

    @Nullable
    @Property(viewable = true, optional = true, order = 15)
    public String getObjectUuid() {
        return objectPath.objectUuid();
    }

    @Nullable
    @Property(viewable = true, optional = true, order = 16)
    public String getParentUuid() {
        return objectPath.parentUuid();
    }

    @NotNull
    @Property(viewable = true, order = 17)
    public String getObjectType() {
        return objectType;
    }

    @NotNull
    @Property(viewable = true, order = 18)
    public String getIdentityStatus() {
        return objectPath.identityStatus();
    }

    @Nullable
    @Override
    public GenericCatalog getCatalog() {
        return ownerCatalog;
    }

    @Override
    public ScratchBirdSchema getSchema() {
        return querySchema;
    }

    @NotNull
    @Override
    public GenericStructContainer getObject() {
        return this;
    }

    @NotNull
    @Override
    public String getName() {
        return name;
    }

    @Nullable
    @Override
    public String getDescription() {
        if (isMetricsBranch()) {
            return "DBeaver client-only ScratchBird metrics and report branch";
        }
        if (!catalogBacked) {
            return "DBeaver client-only ScratchBird branch";
        }
        return null;
    }

    @Override
    public DBSObject getParentObject() {
        return parentSchema == null ? ownerCatalog : parentSchema;
    }

    @NotNull
    @Override
    public Class<? extends DBSEntity> getPrimaryChildType(@Nullable DBRProgressMonitor monitor) throws DBException {
        return GenericTable.class;
    }

    @Override
    public List<? extends GenericTableBase> getTables(@NotNull DBRProgressMonitor monitor) throws DBException {
        if (!isTableFoldersVisible()) {
            return Collections.emptyList();
        }
        List<? extends GenericTableBase> tables = loadCachedTables(monitor);
        if (!tables.isEmpty()) {
            return tables;
        }
        return loadAndCacheMetadataTables(monitor);
    }

    @Override
    public GenericTableBase getTable(@NotNull DBRProgressMonitor monitor, @NotNull String name) throws DBException {
        if (!isTableFoldersVisible()) {
            return null;
        }
        GenericTableBase table = loadCachedTable(monitor, name);
        if (table != null) {
            return table;
        }
        loadAndCacheMetadataTables(monitor);
        return getTableCache().getCachedObject(name);
    }

    @Override
    public List<? extends GenericTable> getPhysicalTables(@NotNull DBRProgressMonitor monitor) throws DBException {
        if (!isTableFoldersVisible()) {
            return Collections.emptyList();
        }
        List<GenericTable> physicalTables = new ArrayList<>();
        for (GenericTableBase table : getTables(monitor)) {
            if (table instanceof GenericTable genericTable && table.isPhysicalTable()) {
                physicalTables.add(genericTable);
            }
        }
        return physicalTables;
    }

    @Override
    public List<? extends GenericView> getViews(@NotNull DBRProgressMonitor monitor) throws DBException {
        if (!isViewFoldersVisible()) {
            return Collections.emptyList();
        }
        List<GenericView> views = new ArrayList<>();
        for (GenericTableBase table : getTables(monitor)) {
            if (table instanceof GenericView view) {
                views.add(view);
            }
        }
        return views;
    }

    @Override
    public Collection<? extends DBSDataType> getDataTypes(@NotNull DBRProgressMonitor monitor) throws DBException {
        if (!isDataTypesFolderVisible()) {
            return Collections.emptyList();
        }
        return querySchema.getDataTypes(monitor);
    }

    @Association
    public Collection<GenericUniqueKey> getConstraints(@NotNull DBRProgressMonitor monitor) throws DBException {
        if (!isConstraintFoldersVisible()) {
            return Collections.emptyList();
        }
        return getConstraintKeysCache().getObjects(monitor, this, null);
    }

    @Override
    public Collection<GenericTableIndex> getIndexes(@NotNull DBRProgressMonitor monitor) throws DBException {
        if (!isIndexFoldersVisible()) {
            return Collections.emptyList();
        }
        return super.getIndexes(monitor);
    }

    @Override
    public Collection<? extends DBSObject> getChildren(@NotNull DBRProgressMonitor monitor) throws DBException {
        List<DBSObject> children = new ArrayList<>(childSchemas.values());
        if (isObjectContainerBranch() && !isDomainBranch()) {
            children.addAll(getTables(monitor));
        }
        return children;
    }

    @Override
    public DBSObject getChild(@NotNull DBRProgressMonitor monitor, @NotNull String childName) throws DBException {
        ScratchBirdSchemaNode schemaNode = childSchemas.get(childName);
        if (schemaNode != null) {
            return schemaNode;
        }
        if (!isObjectContainerBranch() || isDomainBranch()) {
            return null;
        }
        return getTable(monitor, childName);
    }

    @Override
    public boolean isSystem() {
        return false;
    }

    private boolean isObjectContainerBranch() {
        if (clientOnly || !catalogBacked || isDomainBranch()) {
            return false;
        }
        return "sys".equals(fullPath) || childSchemas.isEmpty();
    }

    @NotNull
    private List<? extends GenericTableBase> loadCachedTables(@NotNull DBRProgressMonitor monitor) throws DBException {
        try {
            return super.getTables(monitor);
        } catch (DBException e) {
            log.debug("ScratchBird schema table cache is not available for " + fullPath, e);
            return Collections.emptyList();
        }
    }

    @Nullable
    private GenericTableBase loadCachedTable(@NotNull DBRProgressMonitor monitor, @NotNull String name) throws DBException {
        try {
            return super.getTable(monitor, name);
        } catch (DBException e) {
            log.debug("ScratchBird schema table cache lookup is not available for " + fullPath + "." + name, e);
            return null;
        }
    }

    @NotNull
    private List<GenericTableBase> loadAndCacheMetadataTables(@NotNull DBRProgressMonitor monitor) {
        List<GenericTableBase> tables = new ArrayList<>();
        tables.addAll(loadMetadataPhysicalTables(monitor));
        tables.addAll(loadMetadataViews(monitor));
        getTableCache().setCache(tables);
        return tables;
    }

    @NotNull
    private List<GenericTableBase> loadMetadataPhysicalTables(@NotNull DBRProgressMonitor monitor) {
        List<GenericTableBase> tables = new ArrayList<>();
        try (JDBCSession session = DBUtils.openMetaSession(monitor, this, "Load ScratchBird JDBC metadata tables");
             ResultSet resultSet = session.getMetaData().getTables(null, fullPath, "%", PHYSICAL_TABLE_TYPES)) {
            while (resultSet.next()) {
                String tableName = resultSet.getString("TABLE_NAME");
                if (tableName == null || tableName.isEmpty()) {
                    continue;
                }
                tables.add(new ScratchBirdTable(this, tableName, resultSet.getString("TABLE_TYPE"), null));
            }
        } catch (SQLException | DBException e) {
            log.debug("ScratchBird JDBC metadata tables are not available for navigator fallback at " + fullPath, e);
        }
        return tables;
    }

    @NotNull
    private List<GenericTableBase> loadMetadataViews(@NotNull DBRProgressMonitor monitor) {
        List<GenericTableBase> views = new ArrayList<>();
        try (JDBCSession session = DBUtils.openMetaSession(monitor, this, "Load ScratchBird JDBC metadata views");
             ResultSet resultSet = session.getMetaData().getTables(null, fullPath, "%", VIEW_TYPES)) {
            while (resultSet.next()) {
                String viewName = resultSet.getString("TABLE_NAME");
                if (viewName == null || viewName.isEmpty()) {
                    continue;
                }
                views.add(new ScratchBirdView(this, viewName, resultSet.getString("TABLE_TYPE"), null));
            }
        } catch (SQLException | DBException e) {
            log.debug("ScratchBird JDBC metadata views are not available for navigator fallback at " + fullPath, e);
        }
        return views;
    }
}
