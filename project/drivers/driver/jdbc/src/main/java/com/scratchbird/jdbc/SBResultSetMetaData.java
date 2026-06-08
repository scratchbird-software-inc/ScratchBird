// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird JDBC Driver
 * Copyright (c) 2025 ScratchBird Project
 */
package com.scratchbird.jdbc;

import java.sql.*;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/**
 * JDBC ResultSetMetaData implementation for ScratchBird.
 */
public class SBResultSetMetaData implements ResultSetMetaData {

    private final List<SBColumnInfo> columns;
    private final boolean updatable;
    private final Set<Integer> writableColumns;
    private final Set<Integer> autoIncrementColumns;
    private final Map<Integer, String> schemaNamesByColumn;
    private final Map<Integer, String> tableNamesByColumn;
    private final Map<Integer, String> catalogNamesByColumn;
    private final String schemaName;
    private final String tableName;
    private final String catalogName;

    public SBResultSetMetaData(List<SBColumnInfo> columns) {
        this(columns, false, Collections.emptySet(), Collections.emptySet(), "", "", "");
    }

    public SBResultSetMetaData(List<SBColumnInfo> columns, boolean updatable,
                               Set<Integer> writableColumns, String schemaName,
                               String tableName, String catalogName) {
        this(
            columns,
            updatable,
            writableColumns,
            Collections.emptySet(),
            Collections.emptyMap(),
            Collections.emptyMap(),
            Collections.emptyMap(),
            schemaName,
            tableName,
            catalogName
        );
    }

    public SBResultSetMetaData(List<SBColumnInfo> columns, boolean updatable,
                               Set<Integer> writableColumns, Set<Integer> autoIncrementColumns,
                               String schemaName, String tableName, String catalogName) {
        this(
            columns,
            updatable,
            writableColumns,
            autoIncrementColumns,
            Collections.emptyMap(),
            Collections.emptyMap(),
            Collections.emptyMap(),
            schemaName,
            tableName,
            catalogName
        );
    }

    public SBResultSetMetaData(List<SBColumnInfo> columns, boolean updatable,
                               Set<Integer> writableColumns, Set<Integer> autoIncrementColumns,
                               Map<Integer, String> schemaNamesByColumn,
                               Map<Integer, String> tableNamesByColumn,
                               Map<Integer, String> catalogNamesByColumn,
                               String schemaName, String tableName, String catalogName) {
        this.columns = columns;
        this.updatable = updatable;
        this.writableColumns = writableColumns == null
            ? Collections.emptySet()
            : Collections.unmodifiableSet(new HashSet<>(writableColumns));
        this.autoIncrementColumns = autoIncrementColumns == null
            ? Collections.emptySet()
            : Collections.unmodifiableSet(new HashSet<>(autoIncrementColumns));
        this.schemaNamesByColumn = schemaNamesByColumn == null
            ? Collections.emptyMap()
            : Collections.unmodifiableMap(new HashMap<>(schemaNamesByColumn));
        this.tableNamesByColumn = tableNamesByColumn == null
            ? Collections.emptyMap()
            : Collections.unmodifiableMap(new HashMap<>(tableNamesByColumn));
        this.catalogNamesByColumn = catalogNamesByColumn == null
            ? Collections.emptyMap()
            : Collections.unmodifiableMap(new HashMap<>(catalogNamesByColumn));
        this.schemaName = schemaName == null ? "" : schemaName;
        this.tableName = tableName == null ? "" : tableName;
        this.catalogName = catalogName == null ? "" : catalogName;
    }

    private SBColumnInfo getColumn(int column) throws SQLException {
        if (column < 1 || column > columns.size()) {
            throw new SQLException("Column index out of range: " + column, "42703");
        }
        return columns.get(column - 1);
    }

    @Override
    public int getColumnCount() throws SQLException {
        return columns.size();
    }

    @Override
    public boolean isAutoIncrement(int column) throws SQLException {
        getColumn(column);
        return autoIncrementColumns.contains(column);
    }

    @Override
    public boolean isCaseSensitive(int column) throws SQLException {
        int type = getColumnType(column);
        return type == Types.CHAR || type == Types.VARCHAR || type == Types.LONGVARCHAR;
    }

    @Override
    public boolean isSearchable(int column) throws SQLException {
        return true;
    }

    @Override
    public boolean isCurrency(int column) throws SQLException {
        return "money".equalsIgnoreCase(getColumnTypeName(column));
    }

    @Override
    public int isNullable(int column) throws SQLException {
        return getColumn(column).isNullable() ? columnNullable : columnNoNulls;
    }

    @Override
    public boolean isSigned(int column) throws SQLException {
        int type = getColumnType(column);
        return type == Types.SMALLINT || type == Types.INTEGER || type == Types.BIGINT ||
               type == Types.REAL || type == Types.DOUBLE || type == Types.NUMERIC ||
               type == Types.DECIMAL;
    }

    @Override
    public int getColumnDisplaySize(int column) throws SQLException {
        int type = getColumnType(column);
        int precision = getPrecision(column);
        String typeName = getColumnTypeName(column).toLowerCase(Locale.ROOT);
        switch (type) {
            case Types.BOOLEAN: return 5;
            case Types.SMALLINT: return 6;
            case Types.INTEGER: return 11;
            case Types.BIGINT: return 20;
            case Types.REAL: return 14;
            case Types.DOUBLE: return 24;
            case Types.NUMERIC:
            case Types.DECIMAL:
                if (precision > 0) {
                    int scale = getScale(column);
                    int size = precision;
                    if (scale > 0) {
                        size += 1; // decimal point
                    }
                    return size + 1; // sign
                }
                return 40;
            case Types.DATE: return 10;
            case Types.TIME: return 8;
            case Types.TIMESTAMP: return 29;
            case Types.CHAR:
            case Types.VARCHAR:
            case Types.LONGVARCHAR:
                if (precision > 0) {
                    return precision;
                }
                return typeName.contains("text") ? 65535 : 255;
            default: return 255;
        }
    }

    @Override
    public String getColumnLabel(int column) throws SQLException {
        return getColumn(column).getName();
    }

    @Override
    public String getColumnName(int column) throws SQLException {
        return getColumn(column).getName();
    }

    @Override
    public String getSchemaName(int column) throws SQLException {
        getColumn(column);
        return valueForColumn(schemaNamesByColumn, column, schemaName);
    }

    @Override
    public int getPrecision(int column) throws SQLException {
        SBColumnInfo col = getColumn(column);
        int oid = col.getTypeOid();
        int modifier = col.getTypeModifier();
        if (modifier <= 4) {
            return defaultPrecisionForType(oid);
        }
        int baseModifier = modifier - 4;
        switch (oid) {
            case 1043: // varchar(n)
            case 1042: // bpchar(n)
                return baseModifier;
            case 1700: // numeric(p,s)
                int precision = (baseModifier >> 16) & 0xFFFF;
                return precision > 0 ? precision : defaultPrecisionForType(oid);
            default:
                return defaultPrecisionForType(oid);
        }
    }

    @Override
    public int getScale(int column) throws SQLException {
        SBColumnInfo col = getColumn(column);
        int modifier = col.getTypeModifier();
        if (modifier > 4 && col.getTypeOid() == 1700) {
            return (modifier - 4) & 0xFFFF;
        }
        return defaultScaleForType(col.getTypeOid());
    }

    @Override
    public String getTableName(int column) throws SQLException {
        getColumn(column);
        return valueForColumn(tableNamesByColumn, column, tableName);
    }

    @Override
    public String getCatalogName(int column) throws SQLException {
        getColumn(column);
        return valueForColumn(catalogNamesByColumn, column, catalogName);
    }

    @Override
    public int getColumnType(int column) throws SQLException {
        SBColumnInfo col = getColumn(column);
        return mapOidToSqlType(col.getTypeOid());
    }

    @Override
    public String getColumnTypeName(int column) throws SQLException {
        SBColumnInfo col = getColumn(column);
        return mapOidToTypeName(col.getTypeOid());
    }

    @Override
    public boolean isReadOnly(int column) throws SQLException {
        return !isWritable(column);
    }

    @Override
    public boolean isWritable(int column) throws SQLException {
        if (!updatable) {
            return false;
        }
        if (!writableColumns.isEmpty()) {
            return writableColumns.contains(column);
        }
        SBColumnInfo col = getColumn(column);
        return col != null && col.getTableOid() > 0 && col.getColumnNumber() > 0;
    }

    @Override
    public boolean isDefinitelyWritable(int column) throws SQLException {
        return isWritable(column);
    }

    @Override
    public String getColumnClassName(int column) throws SQLException {
        int type = getColumnType(column);
        switch (type) {
            case Types.BOOLEAN: return Boolean.class.getName();
            case Types.SMALLINT: return Short.class.getName();
            case Types.INTEGER: return Integer.class.getName();
            case Types.BIGINT: return Long.class.getName();
            case Types.REAL: return Float.class.getName();
            case Types.DOUBLE: return Double.class.getName();
            case Types.NUMERIC:
            case Types.DECIMAL: return java.math.BigDecimal.class.getName();
            case Types.CHAR:
            case Types.VARCHAR:
            case Types.LONGVARCHAR: return String.class.getName();
            case Types.BINARY:
            case Types.VARBINARY:
            case Types.LONGVARBINARY: return byte[].class.getName();
            case Types.DATE: return java.sql.Date.class.getName();
            case Types.TIME: return java.sql.Time.class.getName();
            case Types.TIMESTAMP: return java.sql.Timestamp.class.getName();
            case Types.TIMESTAMP_WITH_TIMEZONE: return java.time.OffsetDateTime.class.getName();
            case Types.ARRAY: return java.sql.Array.class.getName();
            case Types.OTHER:
                String typeName = getColumnTypeName(column).toLowerCase(Locale.ROOT);
                if (typeName.contains("uuid")) {
                    return java.util.UUID.class.getName();
                }
                if (typeName.contains("json")) {
                    return String.class.getName();
                }
                return Object.class.getName();
            default: return Object.class.getName();
        }
    }

    @Override
    public <T> T unwrap(Class<T> iface) throws SQLException {
        if (iface.isAssignableFrom(getClass())) {
            return iface.cast(this);
        }
        throw new SQLException("Cannot unwrap to " + iface.getName(), "0A000");
    }

    @Override
    public boolean isWrapperFor(Class<?> iface) throws SQLException {
        return iface.isAssignableFrom(getClass());
    }

    private static String valueForColumn(Map<Integer, String> valuesByColumn, int column, String fallback) {
        String value = valuesByColumn.get(column);
        return value != null ? value : fallback;
    }

    // ==================== Type Mapping ====================

    private int mapOidToSqlType(int oid) {
        switch (oid) {
            case 16: return Types.BOOLEAN;       // bool
            case 21: return Types.SMALLINT;      // int2
            case 23: return Types.INTEGER;       // int4
            case 20: return Types.BIGINT;        // int8
            case 700: return Types.REAL;         // float4
            case 701: return Types.DOUBLE;       // float8
            case 1700: return Types.NUMERIC;     // numeric
            case 790: return Types.NUMERIC;      // money
            case 18: return Types.CHAR;          // char
            case 25: return Types.VARCHAR;       // text
            case 1042: return Types.CHAR;        // bpchar
            case 1043: return Types.VARCHAR;     // varchar
            case 17: return Types.VARBINARY;     // bytea
            case 1082: return Types.DATE;        // date
            case 1083: return Types.TIME;        // time
            case 1114: return Types.TIMESTAMP;   // timestamp
            case 1184: return Types.TIMESTAMP_WITH_TIMEZONE; // timestamptz
            case 28: return Types.BIGINT;        // xid
            case 2950: return Types.OTHER;       // uuid
            case 114: return Types.OTHER;        // json
            case 3802: return Types.OTHER;       // jsonb
            case 1007: return Types.ARRAY;       // int4[]
            case 1009: return Types.ARRAY;       // text[]
            case 869: return Types.OTHER;        // inet
            case 650: return Types.OTHER;        // cidr
            case 829: return Types.OTHER;        // macaddr
            default: return Types.OTHER;
        }
    }

    private String mapOidToTypeName(int oid) {
        switch (oid) {
            case 16: return "boolean";
            case 21: return "smallint";
            case 23: return "integer";
            case 20: return "bigint";
            case 700: return "real";
            case 701: return "double precision";
            case 1700: return "numeric";
            case 790: return "money";
            case 18: return "char";
            case 25: return "text";
            case 1042: return "bpchar";
            case 1043: return "varchar";
            case 17: return "bytea";
            case 1082: return "date";
            case 1083: return "time";
            case 1114: return "timestamp";
            case 1184: return "timestamptz";
            case 28: return "xid";
            case 2950: return "uuid";
            case 114: return "json";
            case 3802: return "jsonb";
            case 1007: return "integer[]";
            case 1009: return "text[]";
            case 869: return "inet";
            case 650: return "cidr";
            case 829: return "macaddr";
            default: return "unknown";
        }
    }

    private int defaultPrecisionForType(int oid) {
        return switch (oid) {
            case 16 -> 1;        // bool
            case 21 -> 5;        // int2
            case 23 -> 10;       // int4
            case 20 -> 19;       // int8
            case 700 -> 24;      // float4
            case 701 -> 53;      // float8
            case 1700, 790 -> 38; // numeric, money
            case 18 -> 1;        // char
            case 25 -> 65535;    // text
            case 1042, 1043 -> 255; // bpchar, varchar (when typmod absent)
            case 1082 -> 10;     // date
            case 1083 -> 8;      // time
            case 1114, 1184 -> 29; // timestamp
            case 28 -> 10;       // xid
            case 2950 -> 36;     // uuid
            default -> 0;
        };
    }

    private int defaultScaleForType(int oid) {
        return switch (oid) {
            case 1700 -> 0;      // numeric
            case 790 -> 2;       // money
            case 700 -> 6;       // float4
            case 701 -> 15;      // float8
            default -> 0;
        };
    }
}
