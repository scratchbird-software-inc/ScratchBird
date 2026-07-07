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
import org.jkiss.dbeaver.ext.generic.model.GenericCatalog;
import org.jkiss.dbeaver.ext.generic.model.GenericDataSource;
import org.jkiss.dbeaver.ext.generic.model.GenericSequence;
import org.jkiss.dbeaver.ext.generic.model.GenericSchema;
import org.jkiss.dbeaver.ext.generic.model.GenericStructContainer;
import org.jkiss.dbeaver.ext.generic.model.GenericTableBase;
import org.jkiss.dbeaver.ext.generic.model.meta.GenericMetaModel;
import org.jkiss.dbeaver.model.DBPDataSourceContainer;
import org.jkiss.dbeaver.model.exec.jdbc.JDBCPreparedStatement;
import org.jkiss.dbeaver.model.exec.jdbc.JDBCResultSet;
import org.jkiss.dbeaver.model.exec.jdbc.JDBCSession;
import org.jkiss.dbeaver.model.exec.jdbc.JDBCStatement;
import org.jkiss.dbeaver.model.impl.jdbc.JDBCUtils;
import org.jkiss.dbeaver.model.runtime.DBRProgressMonitor;
import org.jkiss.utils.CommonUtils;

import java.sql.SQLException;

public class ScratchBirdMetaModel extends GenericMetaModel {

    @NotNull
    @Override
    public GenericDataSource createDataSourceImpl(
        @NotNull DBRProgressMonitor monitor,
        @NotNull DBPDataSourceContainer container
    ) throws DBException {
        return new ScratchBirdDataSource(monitor, container, this, new ScratchBirdSQLDialect());
    }

    @Override
    public GenericCatalog createCatalogImpl(@NotNull GenericDataSource dataSource, @NotNull String catalogName) {
        return new ScratchBirdCatalog(dataSource, catalogName);
    }

    @Override
    public boolean useCatalogInObjectNames() {
        return false;
    }

    @Override
    public boolean isSystemSchema(@NotNull GenericSchema schema) {
        return ScratchBirdNamespaceSemantics.isSystemPath(schema.getName());
    }

    @Override
    public GenericSchema createSchemaImpl(
        @NotNull GenericDataSource dataSource,
        @Nullable GenericCatalog catalog,
        @NotNull String schemaName
    ) throws DBException {
        return new ScratchBirdSchema(dataSource, catalog, schemaName);
    }

    @NotNull
    @Override
    public GenericTableBase createTableOrViewImpl(
        @NotNull GenericStructContainer container,
        @Nullable String tableName,
        @Nullable String tableType,
        @Nullable JDBCResultSet dbResult
    ) {
        if (tableType != null && isView(tableType)) {
            return new ScratchBirdView(container, tableName, tableType, dbResult);
        }
        return new ScratchBirdTable(container, tableName, tableType, dbResult);
    }

    @Override
    public boolean supportsSequences(@NotNull GenericDataSource dataSource) {
        return true;
    }

    @Override
    public JDBCStatement prepareSequencesLoadStatement(
        @NotNull JDBCSession session,
        @NotNull GenericStructContainer container
    ) throws SQLException {
        JDBCPreparedStatement statement = session.prepareStatement(
            "SELECT SEQUENCE_SCHEMA, SEQUENCE_NAME, START_VALUE, MINIMUM_VALUE, MAXIMUM_VALUE, INCREMENT " +
                "FROM information_schema.sequences " +
                "WHERE sequence_schema = ? " +
                "ORDER BY sequence_name");
        String schemaName = container.getSchema() == null ? container.getName() : container.getSchema().getName();
        statement.setString(1, schemaName);
        return statement;
    }

    @Override
    public GenericSequence createSequenceImpl(
        @NotNull JDBCSession session,
        @NotNull GenericStructContainer container,
        @NotNull JDBCResultSet dbResult
    ) {
        String name = JDBCUtils.safeGetString(dbResult, "SEQUENCE_NAME");
        if (CommonUtils.isEmpty(name)) {
            return null;
        }

        return new GenericSequence(
            container,
            name,
            null,
            JDBCUtils.safeGetLong(dbResult, "START_VALUE"),
            JDBCUtils.safeGetLong(dbResult, "MINIMUM_VALUE"),
            JDBCUtils.safeGetLong(dbResult, "MAXIMUM_VALUE"),
            JDBCUtils.safeGetLong(dbResult, "INCREMENT")
        );
    }
}
