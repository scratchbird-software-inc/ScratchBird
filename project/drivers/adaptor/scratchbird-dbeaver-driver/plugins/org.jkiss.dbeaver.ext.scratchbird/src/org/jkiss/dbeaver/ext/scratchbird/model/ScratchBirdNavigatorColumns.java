// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package org.jkiss.dbeaver.ext.scratchbird.model;

import org.jkiss.code.NotNull;
import org.jkiss.dbeaver.DBException;
import org.jkiss.dbeaver.ext.generic.model.GenericTableBase;
import org.jkiss.dbeaver.ext.generic.model.GenericTableColumn;
import org.jkiss.dbeaver.model.DBUtils;
import org.jkiss.dbeaver.model.exec.jdbc.JDBCSession;
import org.jkiss.dbeaver.model.runtime.DBRProgressMonitor;
import org.jkiss.utils.CommonUtils;

import java.sql.DatabaseMetaData;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

final class ScratchBirdNavigatorColumns {

    private ScratchBirdNavigatorColumns() {
    }

    @NotNull
    static List<GenericTableColumn> load(
        @NotNull DBRProgressMonitor monitor,
        @NotNull GenericTableBase table,
        @NotNull String authorityPath
    ) throws DBException {
        String schemaPath = ScratchBirdQualifiedNames.parentPath(authorityPath);
        String tableName = table.getName();
        if (CommonUtils.isEmpty(schemaPath) || CommonUtils.isEmpty(tableName)) {
            return List.of();
        }

        List<GenericTableColumn> columns = new ArrayList<>();
        Set<String> emittedNames = new LinkedHashSet<>();
        try (JDBCSession session = DBUtils.openMetaSession(monitor, table, "Load ScratchBird navigator columns");
             ResultSet resultSet = session.getMetaData().getColumns(null, schemaPath, tableName, "%")) {
            while (resultSet.next()) {
                String columnName = resultSet.getString("COLUMN_NAME");
                if (CommonUtils.isEmpty(columnName) || !emittedNames.add(columnName.toLowerCase())) {
                    continue;
                }
                int jdbcType = resultSet.getInt("DATA_TYPE");
                String typeName = resultSet.getString("TYPE_NAME");
                int ordinal = resultSet.getInt("ORDINAL_POSITION");
                int precision = resultSet.getInt("COLUMN_SIZE");
                int scale = resultSet.getInt("DECIMAL_DIGITS");
                int radix = resultSet.getInt("NUM_PREC_RADIX");
                int nullable = resultSet.getInt("NULLABLE");
                int charOctetLength = resultSet.getInt("CHAR_OCTET_LENGTH");
                String remarks = resultSet.getString("REMARKS");
                String defaultValue = resultSet.getString("COLUMN_DEF");
                String generated = resultSet.getString("IS_GENERATEDCOLUMN");
                String autoIncrement = resultSet.getString("IS_AUTOINCREMENT");

                columns.add(new GenericTableColumn(
                    table,
                    columnName,
                    typeName,
                    jdbcType,
                    jdbcType,
                    ordinal,
                    Math.max(0, precision),
                    Math.max(0, charOctetLength),
                    scale <= 0 ? null : scale,
                    precision <= 0 ? null : precision,
                    radix,
                    nullable == DatabaseMetaData.columnNoNulls,
                    remarks,
                    defaultValue,
                    "YES".equalsIgnoreCase(generated),
                    "YES".equalsIgnoreCase(autoIncrement)));
            }
        } catch (SQLException e) {
            throw new DBException("ScratchBird navigator columns are not available for " + authorityPath, e);
        }
        return columns;
    }
}
