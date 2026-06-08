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

import java.io.*;
import java.math.*;
import java.net.*;
import java.sql.*;
import java.time.Instant;
import java.time.LocalDate;
import java.time.LocalDateTime;
import java.time.LocalTime;
import java.time.OffsetDateTime;
import java.time.OffsetTime;
import java.time.ZonedDateTime;
import java.time.ZoneOffset;
import java.util.*;
import java.util.regex.Pattern;

/**
 * JDBC CallableStatement implementation for ScratchBird.
 */
public class SBCallableStatement extends SBPreparedStatement implements CallableStatement {

    private static final Pattern FUNCTION_ESCAPE_PATTERN =
        Pattern.compile("(?is)^\\?\\s*=\\s*call\\s+(.+)$");
    private static final Pattern PROCEDURE_ESCAPE_PATTERN =
        Pattern.compile("(?is)^call\\s+(.+)$");

    private final Map<Integer, Object> outParameters = new HashMap<>();
    private final Map<Integer, Integer> outParameterSqlTypes = new HashMap<>();
    private final Map<String, Integer> namedParameters = new HashMap<>();
    private final boolean functionReturnCallSyntax;
    private boolean lastOutParameterWasNull = false;

    public SBCallableStatement(SBConnection connection, String sql, int resultSetType,
                               int resultSetConcurrency, int resultSetHoldability)
            throws SQLException {
        this(connection, rewriteCallableSql(sql), resultSetType, resultSetConcurrency, resultSetHoldability);
    }

    private SBCallableStatement(SBConnection connection, CallableSqlRewrite rewrite,
                                int resultSetType, int resultSetConcurrency, int resultSetHoldability)
            throws SQLException {
        super(connection, rewrite.sql(), resultSetType, resultSetConcurrency, resultSetHoldability);
        this.functionReturnCallSyntax = rewrite.functionReturnCall();
    }

    private record CallableSqlRewrite(String sql, boolean functionReturnCall) {}

    private static CallableSqlRewrite rewriteCallableSql(String sql) {
        if (sql == null) {
            return new CallableSqlRewrite(null, false);
        }
        String trimmed = sql.trim();
        if (!trimmed.startsWith("{") || !trimmed.endsWith("}")) {
            return new CallableSqlRewrite(sql, false);
        }

        String inner = trimmed.substring(1, trimmed.length() - 1).trim();
        var functionMatch = FUNCTION_ESCAPE_PATTERN.matcher(inner);
        if (functionMatch.matches()) {
            String target = functionMatch.group(1).trim();
            return new CallableSqlRewrite("SELECT " + target, true);
        }

        var procedureMatch = PROCEDURE_ESCAPE_PATTERN.matcher(inner);
        if (procedureMatch.matches()) {
            String target = procedureMatch.group(1).trim();
            return new CallableSqlRewrite("CALL " + target, false);
        }

        return new CallableSqlRewrite(sql, false);
    }

    @Override
    protected int logicalParameterCount() {
        if (functionReturnCallSyntax) {
            return parameterCount + 1;
        }
        return parameterCount;
    }

    @Override
    protected int mapParameterIndex(int parameterIndex) throws SQLException {
        if (!functionReturnCallSyntax) {
            return parameterIndex;
        }
        if (parameterIndex == 1) {
            throw new SQLException("Parameter 1 is function return value and cannot be bound", "07009");
        }
        return parameterIndex - 1;
    }

    @Override
    public boolean execute() throws SQLException {
        boolean hasResultSet = super.execute();
        hydrateRegisteredOutParameters();
        return hasResultSet;
    }

    @Override
    public void registerOutParameter(int parameterIndex, int sqlType) throws SQLException {
        outParameters.put(parameterIndex, null);
        outParameterSqlTypes.put(parameterIndex, sqlType);
    }

    @Override
    public void registerOutParameter(int parameterIndex, int sqlType, int scale) throws SQLException {
        registerOutParameter(parameterIndex, sqlType);
    }

    @Override
    public void registerOutParameter(int parameterIndex, int sqlType, String typeName) throws SQLException {
        registerOutParameter(parameterIndex, sqlType);
    }

    @Override
    public void registerOutParameter(String parameterName, int sqlType) throws SQLException {
        int index = getParameterIndex(parameterName);
        registerOutParameter(index, sqlType);
    }

    @Override
    public void registerOutParameter(String parameterName, int sqlType, int scale) throws SQLException {
        registerOutParameter(parameterName, sqlType);
    }

    @Override
    public void registerOutParameter(String parameterName, int sqlType, String typeName) throws SQLException {
        registerOutParameter(parameterName, sqlType);
    }

    @Override
    public void registerOutParameter(int parameterIndex, SQLType sqlType) throws SQLException {
        registerOutParameter(parameterIndex, sqlTypeCode(sqlType));
    }

    @Override
    public void registerOutParameter(int parameterIndex, SQLType sqlType, int scale) throws SQLException {
        registerOutParameter(parameterIndex, sqlTypeCode(sqlType), scale);
    }

    @Override
    public void registerOutParameter(int parameterIndex, SQLType sqlType, String typeName) throws SQLException {
        registerOutParameter(parameterIndex, sqlTypeCode(sqlType), typeName);
    }

    @Override
    public void registerOutParameter(String parameterName, SQLType sqlType) throws SQLException {
        registerOutParameter(parameterName, sqlTypeCode(sqlType));
    }

    @Override
    public void registerOutParameter(String parameterName, SQLType sqlType, int scale) throws SQLException {
        registerOutParameter(parameterName, sqlTypeCode(sqlType), scale);
    }

    @Override
    public void registerOutParameter(String parameterName, SQLType sqlType, String typeName) throws SQLException {
        registerOutParameter(parameterName, sqlTypeCode(sqlType), typeName);
    }

    @Override
    public boolean wasNull() throws SQLException {
        checkClosed();
        return lastOutParameterWasNull;
    }

    private Object readOutParameter(int parameterIndex) {
        Object value = outParameters.get(parameterIndex);
        lastOutParameterWasNull = (value == null);
        return value;
    }

    private void hydrateRegisteredOutParameters() throws SQLException {
        if (outParameters.isEmpty() || currentResultSet == null) {
            return;
        }
        Object[] firstRow = currentResultSet.firstBufferedRowSnapshot();
        if (firstRow != null) {
            for (Integer index : new TreeSet<>(outParameters.keySet())) {
                Integer sqlType = outParameterSqlTypes.get(index);
                if (sqlType != null && sqlType == Types.REF_CURSOR) {
                    outParameters.put(index, currentResultSet);
                    continue;
                }
                int rowOffset = index - 1;
                if (rowOffset >= 0 && rowOffset < firstRow.length) {
                    outParameters.put(index, firstRow[rowOffset]);
                } else {
                    outParameters.put(index, null);
                }
            }
            return;
        }
        boolean hasRow = currentResultSet.next();
        if (!hasRow) {
            return;
        }
        for (Integer index : new TreeSet<>(outParameters.keySet())) {
            Integer sqlType = outParameterSqlTypes.get(index);
            if (sqlType != null && sqlType == Types.REF_CURSOR) {
                outParameters.put(index, currentResultSet);
                continue;
            }
            try {
                outParameters.put(index, currentResultSet.getObject(index));
            } catch (SQLException ex) {
                outParameters.put(index, null);
            }
        }
        try {
            currentResultSet.beforeFirst();
        } catch (SQLException ignored) {
            // Forward-only result sets cannot be rewound.
        }
    }

    // Getters by index
    @Override
    public String getString(int parameterIndex) throws SQLException {
        Object value = readOutParameter(parameterIndex);
        return value != null ? value.toString() : null;
    }

    @Override
    public boolean getBoolean(int parameterIndex) throws SQLException {
        Object value = readOutParameter(parameterIndex);
        if (value == null) return false;
        if (value instanceof Boolean) return (Boolean) value;
        return Boolean.parseBoolean(value.toString());
    }

    @Override
    public byte getByte(int parameterIndex) throws SQLException {
        Object value = readOutParameter(parameterIndex);
        if (value == null) return 0;
        if (value instanceof Number) return ((Number) value).byteValue();
        return Byte.parseByte(value.toString());
    }

    @Override
    public short getShort(int parameterIndex) throws SQLException {
        Object value = readOutParameter(parameterIndex);
        if (value == null) return 0;
        if (value instanceof Number) return ((Number) value).shortValue();
        return Short.parseShort(value.toString());
    }

    @Override
    public int getInt(int parameterIndex) throws SQLException {
        Object value = readOutParameter(parameterIndex);
        if (value == null) return 0;
        if (value instanceof Number) return ((Number) value).intValue();
        return Integer.parseInt(value.toString());
    }

    @Override
    public long getLong(int parameterIndex) throws SQLException {
        Object value = readOutParameter(parameterIndex);
        if (value == null) return 0;
        if (value instanceof Number) return ((Number) value).longValue();
        return Long.parseLong(value.toString());
    }

    @Override
    public float getFloat(int parameterIndex) throws SQLException {
        Object value = readOutParameter(parameterIndex);
        if (value == null) return 0;
        if (value instanceof Number) return ((Number) value).floatValue();
        return Float.parseFloat(value.toString());
    }

    @Override
    public double getDouble(int parameterIndex) throws SQLException {
        Object value = readOutParameter(parameterIndex);
        if (value == null) return 0;
        if (value instanceof Number) return ((Number) value).doubleValue();
        return Double.parseDouble(value.toString());
    }

    @Override
    @Deprecated
    public BigDecimal getBigDecimal(int parameterIndex, int scale) throws SQLException {
        BigDecimal bd = getBigDecimal(parameterIndex);
        return bd != null ? bd.setScale(scale, RoundingMode.HALF_UP) : null;
    }

    @Override
    public byte[] getBytes(int parameterIndex) throws SQLException {
        Object value = readOutParameter(parameterIndex);
        if (value == null) return null;
        if (value instanceof byte[]) return (byte[]) value;
        return value.toString().getBytes();
    }

    @Override
    public java.sql.Date getDate(int parameterIndex) throws SQLException {
        Object value = readOutParameter(parameterIndex);
        if (value == null) return null;
        if (value instanceof java.sql.Date) return (java.sql.Date) value;
        return java.sql.Date.valueOf(value.toString());
    }

    @Override
    public Time getTime(int parameterIndex) throws SQLException {
        Object value = readOutParameter(parameterIndex);
        if (value == null) return null;
        if (value instanceof Time) return (Time) value;
        return Time.valueOf(value.toString());
    }

    @Override
    public Timestamp getTimestamp(int parameterIndex) throws SQLException {
        Object value = readOutParameter(parameterIndex);
        if (value == null) return null;
        if (value instanceof Timestamp) return (Timestamp) value;
        return Timestamp.valueOf(value.toString());
    }

    @Override
    public Object getObject(int parameterIndex) throws SQLException {
        return readOutParameter(parameterIndex);
    }

    @Override
    public BigDecimal getBigDecimal(int parameterIndex) throws SQLException {
        Object value = readOutParameter(parameterIndex);
        if (value == null) return null;
        if (value instanceof BigDecimal) return (BigDecimal) value;
        return new BigDecimal(value.toString());
    }

    @Override
    public Object getObject(int parameterIndex, Map<String, Class<?>> map) throws SQLException {
        Object value = getObject(parameterIndex);
        if (value == null || map == null || map.isEmpty()) {
            return value;
        }
        if (!(value instanceof Struct || value instanceof Object[] || value instanceof Collection<?>)) {
            return value;
        }
        try (SBResultSet bridge = bridgeSingleValueResultSet(value)) {
            if (!bridge.next()) {
                return null;
            }
            return bridge.getObject(1, map);
        }
    }

    @Override
    public Ref getRef(int parameterIndex) throws SQLException {
        return SBRef.fromObject(readOutParameter(parameterIndex));
    }

    @Override
    public Blob getBlob(int parameterIndex) throws SQLException {
        byte[] bytes = getBytes(parameterIndex);
        return bytes != null ? new SBBlob(bytes) : null;
    }

    @Override
    public Clob getClob(int parameterIndex) throws SQLException {
        String s = getString(parameterIndex);
        return s != null ? new SBClob(s) : null;
    }

    @Override
    public Array getArray(int parameterIndex) throws SQLException {
        Object value = readOutParameter(parameterIndex);
        if (value == null) return null;
        if (value instanceof Array) return (Array) value;
        if (value instanceof Object[] elements) {
            return new SBArray(inferArrayBaseType(elements), elements);
        }
        if (value instanceof Collection<?> collection) {
            Object[] elements = collection.toArray();
            return new SBArray(inferArrayBaseType(elements), elements);
        }
        if (value instanceof String text) {
            Object[] elements = parseArrayLiteral(text);
            return new SBArray(inferArrayBaseType(elements), elements);
        }
        if (value.getClass().isArray()) {
            int len = java.lang.reflect.Array.getLength(value);
            Object[] elements = new Object[len];
            for (int i = 0; i < len; i++) {
                elements[i] = java.lang.reflect.Array.get(value, i);
            }
            return new SBArray(inferArrayBaseType(elements), elements);
        }
        throw new SQLException("Value cannot be converted to SQL ARRAY", "HY000");
    }

    @Override
    public java.sql.Date getDate(int parameterIndex, Calendar cal) throws SQLException {
        return getDate(parameterIndex);
    }

    @Override
    public Time getTime(int parameterIndex, Calendar cal) throws SQLException {
        return getTime(parameterIndex);
    }

    @Override
    public Timestamp getTimestamp(int parameterIndex, Calendar cal) throws SQLException {
        return getTimestamp(parameterIndex);
    }

    @Override
    public URL getURL(int parameterIndex) throws SQLException {
        String s = getString(parameterIndex);
        if (s == null) return null;
        try {
            return new URL(s);
        } catch (MalformedURLException e) {
            throw new SQLException("Invalid URL: " + s, "HY000", e);
        }
    }

    // Getters by name
    @Override
    public String getString(String parameterName) throws SQLException {
        return getString(getParameterIndex(parameterName));
    }

    @Override
    public boolean getBoolean(String parameterName) throws SQLException {
        return getBoolean(getParameterIndex(parameterName));
    }

    @Override
    public byte getByte(String parameterName) throws SQLException {
        return getByte(getParameterIndex(parameterName));
    }

    @Override
    public short getShort(String parameterName) throws SQLException {
        return getShort(getParameterIndex(parameterName));
    }

    @Override
    public int getInt(String parameterName) throws SQLException {
        return getInt(getParameterIndex(parameterName));
    }

    @Override
    public long getLong(String parameterName) throws SQLException {
        return getLong(getParameterIndex(parameterName));
    }

    @Override
    public float getFloat(String parameterName) throws SQLException {
        return getFloat(getParameterIndex(parameterName));
    }

    @Override
    public double getDouble(String parameterName) throws SQLException {
        return getDouble(getParameterIndex(parameterName));
    }

    @Override
    public byte[] getBytes(String parameterName) throws SQLException {
        return getBytes(getParameterIndex(parameterName));
    }

    @Override
    public java.sql.Date getDate(String parameterName) throws SQLException {
        return getDate(getParameterIndex(parameterName));
    }

    @Override
    public Time getTime(String parameterName) throws SQLException {
        return getTime(getParameterIndex(parameterName));
    }

    @Override
    public Timestamp getTimestamp(String parameterName) throws SQLException {
        return getTimestamp(getParameterIndex(parameterName));
    }

    @Override
    public Object getObject(String parameterName) throws SQLException {
        return getObject(getParameterIndex(parameterName));
    }

    @Override
    public BigDecimal getBigDecimal(String parameterName) throws SQLException {
        return getBigDecimal(getParameterIndex(parameterName));
    }

    @Override
    public Object getObject(String parameterName, Map<String, Class<?>> map) throws SQLException {
        return getObject(getParameterIndex(parameterName), map);
    }

    @Override
    public Ref getRef(String parameterName) throws SQLException {
        return getRef(getParameterIndex(parameterName));
    }

    @Override
    public Blob getBlob(String parameterName) throws SQLException {
        return getBlob(getParameterIndex(parameterName));
    }

    @Override
    public Clob getClob(String parameterName) throws SQLException {
        return getClob(getParameterIndex(parameterName));
    }

    @Override
    public Array getArray(String parameterName) throws SQLException {
        return getArray(getParameterIndex(parameterName));
    }

    @Override
    public java.sql.Date getDate(String parameterName, Calendar cal) throws SQLException {
        return getDate(getParameterIndex(parameterName), cal);
    }

    @Override
    public Time getTime(String parameterName, Calendar cal) throws SQLException {
        return getTime(getParameterIndex(parameterName), cal);
    }

    @Override
    public Timestamp getTimestamp(String parameterName, Calendar cal) throws SQLException {
        return getTimestamp(getParameterIndex(parameterName), cal);
    }

    @Override
    public URL getURL(String parameterName) throws SQLException {
        return getURL(getParameterIndex(parameterName));
    }

    // Setters by name
    @Override
    public void setNull(String parameterName, int sqlType) throws SQLException {
        setNull(getParameterIndex(parameterName), sqlType);
    }

    @Override
    public void setNull(String parameterName, int sqlType, String typeName) throws SQLException {
        setNull(getParameterIndex(parameterName), sqlType, typeName);
    }

    @Override
    public void setBoolean(String parameterName, boolean x) throws SQLException {
        setBoolean(getParameterIndex(parameterName), x);
    }

    @Override
    public void setByte(String parameterName, byte x) throws SQLException {
        setByte(getParameterIndex(parameterName), x);
    }

    @Override
    public void setShort(String parameterName, short x) throws SQLException {
        setShort(getParameterIndex(parameterName), x);
    }

    @Override
    public void setInt(String parameterName, int x) throws SQLException {
        setInt(getParameterIndex(parameterName), x);
    }

    @Override
    public void setLong(String parameterName, long x) throws SQLException {
        setLong(getParameterIndex(parameterName), x);
    }

    @Override
    public void setFloat(String parameterName, float x) throws SQLException {
        setFloat(getParameterIndex(parameterName), x);
    }

    @Override
    public void setDouble(String parameterName, double x) throws SQLException {
        setDouble(getParameterIndex(parameterName), x);
    }

    @Override
    public void setBigDecimal(String parameterName, BigDecimal x) throws SQLException {
        setBigDecimal(getParameterIndex(parameterName), x);
    }

    @Override
    public void setString(String parameterName, String x) throws SQLException {
        setString(getParameterIndex(parameterName), x);
    }

    @Override
    public void setBytes(String parameterName, byte[] x) throws SQLException {
        setBytes(getParameterIndex(parameterName), x);
    }

    @Override
    public void setDate(String parameterName, java.sql.Date x) throws SQLException {
        setDate(getParameterIndex(parameterName), x);
    }

    @Override
    public void setTime(String parameterName, Time x) throws SQLException {
        setTime(getParameterIndex(parameterName), x);
    }

    @Override
    public void setTimestamp(String parameterName, Timestamp x) throws SQLException {
        setTimestamp(getParameterIndex(parameterName), x);
    }

    @Override
    public void setAsciiStream(String parameterName, InputStream x, int length) throws SQLException {
        setAsciiStream(getParameterIndex(parameterName), x, length);
    }

    @Override
    public void setBinaryStream(String parameterName, InputStream x, int length) throws SQLException {
        setBinaryStream(getParameterIndex(parameterName), x, length);
    }

    @Override
    public void setObject(String parameterName, Object x, int targetSqlType, int scale) throws SQLException {
        setObject(getParameterIndex(parameterName), x, targetSqlType, scale);
    }

    @Override
    public void setObject(String parameterName, Object x, SQLType targetSqlType, int scaleOrLength)
            throws SQLException {
        if (targetSqlType == null) {
            setObject(parameterName, x);
            return;
        }
        setObject(parameterName, x, sqlTypeCode(targetSqlType), scaleOrLength);
    }

    @Override
    public void setObject(String parameterName, Object x, int targetSqlType) throws SQLException {
        setObject(getParameterIndex(parameterName), x, targetSqlType);
    }

    @Override
    public void setObject(String parameterName, Object x, SQLType targetSqlType) throws SQLException {
        if (targetSqlType == null) {
            setObject(parameterName, x);
            return;
        }
        setObject(parameterName, x, sqlTypeCode(targetSqlType));
    }

    @Override
    public void setObject(String parameterName, Object x) throws SQLException {
        setObject(getParameterIndex(parameterName), x);
    }

    @Override
    public void setCharacterStream(String parameterName, Reader reader, int length) throws SQLException {
        setCharacterStream(getParameterIndex(parameterName), reader, length);
    }

    @Override
    public void setDate(String parameterName, java.sql.Date x, Calendar cal) throws SQLException {
        setDate(getParameterIndex(parameterName), x, cal);
    }

    @Override
    public void setTime(String parameterName, Time x, Calendar cal) throws SQLException {
        setTime(getParameterIndex(parameterName), x, cal);
    }

    @Override
    public void setTimestamp(String parameterName, Timestamp x, Calendar cal) throws SQLException {
        setTimestamp(getParameterIndex(parameterName), x, cal);
    }

    @Override
    public void setURL(String parameterName, URL val) throws SQLException {
        setURL(getParameterIndex(parameterName), val);
    }

    @Override
    public RowId getRowId(int parameterIndex) throws SQLException {
        return SBRowId.fromObject(getObject(parameterIndex));
    }

    @Override
    public RowId getRowId(String parameterName) throws SQLException {
        return getRowId(getParameterIndex(parameterName));
    }

    @Override
    public void setRowId(String parameterName, RowId x) throws SQLException {
        setRowId(getParameterIndex(parameterName), x);
    }

    @Override
    public void setNString(String parameterName, String value) throws SQLException {
        setNString(getParameterIndex(parameterName), value);
    }

    @Override
    public void setNCharacterStream(String parameterName, Reader value, long length) throws SQLException {
        setNCharacterStream(getParameterIndex(parameterName), value, length);
    }

    @Override
    public void setNClob(String parameterName, NClob value) throws SQLException {
        setNClob(getParameterIndex(parameterName), value);
    }

    @Override
    public void setClob(String parameterName, Reader reader, long length) throws SQLException {
        setClob(getParameterIndex(parameterName), reader, length);
    }

    @Override
    public void setBlob(String parameterName, InputStream inputStream, long length) throws SQLException {
        setBlob(getParameterIndex(parameterName), inputStream, length);
    }

    @Override
    public void setNClob(String parameterName, Reader reader, long length) throws SQLException {
        setNClob(getParameterIndex(parameterName), reader, length);
    }

    @Override
    public NClob getNClob(int parameterIndex) throws SQLException {
        String s = getString(parameterIndex);
        return s != null ? new SBNClob(s) : null;
    }

    @Override
    public NClob getNClob(String parameterName) throws SQLException {
        return getNClob(getParameterIndex(parameterName));
    }

    @Override
    public void setSQLXML(String parameterName, SQLXML xmlObject) throws SQLException {
        setSQLXML(getParameterIndex(parameterName), xmlObject);
    }

    @Override
    public SQLXML getSQLXML(int parameterIndex) throws SQLException {
        String s = getString(parameterIndex);
        return s != null ? new SBSQLXML(s) : null;
    }

    @Override
    public SQLXML getSQLXML(String parameterName) throws SQLException {
        return getSQLXML(getParameterIndex(parameterName));
    }

    @Override
    public String getNString(int parameterIndex) throws SQLException {
        return getString(parameterIndex);
    }

    @Override
    public String getNString(String parameterName) throws SQLException {
        return getString(parameterName);
    }

    @Override
    public Reader getNCharacterStream(int parameterIndex) throws SQLException {
        String s = getString(parameterIndex);
        return s != null ? new StringReader(s) : null;
    }

    @Override
    public Reader getNCharacterStream(String parameterName) throws SQLException {
        return getNCharacterStream(getParameterIndex(parameterName));
    }

    @Override
    public Reader getCharacterStream(int parameterIndex) throws SQLException {
        String s = getString(parameterIndex);
        return s != null ? new StringReader(s) : null;
    }

    @Override
    public Reader getCharacterStream(String parameterName) throws SQLException {
        return getCharacterStream(getParameterIndex(parameterName));
    }

    @Override
    public void setBlob(String parameterName, Blob x) throws SQLException {
        setBlob(getParameterIndex(parameterName), x);
    }

    @Override
    public void setClob(String parameterName, Clob x) throws SQLException {
        setClob(getParameterIndex(parameterName), x);
    }

    @Override
    public void setAsciiStream(String parameterName, InputStream x, long length) throws SQLException {
        setAsciiStream(getParameterIndex(parameterName), x, length);
    }

    @Override
    public void setBinaryStream(String parameterName, InputStream x, long length) throws SQLException {
        setBinaryStream(getParameterIndex(parameterName), x, length);
    }

    @Override
    public void setCharacterStream(String parameterName, Reader reader, long length) throws SQLException {
        setCharacterStream(getParameterIndex(parameterName), reader, length);
    }

    @Override
    public void setAsciiStream(String parameterName, InputStream x) throws SQLException {
        setAsciiStream(getParameterIndex(parameterName), x);
    }

    @Override
    public void setBinaryStream(String parameterName, InputStream x) throws SQLException {
        setBinaryStream(getParameterIndex(parameterName), x);
    }

    @Override
    public void setCharacterStream(String parameterName, Reader reader) throws SQLException {
        setCharacterStream(getParameterIndex(parameterName), reader);
    }

    @Override
    public void setNCharacterStream(String parameterName, Reader value) throws SQLException {
        setNCharacterStream(getParameterIndex(parameterName), value);
    }

    @Override
    public void setClob(String parameterName, Reader reader) throws SQLException {
        setClob(getParameterIndex(parameterName), reader);
    }

    @Override
    public void setBlob(String parameterName, InputStream inputStream) throws SQLException {
        setBlob(getParameterIndex(parameterName), inputStream);
    }

    @Override
    public void setNClob(String parameterName, Reader reader) throws SQLException {
        setNClob(getParameterIndex(parameterName), reader);
    }

    @Override
    public <T> T getObject(int parameterIndex, Class<T> type) throws SQLException {
        Object value = getObject(parameterIndex);
        if (type == null) {
            throw new SQLException("Target type cannot be null", "HY004");
        }
        if (value == null) {
            return null;
        }
        if (type.isInstance(value)) {
            return type.cast(value);
        }

        if (type == String.class) {
            return type.cast(value.toString());
        } else if (type == Integer.class || type == int.class) {
            Integer converted = Integer.valueOf(getInt(parameterIndex));
            @SuppressWarnings("unchecked")
            T casted = (T) converted;
            return casted;
        } else if (type == Long.class || type == long.class) {
            Long converted = Long.valueOf(getLong(parameterIndex));
            @SuppressWarnings("unchecked")
            T casted = (T) converted;
            return casted;
        } else if (type == Double.class || type == double.class) {
            Double converted = Double.valueOf(getDouble(parameterIndex));
            @SuppressWarnings("unchecked")
            T casted = (T) converted;
            return casted;
        } else if (type == Float.class || type == float.class) {
            Float converted = Float.valueOf(getFloat(parameterIndex));
            @SuppressWarnings("unchecked")
            T casted = (T) converted;
            return casted;
        } else if (type == Short.class || type == short.class) {
            Short converted = Short.valueOf(getShort(parameterIndex));
            @SuppressWarnings("unchecked")
            T casted = (T) converted;
            return casted;
        } else if (type == Byte.class || type == byte.class) {
            Byte converted = Byte.valueOf(getByte(parameterIndex));
            @SuppressWarnings("unchecked")
            T casted = (T) converted;
            return casted;
        } else if (type == Boolean.class || type == boolean.class) {
            Boolean converted = Boolean.valueOf(getBoolean(parameterIndex));
            @SuppressWarnings("unchecked")
            T casted = (T) converted;
            return casted;
        } else if (type == BigDecimal.class) {
            return type.cast(getBigDecimal(parameterIndex));
        } else if (type == java.sql.Date.class) {
            return type.cast(getDate(parameterIndex));
        } else if (type == Time.class) {
            return type.cast(getTime(parameterIndex));
        } else if (type == Timestamp.class) {
            return type.cast(getTimestamp(parameterIndex));
        } else if (type == LocalDate.class) {
            java.sql.Date date = getDate(parameterIndex);
            return date == null ? null : type.cast(date.toLocalDate());
        } else if (type == LocalTime.class) {
            Time time = getTime(parameterIndex);
            return time == null ? null : type.cast(time.toLocalTime());
        } else if (type == OffsetTime.class) {
            if (value instanceof OffsetTime offsetTime) {
                return type.cast(offsetTime);
            }
            if (value instanceof OffsetDateTime offsetDateTime) {
                return type.cast(offsetDateTime.toOffsetTime());
            }
            if (value instanceof LocalTime localTime) {
                return type.cast(OffsetTime.of(localTime, ZoneOffset.UTC));
            }
            if (value instanceof Time timeValue) {
                return type.cast(OffsetTime.of(timeValue.toLocalTime(), ZoneOffset.UTC));
            }
            return type.cast(parseOffsetTimeLiteral(value.toString()));
        } else if (type == LocalDateTime.class) {
            Timestamp ts = getTimestamp(parameterIndex);
            return ts == null ? null : type.cast(ts.toLocalDateTime());
        } else if (type == ZonedDateTime.class) {
            if (value instanceof ZonedDateTime zonedDateTime) {
                return type.cast(zonedDateTime);
            }
            if (value instanceof OffsetDateTime offsetDateTime) {
                return type.cast(offsetDateTime.toZonedDateTime());
            }
            if (value instanceof Instant instant) {
                return type.cast(instant.atZone(ZoneOffset.UTC));
            }
            Timestamp ts = getTimestamp(parameterIndex);
            return ts == null ? null : type.cast(ts.toInstant().atZone(ZoneOffset.UTC));
        } else if (type == OffsetDateTime.class) {
            if (value instanceof OffsetDateTime offsetDateTime) {
                return type.cast(offsetDateTime);
            }
            if (value instanceof ZonedDateTime zonedDateTime) {
                return type.cast(zonedDateTime.toOffsetDateTime());
            }
            Timestamp ts = getTimestamp(parameterIndex);
            return ts == null ? null : type.cast(ts.toInstant().atOffset(ZoneOffset.UTC));
        } else if (type == Instant.class) {
            Timestamp ts = getTimestamp(parameterIndex);
            return ts == null ? null : type.cast(ts.toInstant());
        } else if (type == byte[].class) {
            return type.cast(getBytes(parameterIndex));
        } else if (type == UUID.class) {
            String s = getString(parameterIndex);
            return s == null ? null : type.cast(UUID.fromString(s));
        } else if (type == Array.class) {
            return type.cast(getArray(parameterIndex));
        } else if (type == Struct.class) {
            Object raw = readOutParameter(parameterIndex);
            if (raw == null) {
                return null;
            }
            if (raw instanceof Struct structValue) {
                return type.cast(structValue);
            }
            if (raw instanceof Object[] attrs) {
                return type.cast(new SBStruct("record", attrs));
            }
            if (raw instanceof Collection<?> attrs) {
                return type.cast(new SBStruct("record", attrs.toArray()));
            }
            throw new SQLException("Cannot convert to " + type.getName(), "HY000");
        } else if (SQLData.class.isAssignableFrom(type)) {
            try (SBResultSet bridge = bridgeSingleValueResultSet(value)) {
                if (!bridge.next()) {
                    return null;
                }
                return bridge.getObject(1, type);
            }
        } else if (type == Ref.class) {
            return type.cast(getRef(parameterIndex));
        } else if (type == Blob.class) {
            return type.cast(getBlob(parameterIndex));
        } else if (type == Clob.class) {
            return type.cast(getClob(parameterIndex));
        } else if (type == NClob.class) {
            return type.cast(getNClob(parameterIndex));
        } else if (type == RowId.class) {
            Object raw = readOutParameter(parameterIndex);
            return raw == null ? null : type.cast(SBRowId.fromObject(raw));
        } else if (type == SQLXML.class) {
            Object raw = readOutParameter(parameterIndex);
            return raw == null ? null : type.cast(new SBSQLXML(raw.toString()));
        } else if (type == ResultSet.class) {
            Object raw = readOutParameter(parameterIndex);
            if (raw == null) {
                return null;
            }
            if (raw instanceof ResultSet resultSet) {
                return type.cast(resultSet);
            }
            throw new SQLException("Cannot convert to " + type.getName(), "HY000");
        } else if (type == URL.class) {
            return type.cast(getURL(parameterIndex));
        }

        throw new SQLException("Cannot convert to " + type.getName(), "HY000");
    }

    private static SBResultSet bridgeSingleValueResultSet(Object value) {
        SBColumnInfo column = new SBColumnInfo();
        column.setName("out");
        return new SBResultSet(
            null,
            Collections.singletonList(column),
            Collections.singletonList(new Object[] {value})
        );
    }

    private static String inferArrayBaseType(Object[] elements) {
        if (elements == null) {
            return "text";
        }
        for (Object element : elements) {
            if (element == null) {
                continue;
            }
            if (element instanceof Boolean) return "boolean";
            if (element instanceof Short) return "smallint";
            if (element instanceof Integer) return "integer";
            if (element instanceof Long) return "bigint";
            if (element instanceof Float) return "real";
            if (element instanceof Double || element instanceof BigDecimal) return "numeric";
            if (element instanceof byte[]) return "bytea";
            if (element instanceof java.sql.Date) return "date";
            if (element instanceof java.sql.Time) return "time";
            if (element instanceof java.sql.Timestamp) return "timestamp";
            if (element instanceof OffsetTime) return "timetz";
            if (element instanceof OffsetDateTime || element instanceof ZonedDateTime || element instanceof Instant) {
                return "timestamptz";
            }
            return "text";
        }
        return "text";
    }

    private static OffsetTime parseOffsetTimeLiteral(String value) throws SQLException {
        if (value == null) {
            throw new SQLException("TIME WITH TIME ZONE literal cannot be null", "22007");
        }
        String normalized = value.trim();
        if (normalized.matches(".*[+-]\\d{2}$")) {
            normalized = normalized + ":00";
        }
        try {
            return OffsetTime.parse(normalized);
        } catch (RuntimeException ex) {
            throw new SQLException("Invalid TIME WITH TIME ZONE literal: " + value, "22007", ex);
        }
    }

    private static int sqlTypeCode(SQLType sqlType) throws SQLException {
        if (sqlType == null) {
            throw new SQLException("SQLType cannot be null", "HY004");
        }
        Integer vendorType = sqlType.getVendorTypeNumber();
        if (vendorType == null) {
            throw new SQLException("SQLType vendor type is not available: " + sqlType, "HY004");
        }
        return vendorType;
    }

    private static Object[] parseArrayLiteral(String raw) throws SQLException {
        String text = raw == null ? "" : raw.trim();
        if (text.isEmpty()) {
            return new Object[0];
        }
        if (text.regionMatches(true, 0, "ARRAY[", 0, 6) && text.endsWith("]")) {
            text = text.substring(6, text.length() - 1).trim();
        } else if ((text.startsWith("{") && text.endsWith("}")) ||
                   (text.startsWith("[") && text.endsWith("]"))) {
            text = text.substring(1, text.length() - 1).trim();
        }
        if (text.isEmpty()) {
            return new Object[0];
        }
        List<String> tokens = splitArrayTokens(text);
        List<Object> values = new ArrayList<>(tokens.size());
        for (String token : tokens) {
            values.add(parseArrayToken(token));
        }
        return values.toArray(new Object[0]);
    }

    private static List<String> splitArrayTokens(String text) {
        List<String> tokens = new ArrayList<>();
        StringBuilder current = new StringBuilder();
        int nesting = 0;
        boolean inSingle = false;
        boolean inDouble = false;
        boolean escaped = false;

        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            if (escaped) {
                current.append(c);
                escaped = false;
                continue;
            }
            if ((inSingle || inDouble) && c == '\\') {
                current.append(c);
                escaped = true;
                continue;
            }
            if (c == '\'' && !inDouble) {
                inSingle = !inSingle;
                current.append(c);
                continue;
            }
            if (c == '"' && !inSingle) {
                inDouble = !inDouble;
                current.append(c);
                continue;
            }
            if (!inSingle && !inDouble) {
                if (c == '{' || c == '[' || c == '(') {
                    nesting++;
                } else if (c == '}' || c == ']' || c == ')') {
                    if (nesting > 0) nesting--;
                } else if (c == ',' && nesting == 0) {
                    tokens.add(current.toString().trim());
                    current.setLength(0);
                    continue;
                }
            }
            current.append(c);
        }
        tokens.add(current.toString().trim());
        return tokens;
    }

    private static Object parseArrayToken(String token) throws SQLException {
        if (token == null) {
            return null;
        }
        String value = token.trim();
        if (value.isEmpty()) {
            return "";
        }
        if ("NULL".equalsIgnoreCase(value)) {
            return null;
        }
        if ((value.startsWith("{") && value.endsWith("}")) ||
            (value.startsWith("[") && value.endsWith("]")) ||
            (value.regionMatches(true, 0, "ARRAY[", 0, 6) && value.endsWith("]"))) {
            return parseArrayLiteral(value);
        }
        if ((value.length() >= 2 && value.startsWith("\"") && value.endsWith("\"")) ||
            (value.length() >= 2 && value.startsWith("'") && value.endsWith("'"))) {
            return unquoteArrayToken(value);
        }
        if ("true".equalsIgnoreCase(value) || "t".equalsIgnoreCase(value)) {
            return Boolean.TRUE;
        }
        if ("false".equalsIgnoreCase(value) || "f".equalsIgnoreCase(value)) {
            return Boolean.FALSE;
        }
        try {
            long asLong = Long.parseLong(value);
            if (asLong <= Integer.MAX_VALUE && asLong >= Integer.MIN_VALUE) {
                return (int) asLong;
            }
            return asLong;
        } catch (NumberFormatException ignored) {
            // Fall through to floating point/string parsing.
        }
        try {
            return Double.parseDouble(value);
        } catch (NumberFormatException ignored) {
            return value;
        }
    }

    private static String unquoteArrayToken(String value) {
        String inner = value.substring(1, value.length() - 1);
        inner = inner.replace("\\\\", "\\");
        inner = inner.replace("\\\"", "\"");
        inner = inner.replace("\\'", "'");
        inner = inner.replace("''", "'");
        return inner;
    }

    @Override
    public <T> T getObject(String parameterName, Class<T> type) throws SQLException {
        return getObject(getParameterIndex(parameterName), type);
    }

    private int getParameterIndex(String parameterName) throws SQLException {
        if (parameterName == null || parameterName.isBlank()) {
            throw new SQLException("Parameter name cannot be empty", "07009");
        }
        Integer mapped = namedParameters.get(parameterName);
        if (mapped != null) {
            return mapped;
        }

        Integer parsedNamed = resolveNamedParameterIndex(parameterName);
        if (parsedNamed != null) {
            int logicalIndex = functionReturnCallSyntax ? parsedNamed + 1 : parsedNamed;
            namedParameters.put(parameterName, logicalIndex);
            return logicalIndex;
        }

        String normalized = parameterName.trim().toLowerCase(Locale.ROOT);
        String numericToken = normalized;
        if (numericToken.startsWith("@")) {
            numericToken = numericToken.substring(1);
        }
        if (numericToken.startsWith(":")) {
            numericToken = numericToken.substring(1);
        }
        if (numericToken.startsWith("param")) {
            numericToken = numericToken.substring(5);
        } else if (numericToken.startsWith("p")) {
            numericToken = numericToken.substring(1);
        }

        try {
            int index = Integer.parseInt(numericToken);
            int logicalCount = logicalParameterCount();
            if (index < 1 || index > logicalCount) {
                throw new SQLException("Parameter index out of range: " + parameterName, "07009");
            }
            namedParameters.put(parameterName, index);
            return index;
        } catch (NumberFormatException ex) {
            throw new SQLException("Parameter not found: " + parameterName, "07009");
        }
    }
}
