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
import java.nio.charset.StandardCharsets;
import java.sql.*;
import java.util.*;
import java.time.*;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * JDBC ResultSet implementation for ScratchBird.
 */
public class SBResultSet implements ResultSet {
    private static final Map<TableCacheKey, String> TABLE_SQL_BY_KEY = new ConcurrentHashMap<>();
    private static final Map<TableCacheKey, Map<Integer, String>> TABLE_COLUMN_BY_KEY_AND_ATTNUM =
        new ConcurrentHashMap<>();
    private static final Set<String> TABLE_REFERENCE_NON_ALIAS_TOKENS = Set.of(
        "join",
        "left",
        "right",
        "full",
        "inner",
        "outer",
        "cross",
        "natural",
        "straight_join",
        "on",
        "using",
        "where",
        "group",
        "having",
        "order",
        "limit",
        "offset",
        "fetch",
        "for",
        "union",
        "intersect",
        "except",
        "window",
        "lateral"
    );

    private record TableCacheKey(String namespace, int tableOid) {}

    // Parent statement
    private final SBStatement statement;

    // Column metadata
    private List<SBColumnInfo> columns;
    private final Map<String, Integer> columnNameIndex;

    // Row data
    private final SBRowStream stream;
    private final List<Object[]> rows;
    private Object[] currentRowData;
    private int rowsRead = 0;
    private final int maxRowsLimit;

    // Position (0-indexed internally, before-first is -1)
    private int currentRow = -1;

    // State
    private final AtomicBoolean closed = new AtomicBoolean(false);
    private boolean wasNull = false;

    // Warnings
    private SQLWarning warnings;

    // Fetch direction and size
    private int fetchDirection = ResultSet.FETCH_FORWARD;
    private int fetchSize = 0;
    private String cursorName;
    private final int resultSetType;

    // Updatable result set state
    private boolean updatable;
    private final boolean bufferedRowsMutable;
    private UpdateTarget updateTarget;
    private final Map<Integer, Object> pendingUpdates = new LinkedHashMap<>();
    private Object[] originalRowSnapshot;
    private boolean onInsertRow = false;
    private Object[] insertRowBuffer;
    private int savedCurrentRow = -1;
    private Object[] savedCurrentRowData;
    private boolean rowUpdatedFlag = false;
    private boolean rowInsertedFlag = false;
    private boolean rowDeletedFlag = false;

    /**
     * Creates a new result set from a buffered row list.
     */
    public SBResultSet(SBStatement statement, List<SBColumnInfo> columns, List<Object[]> rows) {
        this(statement, new ListRowStream(columns, rows), 0);
    }

    /**
     * Creates a new result set backed by a streaming cursor.
     */
    public SBResultSet(SBStatement statement, SBRowStream stream, int maxRowsLimit) {
        this.statement = statement;
        this.stream = stream;
        this.maxRowsLimit = maxRowsLimit;
        this.columns = stream != null && stream.getColumns() != null ? stream.getColumns() : Collections.emptyList();
        this.rows = stream instanceof ListRowStream ? ((ListRowStream) stream).getRows() : Collections.emptyList();

        this.columnNameIndex = new HashMap<>();
        rebuildColumnIndex();
        this.bufferedRowsMutable = stream instanceof ListRowStream;
        int requestedType = statement != null
            ? statement.resultSetType
            : (this.bufferedRowsMutable ? ResultSet.TYPE_SCROLL_INSENSITIVE : ResultSet.TYPE_FORWARD_ONLY);
        if (requestedType != ResultSet.TYPE_FORWARD_ONLY && !this.bufferedRowsMutable) {
            this.resultSetType = ResultSet.TYPE_FORWARD_ONLY;
        } else {
            this.resultSetType = requestedType;
        }
        recomputeUpdatableState();
    }

    // ==================== Navigation ====================

    @Override
    public boolean next() throws SQLException {
        checkClosed();
        ensureNotOnInsertRow();
        if (maxRowsLimit > 0 && rowsRead >= maxRowsLimit) {
            currentRow = rowsRead;
            currentRowData = null;
            clearRowActionFlags();
            return false;
        }
        Object[] row = stream != null ? stream.nextRow() : null;
        if (row == null) {
            currentRow = rowsRead;
            currentRowData = null;
            clearRowActionFlags();
            return false;
        }
        currentRowData = row;
        rowsRead++;
        currentRow = rowsRead - 1;
        clearRowActionFlags();
        pendingUpdates.clear();
        originalRowSnapshot = null;
        syncColumns();
        return true;
    }

    @Override
    public void close() throws SQLException {
        if (closed.compareAndSet(false, true)) {
            SQLException closeFailure = null;
            if (stream != null) {
                try {
                    stream.close();
                } catch (SQLException e) {
                    closeFailure = e;
                }
            }
            if (statement != null) {
                statement.onResultSetClosed(this);
                statement.checkCloseOnCompletion();
            }
            if (closeFailure != null) {
                throw closeFailure;
            }
        }
    }

    @Override
    public boolean wasNull() throws SQLException {
        checkClosed();
        return wasNull;
    }

    // ==================== Getters by Column Index ====================

    @Override
    public String getString(int columnIndex) throws SQLException {
        Object value = getObject(columnIndex);
        if (value == null) return null;
        return value.toString();
    }

    @Override
    public boolean getBoolean(int columnIndex) throws SQLException {
        Object value = getObject(columnIndex);
        if (value == null) return false;
        if (value instanceof Boolean) return (Boolean) value;
        if (value instanceof Number) return ((Number) value).intValue() != 0;
        String s = value.toString().toLowerCase();
        return "t".equals(s) || "true".equals(s) || "yes".equals(s) || "1".equals(s);
    }

    @Override
    public byte getByte(int columnIndex) throws SQLException {
        Object value = getObject(columnIndex);
        if (value == null) return 0;
        if (value instanceof Number) return ((Number) value).byteValue();
        return Byte.parseByte(value.toString());
    }

    @Override
    public short getShort(int columnIndex) throws SQLException {
        Object value = getObject(columnIndex);
        if (value == null) return 0;
        if (value instanceof Number) return ((Number) value).shortValue();
        return Short.parseShort(value.toString());
    }

    @Override
    public int getInt(int columnIndex) throws SQLException {
        Object value = getObject(columnIndex);
        if (value == null) return 0;
        if (value instanceof Number) return ((Number) value).intValue();
        return Integer.parseInt(value.toString());
    }

    @Override
    public long getLong(int columnIndex) throws SQLException {
        Object value = getObject(columnIndex);
        if (value == null) return 0;
        if (value instanceof Number) return ((Number) value).longValue();
        return Long.parseLong(value.toString());
    }

    @Override
    public float getFloat(int columnIndex) throws SQLException {
        Object value = getObject(columnIndex);
        if (value == null) return 0;
        if (value instanceof Number) return ((Number) value).floatValue();
        return Float.parseFloat(value.toString());
    }

    @Override
    public double getDouble(int columnIndex) throws SQLException {
        Object value = getObject(columnIndex);
        if (value == null) return 0;
        if (value instanceof Number) return ((Number) value).doubleValue();
        return Double.parseDouble(value.toString());
    }

    @Override
    @Deprecated
    public BigDecimal getBigDecimal(int columnIndex, int scale) throws SQLException {
        BigDecimal bd = getBigDecimal(columnIndex);
        if (bd == null) return null;
        return bd.setScale(scale, RoundingMode.HALF_UP);
    }

    @Override
    public BigDecimal getBigDecimal(int columnIndex) throws SQLException {
        Object value = getObject(columnIndex);
        if (value == null) return null;
        if (value instanceof BigDecimal) return (BigDecimal) value;
        if (value instanceof Number) return BigDecimal.valueOf(((Number) value).doubleValue());
        return new BigDecimal(value.toString());
    }

    @Override
    public byte[] getBytes(int columnIndex) throws SQLException {
        Object value = getObject(columnIndex);
        if (value == null) return null;
        if (value instanceof byte[]) return (byte[]) value;
        // Try to decode hex string
        String s = value.toString();
        if (s.startsWith("\\x")) {
            s = s.substring(2);
            byte[] bytes = new byte[s.length() / 2];
            for (int i = 0; i < bytes.length; i++) {
                bytes[i] = (byte) Integer.parseInt(s.substring(i * 2, i * 2 + 2), 16);
            }
            return bytes;
        }
        return s.getBytes();
    }

    @Override
    public java.sql.Date getDate(int columnIndex) throws SQLException {
        Object value = getObject(columnIndex);
        if (value == null) return null;
        if (value instanceof java.sql.Date) return (java.sql.Date) value;
        if (value instanceof java.util.Date) return new java.sql.Date(((java.util.Date) value).getTime());
        return java.sql.Date.valueOf(value.toString());
    }

    @Override
    public java.sql.Date getDate(int columnIndex, Calendar cal) throws SQLException {
        return adjustDate(getDate(columnIndex), cal);
    }

    @Override
    public Time getTime(int columnIndex) throws SQLException {
        Object value = getObject(columnIndex);
        if (value == null) return null;
        if (value instanceof Time) return (Time) value;
        if (value instanceof OffsetTime offsetTime) return Time.valueOf(offsetTime.toLocalTime());
        if (value instanceof LocalTime localTime) return Time.valueOf(localTime);
        if (value instanceof java.util.Date) return new Time(((java.util.Date) value).getTime());
        return Time.valueOf(value.toString());
    }

    @Override
    public Time getTime(int columnIndex, Calendar cal) throws SQLException {
        return adjustTime(getTime(columnIndex), cal);
    }

    @Override
    public Timestamp getTimestamp(int columnIndex) throws SQLException {
        Object value = getObject(columnIndex);
        if (value == null) return null;
        if (value instanceof Timestamp) return (Timestamp) value;
        if (value instanceof Instant instant) return Timestamp.from(instant);
        if (value instanceof OffsetDateTime offsetDateTime) return Timestamp.from(offsetDateTime.toInstant());
        if (value instanceof ZonedDateTime zonedDateTime) return Timestamp.from(zonedDateTime.toInstant());
        if (value instanceof LocalDateTime localDateTime) return Timestamp.valueOf(localDateTime);
        if (value instanceof java.util.Date) return new Timestamp(((java.util.Date) value).getTime());
        return Timestamp.valueOf(value.toString());
    }

    @Override
    public Timestamp getTimestamp(int columnIndex, Calendar cal) throws SQLException {
        return adjustTimestamp(getTimestamp(columnIndex), cal);
    }

    private static java.sql.Date adjustDate(java.sql.Date value, Calendar cal) {
        if (value == null || cal == null) {
            return value;
        }
        Instant instant = Instant.ofEpochMilli(value.getTime());
        LocalDate localDate = instant.atZone(cal.getTimeZone().toZoneId()).toLocalDate();
        return java.sql.Date.valueOf(localDate);
    }

    private static java.sql.Time adjustTime(java.sql.Time value, Calendar cal) {
        if (value == null || cal == null) {
            return value;
        }
        Instant instant = Instant.ofEpochMilli(value.getTime());
        LocalTime localTime = instant.atZone(cal.getTimeZone().toZoneId()).toLocalTime();
        return java.sql.Time.valueOf(localTime);
    }

    private static java.sql.Timestamp adjustTimestamp(java.sql.Timestamp value, Calendar cal) {
        if (value == null || cal == null) {
            return value;
        }
        Instant instant = value.toInstant();
        LocalDateTime localDateTime = instant.atZone(cal.getTimeZone().toZoneId()).toLocalDateTime();
        java.sql.Timestamp adjusted = java.sql.Timestamp.valueOf(localDateTime);
        adjusted.setNanos(value.getNanos());
        return adjusted;
    }

    @Override
    public InputStream getAsciiStream(int columnIndex) throws SQLException {
        String s = getString(columnIndex);
        if (s == null) return null;
        try {
            return new ByteArrayInputStream(s.getBytes("US-ASCII"));
        } catch (UnsupportedEncodingException e) {
            throw new SQLException("ASCII encoding not supported", "HY000", e);
        }
    }

    @Override
    @Deprecated
    public InputStream getUnicodeStream(int columnIndex) throws SQLException {
        String s = getString(columnIndex);
        if (s == null) return null;
        try {
            return new ByteArrayInputStream(s.getBytes("UTF-8"));
        } catch (UnsupportedEncodingException e) {
            throw new SQLException("UTF-8 encoding not supported", "HY000", e);
        }
    }

    @Override
    public InputStream getBinaryStream(int columnIndex) throws SQLException {
        byte[] bytes = getBytes(columnIndex);
        if (bytes == null) return null;
        return new ByteArrayInputStream(bytes);
    }

    @Override
    public Reader getCharacterStream(int columnIndex) throws SQLException {
        String s = getString(columnIndex);
        if (s == null) return null;
        return new StringReader(s);
    }

    // ==================== Getters by Column Label ====================

    @Override
    public String getString(String columnLabel) throws SQLException {
        return getString(findColumn(columnLabel));
    }

    @Override
    public boolean getBoolean(String columnLabel) throws SQLException {
        return getBoolean(findColumn(columnLabel));
    }

    @Override
    public byte getByte(String columnLabel) throws SQLException {
        return getByte(findColumn(columnLabel));
    }

    @Override
    public short getShort(String columnLabel) throws SQLException {
        return getShort(findColumn(columnLabel));
    }

    @Override
    public int getInt(String columnLabel) throws SQLException {
        return getInt(findColumn(columnLabel));
    }

    @Override
    public long getLong(String columnLabel) throws SQLException {
        return getLong(findColumn(columnLabel));
    }

    @Override
    public float getFloat(String columnLabel) throws SQLException {
        return getFloat(findColumn(columnLabel));
    }

    @Override
    public double getDouble(String columnLabel) throws SQLException {
        return getDouble(findColumn(columnLabel));
    }

    @Override
    @Deprecated
    public BigDecimal getBigDecimal(String columnLabel, int scale) throws SQLException {
        return getBigDecimal(findColumn(columnLabel), scale);
    }

    @Override
    public BigDecimal getBigDecimal(String columnLabel) throws SQLException {
        return getBigDecimal(findColumn(columnLabel));
    }

    @Override
    public byte[] getBytes(String columnLabel) throws SQLException {
        return getBytes(findColumn(columnLabel));
    }

    @Override
    public java.sql.Date getDate(String columnLabel) throws SQLException {
        return getDate(findColumn(columnLabel));
    }

    @Override
    public java.sql.Date getDate(String columnLabel, Calendar cal) throws SQLException {
        return getDate(findColumn(columnLabel), cal);
    }

    @Override
    public Time getTime(String columnLabel) throws SQLException {
        return getTime(findColumn(columnLabel));
    }

    @Override
    public Time getTime(String columnLabel, Calendar cal) throws SQLException {
        return getTime(findColumn(columnLabel), cal);
    }

    @Override
    public Timestamp getTimestamp(String columnLabel) throws SQLException {
        return getTimestamp(findColumn(columnLabel));
    }

    @Override
    public Timestamp getTimestamp(String columnLabel, Calendar cal) throws SQLException {
        return getTimestamp(findColumn(columnLabel), cal);
    }

    @Override
    public InputStream getAsciiStream(String columnLabel) throws SQLException {
        return getAsciiStream(findColumn(columnLabel));
    }

    @Override
    @Deprecated
    public InputStream getUnicodeStream(String columnLabel) throws SQLException {
        return getUnicodeStream(findColumn(columnLabel));
    }

    @Override
    public InputStream getBinaryStream(String columnLabel) throws SQLException {
        return getBinaryStream(findColumn(columnLabel));
    }

    @Override
    public Reader getCharacterStream(String columnLabel) throws SQLException {
        return getCharacterStream(findColumn(columnLabel));
    }

    // ==================== Object Getters ====================

    @Override
    public Object getObject(int columnIndex) throws SQLException {
        checkClosed();
        checkColumnIndex(columnIndex);
        Object value;
        if (onInsertRow) {
            if (insertRowBuffer == null) {
                throw new SQLException("Cursor not on a valid row", "HY109");
            }
            value = insertRowBuffer[columnIndex - 1];
        } else {
            checkRow();
            value = currentRowData[columnIndex - 1];
        }
        wasNull = (value == null);
        return value;
    }

    @Override
    public Object getObject(String columnLabel) throws SQLException {
        return getObject(findColumn(columnLabel));
    }

    @Override
    public Object getObject(int columnIndex, Map<String, Class<?>> map) throws SQLException {
        Object value = getObject(columnIndex);
        if (value == null || map == null || map.isEmpty()) {
            return value;
        }

        Struct structValue;
        if (value instanceof Struct || value instanceof Object[] || value instanceof Collection<?>) {
            structValue = toStructValue(value);
        } else {
            return value;
        }

        Class<?> mappedClass = findMappedStructClass(structValue.getSQLTypeName(), map);
        if (mappedClass == null) {
            return structValue;
        }
        if (Struct.class.isAssignableFrom(mappedClass)) {
            return structValue;
        }
        if (!SQLData.class.isAssignableFrom(mappedClass)) {
            return structValue;
        }

        try {
            SQLData sqlData = (SQLData) mappedClass.getDeclaredConstructor().newInstance();
            sqlData.readSQL(new StructSqlInput(structValue.getAttributes()), structValue.getSQLTypeName());
            return sqlData;
        } catch (ReflectiveOperationException ex) {
            throw new SQLException("Failed to instantiate mapped SQLData class: " + mappedClass.getName(),
                "HY000", ex);
        }
    }

    @Override
    public Object getObject(String columnLabel, Map<String, Class<?>> map) throws SQLException {
        return getObject(findColumn(columnLabel), map);
    }

    @Override
    public <T> T getObject(int columnIndex, Class<T> type) throws SQLException {
        Object value = getObject(columnIndex);
        if (type == null) {
            throw new SQLException("Target type cannot be null", "HY004");
        }
        if (value == null) {
            return null;
        }

        if (type.isInstance(value)) {
            return type.cast(value);
        }

        // Type conversions
        if (type == String.class) {
            return type.cast(value.toString());
        } else if (type == Integer.class || type == int.class) {
            Integer converted = Integer.valueOf(getInt(columnIndex));
            @SuppressWarnings("unchecked")
            T casted = (T) converted;
            return casted;
        } else if (type == Long.class || type == long.class) {
            Long converted = Long.valueOf(getLong(columnIndex));
            @SuppressWarnings("unchecked")
            T casted = (T) converted;
            return casted;
        } else if (type == Double.class || type == double.class) {
            Double converted = Double.valueOf(getDouble(columnIndex));
            @SuppressWarnings("unchecked")
            T casted = (T) converted;
            return casted;
        } else if (type == Float.class || type == float.class) {
            Float converted = Float.valueOf(getFloat(columnIndex));
            @SuppressWarnings("unchecked")
            T casted = (T) converted;
            return casted;
        } else if (type == Short.class || type == short.class) {
            Short converted = Short.valueOf(getShort(columnIndex));
            @SuppressWarnings("unchecked")
            T casted = (T) converted;
            return casted;
        } else if (type == Byte.class || type == byte.class) {
            Byte converted = Byte.valueOf(getByte(columnIndex));
            @SuppressWarnings("unchecked")
            T casted = (T) converted;
            return casted;
        } else if (type == Boolean.class || type == boolean.class) {
            Boolean converted = Boolean.valueOf(getBoolean(columnIndex));
            @SuppressWarnings("unchecked")
            T casted = (T) converted;
            return casted;
        } else if (type == BigDecimal.class) {
            return type.cast(getBigDecimal(columnIndex));
        } else if (type == java.sql.Date.class) {
            return type.cast(getDate(columnIndex));
        } else if (type == Time.class) {
            return type.cast(getTime(columnIndex));
        } else if (type == Timestamp.class) {
            return type.cast(getTimestamp(columnIndex));
        } else if (type == LocalDate.class) {
            java.sql.Date date = getDate(columnIndex);
            return date == null ? null : type.cast(date.toLocalDate());
        } else if (type == LocalTime.class) {
            Time time = getTime(columnIndex);
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
            Timestamp ts = getTimestamp(columnIndex);
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
            Timestamp ts = getTimestamp(columnIndex);
            return ts == null ? null : type.cast(ts.toInstant().atZone(ZoneOffset.UTC));
        } else if (type == OffsetDateTime.class) {
            if (value instanceof OffsetDateTime offsetDateTime) {
                return type.cast(offsetDateTime);
            }
            if (value instanceof ZonedDateTime zonedDateTime) {
                return type.cast(zonedDateTime.toOffsetDateTime());
            }
            Timestamp ts = getTimestamp(columnIndex);
            return ts == null ? null : type.cast(ts.toInstant().atOffset(ZoneOffset.UTC));
        } else if (type == Instant.class) {
            Timestamp ts = getTimestamp(columnIndex);
            return ts == null ? null : type.cast(ts.toInstant());
        } else if (type == byte[].class) {
            return type.cast(getBytes(columnIndex));
        } else if (type == UUID.class) {
            String s = getString(columnIndex);
            return s == null ? null : type.cast(UUID.fromString(s));
        } else if (type == Array.class) {
            return type.cast(getArray(columnIndex));
        } else if (type == Struct.class) {
            return type.cast(toStructValue(value));
        } else if (SQLData.class.isAssignableFrom(type)) {
            Struct structValue = toStructValue(value);
            try {
                @SuppressWarnings("unchecked")
                T instance = type.getDeclaredConstructor().newInstance();
                SQLData sqlData = (SQLData) instance;
                sqlData.readSQL(new StructSqlInput(structValue.getAttributes()), structValue.getSQLTypeName());
                return instance;
            } catch (ReflectiveOperationException ex) {
                throw new SQLException("Failed to instantiate SQLData class: " + type.getName(),
                    "HY000", ex);
            }
        } else if (type == Blob.class) {
            return type.cast(getBlob(columnIndex));
        } else if (type == Clob.class) {
            return type.cast(getClob(columnIndex));
        } else if (type == NClob.class) {
            return type.cast(getNClob(columnIndex));
        } else if (type == Ref.class) {
            return type.cast(getRef(columnIndex));
        } else if (type == RowId.class) {
            return type.cast(getRowId(columnIndex));
        } else if (type == SQLXML.class) {
            return type.cast(getSQLXML(columnIndex));
        } else if (type == URL.class) {
            return type.cast(getURL(columnIndex));
        }

        throw new SQLException("Cannot convert to " + type.getName(), "HY000");
    }

    private Struct toStructValue(Object value) throws SQLException {
        if (value == null) {
            return null;
        }
        if (value instanceof Struct structValue) {
            return structValue;
        }
        if (value instanceof Object[] attrs) {
            return new SBStruct("record", attrs);
        }
        if (value instanceof Collection<?> attrs) {
            return new SBStruct("record", attrs.toArray());
        }
        throw new SQLException("Not a structured type", "HY000");
    }

    private static Class<?> findMappedStructClass(String sqlTypeName, Map<String, Class<?>> map) {
        if (map == null || map.isEmpty()) {
            return null;
        }
        String normalized = sqlTypeName == null ? "" : sqlTypeName.trim();
        if (!normalized.isEmpty()) {
            Class<?> direct = map.get(normalized);
            if (direct != null) {
                return direct;
            }
        }

        String unqualified = unqualifiedTypeName(normalized);
        if (!unqualified.isEmpty()) {
            Class<?> directUnqualified = map.get(unqualified);
            if (directUnqualified != null) {
                return directUnqualified;
            }
        }

        for (Map.Entry<String, Class<?>> entry : map.entrySet()) {
            String key = entry.getKey();
            if (key == null || key.isBlank()) {
                continue;
            }
            if (!normalized.isEmpty() && key.equalsIgnoreCase(normalized)) {
                return entry.getValue();
            }
            if (!unqualified.isEmpty() && key.equalsIgnoreCase(unqualified)) {
                return entry.getValue();
            }
        }
        return null;
    }

    private static String unqualifiedTypeName(String sqlTypeName) {
        if (sqlTypeName == null || sqlTypeName.isBlank()) {
            return "";
        }
        int dotIndex = sqlTypeName.lastIndexOf('.');
        return dotIndex >= 0 ? sqlTypeName.substring(dotIndex + 1) : sqlTypeName;
    }

    private static final class StructSqlInput implements SQLInput {
        private final Object[] values;
        private int index;
        private boolean wasNull;

        private StructSqlInput(Object[] values) {
            this.values = values == null ? new Object[0] : values.clone();
            this.index = 0;
            this.wasNull = false;
        }

        @Override
        public String readString() throws SQLException {
            Object value = nextValue();
            return value == null ? null : value.toString();
        }

        @Override
        public boolean readBoolean() throws SQLException {
            Object value = nextValue();
            if (value == null) {
                return false;
            }
            if (value instanceof Boolean booleanValue) {
                return booleanValue;
            }
            if (value instanceof Number numberValue) {
                return numberValue.intValue() != 0;
            }
            String text = value.toString().trim();
            return "1".equals(text)
                || "t".equalsIgnoreCase(text)
                || "true".equalsIgnoreCase(text)
                || "yes".equalsIgnoreCase(text)
                || "on".equalsIgnoreCase(text);
        }

        @Override
        public byte readByte() throws SQLException {
            Object value = nextValue();
            if (value == null) {
                return 0;
            }
            if (value instanceof Number numberValue) {
                return numberValue.byteValue();
            }
            return Byte.parseByte(value.toString());
        }

        @Override
        public short readShort() throws SQLException {
            Object value = nextValue();
            if (value == null) {
                return 0;
            }
            if (value instanceof Number numberValue) {
                return numberValue.shortValue();
            }
            return Short.parseShort(value.toString());
        }

        @Override
        public int readInt() throws SQLException {
            Object value = nextValue();
            if (value == null) {
                return 0;
            }
            if (value instanceof Number numberValue) {
                return numberValue.intValue();
            }
            return Integer.parseInt(value.toString());
        }

        @Override
        public long readLong() throws SQLException {
            Object value = nextValue();
            if (value == null) {
                return 0L;
            }
            if (value instanceof Number numberValue) {
                return numberValue.longValue();
            }
            return Long.parseLong(value.toString());
        }

        @Override
        public float readFloat() throws SQLException {
            Object value = nextValue();
            if (value == null) {
                return 0f;
            }
            if (value instanceof Number numberValue) {
                return numberValue.floatValue();
            }
            return Float.parseFloat(value.toString());
        }

        @Override
        public double readDouble() throws SQLException {
            Object value = nextValue();
            if (value == null) {
                return 0d;
            }
            if (value instanceof Number numberValue) {
                return numberValue.doubleValue();
            }
            return Double.parseDouble(value.toString());
        }

        @Override
        public BigDecimal readBigDecimal() throws SQLException {
            Object value = nextValue();
            if (value == null) {
                return null;
            }
            if (value instanceof BigDecimal decimalValue) {
                return decimalValue;
            }
            if (value instanceof Number numberValue) {
                return BigDecimal.valueOf(numberValue.doubleValue());
            }
            return new BigDecimal(value.toString());
        }

        @Override
        public byte[] readBytes() throws SQLException {
            Object value = nextValue();
            if (value == null) {
                return null;
            }
            if (value instanceof byte[] binaryValue) {
                return binaryValue.clone();
            }
            return value.toString().getBytes(StandardCharsets.UTF_8);
        }

        @Override
        public java.sql.Date readDate() throws SQLException {
            Object value = nextValue();
            if (value == null) {
                return null;
            }
            if (value instanceof java.sql.Date dateValue) {
                return dateValue;
            }
            if (value instanceof java.util.Date dateValue) {
                return new java.sql.Date(dateValue.getTime());
            }
            if (value instanceof LocalDate localDate) {
                return java.sql.Date.valueOf(localDate);
            }
            return java.sql.Date.valueOf(value.toString());
        }

        @Override
        public Time readTime() throws SQLException {
            Object value = nextValue();
            if (value == null) {
                return null;
            }
            if (value instanceof Time timeValue) {
                return timeValue;
            }
            if (value instanceof LocalTime localTime) {
                return Time.valueOf(localTime);
            }
            return Time.valueOf(value.toString());
        }

        @Override
        public Timestamp readTimestamp() throws SQLException {
            Object value = nextValue();
            if (value == null) {
                return null;
            }
            if (value instanceof Timestamp timestampValue) {
                return timestampValue;
            }
            if (value instanceof Instant instant) {
                return Timestamp.from(instant);
            }
            if (value instanceof LocalDateTime localDateTime) {
                return Timestamp.valueOf(localDateTime);
            }
            if (value instanceof OffsetDateTime offsetDateTime) {
                return Timestamp.from(offsetDateTime.toInstant());
            }
            if (value instanceof java.util.Date dateValue) {
                return new Timestamp(dateValue.getTime());
            }
            return Timestamp.valueOf(value.toString());
        }

        @Override
        public Reader readCharacterStream() throws SQLException {
            String value = readString();
            return value == null ? null : new StringReader(value);
        }

        @Override
        public InputStream readAsciiStream() throws SQLException {
            String value = readString();
            return value == null ? null : new ByteArrayInputStream(value.getBytes(StandardCharsets.US_ASCII));
        }

        @Override
        public InputStream readBinaryStream() throws SQLException {
            byte[] value = readBytes();
            return value == null ? null : new ByteArrayInputStream(value);
        }

        @Override
        public Object readObject() throws SQLException {
            return nextValue();
        }

        @Override
        public Ref readRef() throws SQLException {
            return SBRef.fromObject(nextValue());
        }

        @Override
        public Blob readBlob() throws SQLException {
            byte[] value = readBytes();
            return value == null ? null : new SBBlob(value);
        }

        @Override
        public Clob readClob() throws SQLException {
            String value = readString();
            return value == null ? null : new SBClob(value);
        }

        @Override
        public Array readArray() throws SQLException {
            Object value = nextValue();
            if (value == null) {
                return null;
            }
            if (value instanceof Array arrayValue) {
                return arrayValue;
            }
            if (value instanceof Object[] arrayValues) {
                return new SBArray(inferArrayBaseType(arrayValues), arrayValues);
            }
            if (value instanceof Collection<?> collectionValues) {
                Object[] arrayValues = collectionValues.toArray();
                return new SBArray(inferArrayBaseType(arrayValues), arrayValues);
            }
            if (value instanceof String textValue) {
                Object[] arrayValues = parseArrayLiteral(textValue);
                return new SBArray(inferArrayBaseType(arrayValues), arrayValues);
            }
            throw new SQLException("Not an array type", "HY000");
        }

        @Override
        public boolean wasNull() throws SQLException {
            return wasNull;
        }

        @Override
        public URL readURL() throws SQLException {
            String value = readString();
            if (value == null) {
                return null;
            }
            try {
                return new URL(value);
            } catch (MalformedURLException ex) {
                throw new SQLException("Invalid URL value in SQLData mapping", "HY000", ex);
            }
        }

        @Override
        public NClob readNClob() throws SQLException {
            String value = readString();
            return value == null ? null : new SBNClob(value);
        }

        @Override
        public String readNString() throws SQLException {
            return readString();
        }

        @Override
        public SQLXML readSQLXML() throws SQLException {
            String value = readString();
            return value == null ? null : new SBSQLXML(value);
        }

        @Override
        public RowId readRowId() throws SQLException {
            return SBRowId.fromObject(nextValue());
        }

        private Object nextValue() throws SQLException {
            if (index >= values.length) {
                wasNull = true;
                return null;
            }
            Object value = values[index++];
            wasNull = (value == null);
            return value;
        }
    }

    @Override
    public <T> T getObject(String columnLabel, Class<T> type) throws SQLException {
        return getObject(findColumn(columnLabel), type);
    }

    // ==================== Positioning ====================

    @Override
    public SQLWarning getWarnings() throws SQLException {
        checkClosed();
        return warnings;
    }

    @Override
    public void clearWarnings() throws SQLException {
        checkClosed();
        warnings = null;
    }

    @Override
    public String getCursorName() throws SQLException {
        checkClosed();
        return cursorName;
    }

    @Override
    public ResultSetMetaData getMetaData() throws SQLException {
        checkClosed();
        syncColumns();
        String schema = "";
        String table = "";
        String catalog = "";
        Map<Integer, String> schemaByColumn = new HashMap<>();
        Map<Integer, String> tableByColumn = new HashMap<>();
        Map<Integer, String> catalogByColumn = new HashMap<>();
        if (updateTarget != null) {
            String[] parsed = parseQualifiedTableSql(updateTarget.tableSql);
            if (parsed[0] != null) {
                schema = parsed[0];
            }
            if (parsed[1] != null) {
                table = parsed[1];
            }
        }
        if (statement != null && statement.connection != null) {
            try {
                String connCatalog = statement.connection.getCatalog();
                if (connCatalog != null) {
                    catalog = connCatalog;
                }
            } catch (SQLException ignored) {
                // Keep catalog blank when unavailable.
            }
        }
        if (columns != null && !columns.isEmpty()) {
            for (int i = 0; i < columns.size(); i++) {
                String tableSql = null;
                if (updateTarget != null) {
                    tableSql = updateTarget.mappedTableSql(i + 1);
                }
                if ((tableSql == null || tableSql.isBlank())
                    && statement != null
                    && statement.connection != null) {
                    SBColumnInfo column = columns.get(i);
                    if (column != null && column.getTableOid() > 0) {
                        tableSql = resolveTableSql(statement, column.getTableOid());
                    }
                }
                if (tableSql != null && !tableSql.isBlank()) {
                    String[] parsed = parseQualifiedTableSql(tableSql);
                    if (parsed[0] != null && !parsed[0].isBlank()) {
                        schemaByColumn.put(i + 1, parsed[0]);
                    }
                    if (parsed[1] != null && !parsed[1].isBlank()) {
                        tableByColumn.put(i + 1, parsed[1]);
                    }
                }
                if (catalog != null && !catalog.isBlank()) {
                    catalogByColumn.put(i + 1, catalog);
                }
            }
        }
        return new SBResultSetMetaData(
            columns,
            updatable,
            writableMetadataColumns(),
            autoIncrementMetadataColumns(),
            schemaByColumn,
            tableByColumn,
            catalogByColumn,
            schema,
            table,
            catalog
        );
    }

    @Override
    public int findColumn(String columnLabel) throws SQLException {
        checkClosed();
        syncColumns();
        Integer index = columnNameIndex.get(columnLabel.toLowerCase());
        if (index == null) {
            throw new SQLException("Column not found: " + columnLabel, "42703");
        }
        return index;
    }

    @Override
    public boolean isBeforeFirst() throws SQLException {
        checkClosed();
        if (!rows.isEmpty()) {
            return currentRow < 0;
        }
        return currentRow < 0 && currentRowData == null && rowsRead == 0;
    }

    @Override
    public boolean isAfterLast() throws SQLException {
        checkClosed();
        if (!rows.isEmpty()) {
            return currentRow >= rows.size();
        }
        return stream != null && stream.isDone() && currentRowData == null;
    }

    @Override
    public boolean isFirst() throws SQLException {
        checkClosed();
        if (!rows.isEmpty()) {
            return currentRow == 0;
        }
        return rowsRead == 1 && currentRowData != null;
    }

    @Override
    public boolean isLast() throws SQLException {
        checkClosed();
        if (!rows.isEmpty()) {
            return currentRow == rows.size() - 1;
        }
        return stream != null && stream.isDone() && currentRowData != null;
    }

    @Override
    public void beforeFirst() throws SQLException {
        checkClosed();
        ensureNotOnInsertRow();
        if (!isScrollableCursor()) {
            throw new SQLException("ResultSet is forward-only", "0A000");
        }
        currentRow = -1;
        currentRowData = null;
        rowsRead = 0;
        clearRowActionFlags();
        pendingUpdates.clear();
        originalRowSnapshot = null;
        if (stream instanceof ListRowStream) {
            ((ListRowStream) stream).setIndex(0);
        }
    }

    @Override
    public void afterLast() throws SQLException {
        checkClosed();
        ensureNotOnInsertRow();
        if (!isScrollableCursor()) {
            throw new SQLException("ResultSet is forward-only", "0A000");
        }
        currentRow = rows.size();
        currentRowData = null;
        rowsRead = rows.size();
        clearRowActionFlags();
        pendingUpdates.clear();
        originalRowSnapshot = null;
        if (stream instanceof ListRowStream) {
            ((ListRowStream) stream).setIndex(rows.size());
        }
    }

    @Override
    public boolean first() throws SQLException {
        checkClosed();
        ensureNotOnInsertRow();
        if (!isScrollableCursor()) {
            throw new SQLException("ResultSet is forward-only", "0A000");
        }
        if (rows.isEmpty()) return false;
        currentRow = 0;
        currentRowData = rows.get(0);
        rowsRead = 1;
        clearRowActionFlags();
        pendingUpdates.clear();
        originalRowSnapshot = null;
        if (stream instanceof ListRowStream) {
            ((ListRowStream) stream).setIndex(1);
        }
        return true;
    }

    @Override
    public boolean last() throws SQLException {
        checkClosed();
        ensureNotOnInsertRow();
        if (!isScrollableCursor()) {
            throw new SQLException("ResultSet is forward-only", "0A000");
        }
        if (rows.isEmpty()) return false;
        currentRow = rows.size() - 1;
        currentRowData = rows.get(currentRow);
        rowsRead = rows.size();
        clearRowActionFlags();
        pendingUpdates.clear();
        originalRowSnapshot = null;
        if (stream instanceof ListRowStream) {
            ((ListRowStream) stream).setIndex(rows.size());
        }
        return true;
    }

    @Override
    public int getRow() throws SQLException {
        checkClosed();
        if (onInsertRow) {
            return 0;
        }
        if (currentRowData == null) return 0;
        return currentRow + 1;  // 1-indexed for JDBC
    }

    @Override
    public boolean absolute(int row) throws SQLException {
        checkClosed();
        ensureNotOnInsertRow();
        if (!isScrollableCursor()) {
            throw new SQLException("ResultSet is forward-only", "0A000");
        }
        if (rows.isEmpty()) return false;

        if (row > 0) {
            currentRow = Math.min(row - 1, rows.size());
        } else if (row < 0) {
            currentRow = Math.max(rows.size() + row, -1);
        } else {
            currentRow = -1;
        }
        if (currentRow >= 0 && currentRow < rows.size()) {
            currentRowData = rows.get(currentRow);
            rowsRead = currentRow + 1;
            clearRowActionFlags();
            pendingUpdates.clear();
            originalRowSnapshot = null;
            if (stream instanceof ListRowStream) {
                ((ListRowStream) stream).setIndex(currentRow + 1);
            }
            return true;
        }
        currentRowData = null;
        rowsRead = currentRow < 0 ? 0 : rows.size();
        clearRowActionFlags();
        pendingUpdates.clear();
        originalRowSnapshot = null;
        if (stream instanceof ListRowStream) {
            ((ListRowStream) stream).setIndex(currentRow < 0 ? 0 : rows.size());
        }
        return false;
    }

    @Override
    public boolean relative(int rows) throws SQLException {
        checkClosed();
        return absolute(currentRow + 1 + rows);
    }

    @Override
    public boolean previous() throws SQLException {
        checkClosed();
        ensureNotOnInsertRow();
        if (!isScrollableCursor()) {
            throw new SQLException("ResultSet is forward-only", "0A000");
        }
        if (rows.isEmpty()) {
            currentRow = -1;
            currentRowData = null;
            rowsRead = 0;
            clearRowActionFlags();
            pendingUpdates.clear();
            originalRowSnapshot = null;
            return false;
        }
        if (currentRow > 0) {
            currentRow--;
            currentRowData = rows.get(currentRow);
            rowsRead = currentRow + 1;
            clearRowActionFlags();
            pendingUpdates.clear();
            originalRowSnapshot = null;
            if (stream instanceof ListRowStream) {
                ((ListRowStream) stream).setIndex(currentRow + 1);
            }
            return true;
        }
        currentRow = -1;
        currentRowData = null;
        rowsRead = 0;
        clearRowActionFlags();
        pendingUpdates.clear();
        originalRowSnapshot = null;
        if (stream instanceof ListRowStream) {
            ((ListRowStream) stream).setIndex(0);
        }
        return false;
    }

    @Override
    public void setFetchDirection(int direction) throws SQLException {
        checkClosed();
        this.fetchDirection = direction;
    }

    @Override
    public int getFetchDirection() throws SQLException {
        checkClosed();
        return fetchDirection;
    }

    @Override
    public void setFetchSize(int rows) throws SQLException {
        checkClosed();
        this.fetchSize = rows;
    }

    @Override
    public int getFetchSize() throws SQLException {
        checkClosed();
        return fetchSize;
    }

    @Override
    public int getType() throws SQLException {
        checkClosed();
        return resultSetType;
    }

    @Override
    public int getConcurrency() throws SQLException {
        checkClosed();
        return updatable ? ResultSet.CONCUR_UPDATABLE : ResultSet.CONCUR_READ_ONLY;
    }

    // ==================== Update Methods (Read-Only) ====================

    @Override
    public boolean rowUpdated() throws SQLException {
        checkClosed();
        return rowUpdatedFlag;
    }

    @Override
    public boolean rowInserted() throws SQLException {
        checkClosed();
        return rowInsertedFlag;
    }

    @Override
    public boolean rowDeleted() throws SQLException {
        checkClosed();
        return rowDeletedFlag;
    }

    @Override
    public void updateNull(int columnIndex) throws SQLException {
        updateColumn(columnIndex, null);
    }

    @Override
    public void updateBoolean(int columnIndex, boolean x) throws SQLException {
        updateColumn(columnIndex, x);
    }

    @Override
    public void updateByte(int columnIndex, byte x) throws SQLException {
        updateColumn(columnIndex, x);
    }

    @Override
    public void updateShort(int columnIndex, short x) throws SQLException {
        updateColumn(columnIndex, x);
    }

    @Override
    public void updateInt(int columnIndex, int x) throws SQLException {
        updateColumn(columnIndex, x);
    }

    @Override
    public void updateLong(int columnIndex, long x) throws SQLException {
        updateColumn(columnIndex, x);
    }

    @Override
    public void updateFloat(int columnIndex, float x) throws SQLException {
        updateColumn(columnIndex, x);
    }

    @Override
    public void updateDouble(int columnIndex, double x) throws SQLException {
        updateColumn(columnIndex, x);
    }

    @Override
    public void updateBigDecimal(int columnIndex, BigDecimal x) throws SQLException {
        updateColumn(columnIndex, x);
    }

    @Override
    public void updateString(int columnIndex, String x) throws SQLException {
        updateColumn(columnIndex, x);
    }

    @Override
    public void updateBytes(int columnIndex, byte[] x) throws SQLException {
        updateColumn(columnIndex, x);
    }

    @Override
    public void updateDate(int columnIndex, java.sql.Date x) throws SQLException {
        updateColumn(columnIndex, x);
    }

    @Override
    public void updateTime(int columnIndex, Time x) throws SQLException {
        updateColumn(columnIndex, x);
    }

    @Override
    public void updateTimestamp(int columnIndex, Timestamp x) throws SQLException {
        updateColumn(columnIndex, x);
    }

    @Override
    public void updateAsciiStream(int columnIndex, InputStream x, int length) throws SQLException {
        updateColumn(columnIndex, readAsciiStreamValue(x, length));
    }

    @Override
    public void updateBinaryStream(int columnIndex, InputStream x, int length) throws SQLException {
        updateColumn(columnIndex, readBinaryStreamValue(x, length));
    }

    @Override
    public void updateCharacterStream(int columnIndex, Reader x, int length) throws SQLException {
        updateColumn(columnIndex, readCharacterStreamValue(x, length));
    }

    @Override
    public void updateObject(int columnIndex, Object x, int scaleOrLength) throws SQLException {
        updateColumn(columnIndex, x);
    }

    @Override
    public void updateObject(int columnIndex, Object x, SQLType targetSqlType, int scaleOrLength)
            throws SQLException {
        if (targetSqlType == null) {
            throw new SQLException("SQLType cannot be null", "HY004");
        }
        updateObject(columnIndex, x, scaleOrLength);
    }

    @Override
    public void updateObject(int columnIndex, Object x, SQLType targetSqlType) throws SQLException {
        if (targetSqlType == null) {
            throw new SQLException("SQLType cannot be null", "HY004");
        }
        updateObject(columnIndex, x);
    }

    @Override
    public void updateObject(int columnIndex, Object x) throws SQLException {
        updateColumn(columnIndex, x);
    }

    // String column variants - delegate to index versions
    @Override
    public void updateNull(String columnLabel) throws SQLException {
        updateNull(findColumn(columnLabel));
    }

    @Override
    public void updateBoolean(String columnLabel, boolean x) throws SQLException {
        updateBoolean(findColumn(columnLabel), x);
    }

    @Override
    public void updateByte(String columnLabel, byte x) throws SQLException {
        updateByte(findColumn(columnLabel), x);
    }

    @Override
    public void updateShort(String columnLabel, short x) throws SQLException {
        updateShort(findColumn(columnLabel), x);
    }

    @Override
    public void updateInt(String columnLabel, int x) throws SQLException {
        updateInt(findColumn(columnLabel), x);
    }

    @Override
    public void updateLong(String columnLabel, long x) throws SQLException {
        updateLong(findColumn(columnLabel), x);
    }

    @Override
    public void updateFloat(String columnLabel, float x) throws SQLException {
        updateFloat(findColumn(columnLabel), x);
    }

    @Override
    public void updateDouble(String columnLabel, double x) throws SQLException {
        updateDouble(findColumn(columnLabel), x);
    }

    @Override
    public void updateBigDecimal(String columnLabel, BigDecimal x) throws SQLException {
        updateBigDecimal(findColumn(columnLabel), x);
    }

    @Override
    public void updateString(String columnLabel, String x) throws SQLException {
        updateString(findColumn(columnLabel), x);
    }

    @Override
    public void updateBytes(String columnLabel, byte[] x) throws SQLException {
        updateBytes(findColumn(columnLabel), x);
    }

    @Override
    public void updateDate(String columnLabel, java.sql.Date x) throws SQLException {
        updateDate(findColumn(columnLabel), x);
    }

    @Override
    public void updateTime(String columnLabel, Time x) throws SQLException {
        updateTime(findColumn(columnLabel), x);
    }

    @Override
    public void updateTimestamp(String columnLabel, Timestamp x) throws SQLException {
        updateTimestamp(findColumn(columnLabel), x);
    }

    @Override
    public void updateAsciiStream(String columnLabel, InputStream x, int length) throws SQLException {
        updateAsciiStream(findColumn(columnLabel), x, length);
    }

    @Override
    public void updateBinaryStream(String columnLabel, InputStream x, int length) throws SQLException {
        updateBinaryStream(findColumn(columnLabel), x, length);
    }

    @Override
    public void updateCharacterStream(String columnLabel, Reader reader, int length) throws SQLException {
        updateCharacterStream(findColumn(columnLabel), reader, length);
    }

    @Override
    public void updateObject(String columnLabel, Object x, int scaleOrLength) throws SQLException {
        updateObject(findColumn(columnLabel), x, scaleOrLength);
    }

    @Override
    public void updateObject(String columnLabel, Object x, SQLType targetSqlType, int scaleOrLength)
            throws SQLException {
        if (targetSqlType == null) {
            throw new SQLException("SQLType cannot be null", "HY004");
        }
        updateObject(findColumn(columnLabel), x, scaleOrLength);
    }

    @Override
    public void updateObject(String columnLabel, Object x, SQLType targetSqlType) throws SQLException {
        if (targetSqlType == null) {
            throw new SQLException("SQLType cannot be null", "HY004");
        }
        updateObject(findColumn(columnLabel), x);
    }

    @Override
    public void updateObject(String columnLabel, Object x) throws SQLException {
        updateObject(findColumn(columnLabel), x);
    }

    @Override
    public void insertRow() throws SQLException {
        checkClosed();
        ensureUpdatable();
        if (!onInsertRow) {
            throw new SQLException("Cursor is not on insert row", "HY109");
        }
        Map<Integer, Object> values = new LinkedHashMap<>();
        for (int i = 1; i <= columns.size(); i++) {
            if (!canMutateColumn(i)) {
                continue;
            }
            Object value = insertRowBuffer[i - 1];
            String targetName = targetColumnNameOrNull(i);
            if (targetName == null && updateTarget != null) {
                continue;
            }
            values.put(i, value);
        }
        if (values.isEmpty() && updateTarget != null) {
            throw new SQLException("Insert row has no writable columns", "HY000");
        }
        executeInsert(values);
        Object[] inserted = insertRowBuffer.clone();
        if (bufferedRowsMutable) {
            try {
                rows.add(inserted);
                currentRow = rows.size() - 1;
            } catch (UnsupportedOperationException ex) {
                currentRow = rowsRead;
            }
        } else {
            currentRow = rowsRead;
        }
        currentRowData = inserted;
        rowsRead = Math.max(rowsRead, currentRow + 1);
        onInsertRow = false;
        insertRowBuffer = null;
        rowInsertedFlag = true;
        rowUpdatedFlag = false;
        rowDeletedFlag = false;
        pendingUpdates.clear();
        originalRowSnapshot = null;
    }

    @Override
    public void updateRow() throws SQLException {
        checkClosed();
        ensureUpdatable();
        ensureNotOnInsertRow();
        checkRow();
        if (pendingUpdates.isEmpty()) {
            return;
        }
        Object[] before = originalRowSnapshot != null ? originalRowSnapshot.clone() : currentRowData.clone();
        Object[] updated = currentRowData.clone();
        for (Map.Entry<Integer, Object> entry : pendingUpdates.entrySet()) {
            updated[entry.getKey() - 1] = entry.getValue();
        }
        executeUpdate(before, pendingUpdates);
        currentRowData = updated;
        if (bufferedRowsMutable && currentRow >= 0 && currentRow < rows.size()) {
            try {
                rows.set(currentRow, updated);
            } catch (UnsupportedOperationException ignored) {
                // Non-buffered semantics continue with current row only.
            }
        }
        rowUpdatedFlag = true;
        rowInsertedFlag = false;
        rowDeletedFlag = false;
        pendingUpdates.clear();
        originalRowSnapshot = null;
    }

    @Override
    public void deleteRow() throws SQLException {
        checkClosed();
        ensureUpdatable();
        ensureNotOnInsertRow();
        checkRow();
        Object[] before = currentRowData.clone();
        executeDelete(before);
        if (bufferedRowsMutable && currentRow >= 0 && currentRow < rows.size()) {
            try {
                rows.remove(currentRow);
            } catch (UnsupportedOperationException ignored) {
                // Continue using non-buffered cursor semantics.
            }
        }
        if (!bufferedRowsMutable) {
            currentRowData = null;
            currentRow = rowsRead;
            rowDeletedFlag = true;
            rowUpdatedFlag = false;
            rowInsertedFlag = false;
            pendingUpdates.clear();
            originalRowSnapshot = null;
            return;
        }
        rowDeletedFlag = true;
        rowUpdatedFlag = false;
        rowInsertedFlag = false;
        pendingUpdates.clear();
        originalRowSnapshot = null;

        if (currentRow >= rows.size()) {
            currentRow = rows.size();
            currentRowData = null;
            rowsRead = rows.size();
            if (stream instanceof ListRowStream) {
                ((ListRowStream) stream).setIndex(rows.size());
            }
            return;
        }
        if (currentRow >= 0 && currentRow < rows.size()) {
            currentRowData = rows.get(currentRow);
            rowsRead = currentRow + 1;
            if (stream instanceof ListRowStream) {
                ((ListRowStream) stream).setIndex(currentRow + 1);
            }
            return;
        }
        currentRowData = null;
        rowsRead = 0;
        if (stream instanceof ListRowStream) {
            ((ListRowStream) stream).setIndex(0);
        }
    }

    @Override
    public void refreshRow() throws SQLException {
        checkClosed();
        ensureUpdatable();
        ensureNotOnInsertRow();
        checkRow();
        Object[] keyRow = originalRowSnapshot != null ? originalRowSnapshot : currentRowData;
        Object[] reloaded = executeRefresh(keyRow);
        if (reloaded != null) {
            currentRowData = reloaded;
            if (bufferedRowsMutable && currentRow >= 0 && currentRow < rows.size()) {
                try {
                    rows.set(currentRow, reloaded);
                } catch (UnsupportedOperationException ignored) {
                    // Non-buffered semantics continue with current row only.
                }
            }
            pendingUpdates.clear();
            originalRowSnapshot = null;
            clearRowActionFlags();
        }
    }

    @Override
    public void cancelRowUpdates() throws SQLException {
        checkClosed();
        ensureUpdatable();
        if (!onInsertRow && originalRowSnapshot != null) {
            currentRowData = originalRowSnapshot.clone();
            if (bufferedRowsMutable && currentRow >= 0 && currentRow < rows.size()) {
                try {
                    rows.set(currentRow, currentRowData);
                } catch (UnsupportedOperationException ignored) {
                    // Non-buffered semantics continue with current row only.
                }
            }
        }
        pendingUpdates.clear();
        originalRowSnapshot = null;
        if (onInsertRow && insertRowBuffer != null) {
            Arrays.fill(insertRowBuffer, null);
        }
        clearRowActionFlags();
    }

    @Override
    public void moveToInsertRow() throws SQLException {
        checkClosed();
        ensureUpdatable();
        if (onInsertRow) {
            return;
        }
        savedCurrentRow = currentRow;
        savedCurrentRowData = currentRowData;
        onInsertRow = true;
        insertRowBuffer = new Object[columns.size()];
        clearRowActionFlags();
        pendingUpdates.clear();
        originalRowSnapshot = null;
    }

    @Override
    public void moveToCurrentRow() throws SQLException {
        checkClosed();
        if (!onInsertRow) {
            return;
        }
        onInsertRow = false;
        insertRowBuffer = null;
        currentRow = savedCurrentRow;
        currentRowData = savedCurrentRowData;
        savedCurrentRow = -1;
        savedCurrentRowData = null;
        pendingUpdates.clear();
        originalRowSnapshot = null;
        clearRowActionFlags();
    }

    @Override
    public Statement getStatement() throws SQLException {
        checkClosed();
        return statement;
    }

    // ==================== Additional Methods ====================

    @Override
    public Ref getRef(int columnIndex) throws SQLException {
        return SBRef.fromObject(getObject(columnIndex));
    }

    @Override
    public Ref getRef(String columnLabel) throws SQLException {
        return getRef(findColumn(columnLabel));
    }

    @Override
    public Blob getBlob(int columnIndex) throws SQLException {
        byte[] bytes = getBytes(columnIndex);
        if (bytes == null) return null;
        return new SBBlob(bytes);
    }

    @Override
    public Blob getBlob(String columnLabel) throws SQLException {
        return getBlob(findColumn(columnLabel));
    }

    @Override
    public Clob getClob(int columnIndex) throws SQLException {
        String s = getString(columnIndex);
        if (s == null) return null;
        return new SBClob(s);
    }

    @Override
    public Clob getClob(String columnLabel) throws SQLException {
        return getClob(findColumn(columnLabel));
    }

    @Override
    public Array getArray(int columnIndex) throws SQLException {
        Object value = getObject(columnIndex);
        if (value == null) return null;
        if (value instanceof Array) return (Array) value;
        if (value instanceof Object[]) {
            Object[] elements = (Object[]) value;
            return new SBArray(inferArrayBaseType(elements), elements);
        }
        if (value instanceof Collection<?>) {
            Object[] elements = ((Collection<?>) value).toArray();
            return new SBArray(inferArrayBaseType(elements), elements);
        }
        if (value instanceof String) {
            Object[] elements = parseArrayLiteral((String) value);
            return new SBArray(inferArrayBaseType(elements), elements);
        }
        throw new SQLException("Not an array type", "HY000");
    }

    @Override
    public Array getArray(String columnLabel) throws SQLException {
        return getArray(findColumn(columnLabel));
    }

    @Override
    public URL getURL(int columnIndex) throws SQLException {
        String s = getString(columnIndex);
        if (s == null) return null;
        try {
            return new URL(s);
        } catch (MalformedURLException e) {
            throw new SQLException("Invalid URL: " + s, "HY000", e);
        }
    }

    @Override
    public URL getURL(String columnLabel) throws SQLException {
        return getURL(findColumn(columnLabel));
    }

    @Override
    public void updateRef(int columnIndex, Ref x) throws SQLException {
        updateColumn(columnIndex, x == null ? null : x.getObject());
    }

    @Override
    public void updateRef(String columnLabel, Ref x) throws SQLException {
        updateRef(findColumn(columnLabel), x);
    }

    @Override
    public void updateBlob(int columnIndex, Blob x) throws SQLException {
        updateColumn(columnIndex, x == null ? null : x.getBytes(1, (int) x.length()));
    }

    @Override
    public void updateBlob(String columnLabel, Blob x) throws SQLException {
        updateBlob(findColumn(columnLabel), x);
    }

    @Override
    public void updateClob(int columnIndex, Clob x) throws SQLException {
        updateColumn(columnIndex, x == null ? null : x.getSubString(1, (int) x.length()));
    }

    @Override
    public void updateClob(String columnLabel, Clob x) throws SQLException {
        updateClob(findColumn(columnLabel), x);
    }

    @Override
    public void updateArray(int columnIndex, Array x) throws SQLException {
        updateColumn(columnIndex, x == null ? null : x.getArray());
    }

    @Override
    public void updateArray(String columnLabel, Array x) throws SQLException {
        updateArray(findColumn(columnLabel), x);
    }

    @Override
    public RowId getRowId(int columnIndex) throws SQLException {
        return SBRowId.fromObject(getObject(columnIndex));
    }

    @Override
    public RowId getRowId(String columnLabel) throws SQLException {
        return getRowId(findColumn(columnLabel));
    }

    @Override
    public void updateRowId(int columnIndex, RowId x) throws SQLException {
        updateColumn(columnIndex, x == null ? null : new String(x.getBytes(), StandardCharsets.UTF_8));
    }

    @Override
    public void updateRowId(String columnLabel, RowId x) throws SQLException {
        updateRowId(findColumn(columnLabel), x);
    }

    @Override
    public int getHoldability() throws SQLException {
        checkClosed();
        if (statement != null) {
            return statement.resultSetHoldability;
        }
        return ResultSet.HOLD_CURSORS_OVER_COMMIT;
    }

    @Override
    public boolean isClosed() throws SQLException {
        return closed.get();
    }

    @Override
    public void updateNString(int columnIndex, String nString) throws SQLException {
        updateColumn(columnIndex, nString);
    }

    @Override
    public void updateNString(String columnLabel, String nString) throws SQLException {
        updateNString(findColumn(columnLabel), nString);
    }

    @Override
    public void updateNClob(int columnIndex, NClob nClob) throws SQLException {
        updateColumn(columnIndex, nClob == null ? null : nClob.getSubString(1, (int) nClob.length()));
    }

    @Override
    public void updateNClob(String columnLabel, NClob nClob) throws SQLException {
        updateNClob(findColumn(columnLabel), nClob);
    }

    @Override
    public NClob getNClob(int columnIndex) throws SQLException {
        String s = getString(columnIndex);
        if (s == null) return null;
        return new SBNClob(s);
    }

    @Override
    public NClob getNClob(String columnLabel) throws SQLException {
        return getNClob(findColumn(columnLabel));
    }

    @Override
    public SQLXML getSQLXML(int columnIndex) throws SQLException {
        String s = getString(columnIndex);
        if (s == null) return null;
        return new SBSQLXML(s);
    }

    @Override
    public SQLXML getSQLXML(String columnLabel) throws SQLException {
        return getSQLXML(findColumn(columnLabel));
    }

    @Override
    public void updateSQLXML(int columnIndex, SQLXML xmlObject) throws SQLException {
        updateColumn(columnIndex, xmlObject == null ? null : xmlObject.getString());
    }

    @Override
    public void updateSQLXML(String columnLabel, SQLXML xmlObject) throws SQLException {
        updateSQLXML(findColumn(columnLabel), xmlObject);
    }

    @Override
    public String getNString(int columnIndex) throws SQLException {
        return getString(columnIndex);
    }

    @Override
    public String getNString(String columnLabel) throws SQLException {
        return getString(columnLabel);
    }

    @Override
    public Reader getNCharacterStream(int columnIndex) throws SQLException {
        return getCharacterStream(columnIndex);
    }

    @Override
    public Reader getNCharacterStream(String columnLabel) throws SQLException {
        return getCharacterStream(columnLabel);
    }

    @Override
    public void updateNCharacterStream(int columnIndex, Reader x, long length) throws SQLException {
        updateColumn(columnIndex, readCharacterStreamValue(x, length));
    }

    @Override
    public void updateNCharacterStream(String columnLabel, Reader reader, long length) throws SQLException {
        updateNCharacterStream(findColumn(columnLabel), reader, length);
    }

    @Override
    public void updateAsciiStream(int columnIndex, InputStream x, long length) throws SQLException {
        updateColumn(columnIndex, readAsciiStreamValue(x, length));
    }

    @Override
    public void updateBinaryStream(int columnIndex, InputStream x, long length) throws SQLException {
        updateColumn(columnIndex, readBinaryStreamValue(x, length));
    }

    @Override
    public void updateCharacterStream(int columnIndex, Reader x, long length) throws SQLException {
        updateColumn(columnIndex, readCharacterStreamValue(x, length));
    }

    @Override
    public void updateAsciiStream(String columnLabel, InputStream x, long length) throws SQLException {
        updateAsciiStream(findColumn(columnLabel), x, length);
    }

    @Override
    public void updateBinaryStream(String columnLabel, InputStream x, long length) throws SQLException {
        updateBinaryStream(findColumn(columnLabel), x, length);
    }

    @Override
    public void updateCharacterStream(String columnLabel, Reader reader, long length) throws SQLException {
        updateCharacterStream(findColumn(columnLabel), reader, length);
    }

    @Override
    public void updateBlob(int columnIndex, InputStream inputStream, long length) throws SQLException {
        updateColumn(columnIndex, readBinaryStreamValue(inputStream, length));
    }

    @Override
    public void updateBlob(String columnLabel, InputStream inputStream, long length) throws SQLException {
        updateBlob(findColumn(columnLabel), inputStream, length);
    }

    @Override
    public void updateClob(int columnIndex, Reader reader, long length) throws SQLException {
        updateColumn(columnIndex, readCharacterStreamValue(reader, length));
    }

    @Override
    public void updateClob(String columnLabel, Reader reader, long length) throws SQLException {
        updateClob(findColumn(columnLabel), reader, length);
    }

    @Override
    public void updateNClob(int columnIndex, Reader reader, long length) throws SQLException {
        updateColumn(columnIndex, readCharacterStreamValue(reader, length));
    }

    @Override
    public void updateNClob(String columnLabel, Reader reader, long length) throws SQLException {
        updateNClob(findColumn(columnLabel), reader, length);
    }

    @Override
    public void updateNCharacterStream(int columnIndex, Reader x) throws SQLException {
        updateColumn(columnIndex, readCharacterStreamValue(x));
    }

    @Override
    public void updateNCharacterStream(String columnLabel, Reader reader) throws SQLException {
        updateNCharacterStream(findColumn(columnLabel), reader);
    }

    @Override
    public void updateAsciiStream(int columnIndex, InputStream x) throws SQLException {
        updateColumn(columnIndex, readAsciiStreamValue(x));
    }

    @Override
    public void updateBinaryStream(int columnIndex, InputStream x) throws SQLException {
        updateColumn(columnIndex, readBinaryStreamValue(x));
    }

    @Override
    public void updateCharacterStream(int columnIndex, Reader x) throws SQLException {
        updateColumn(columnIndex, readCharacterStreamValue(x));
    }

    @Override
    public void updateAsciiStream(String columnLabel, InputStream x) throws SQLException {
        updateAsciiStream(findColumn(columnLabel), x);
    }

    @Override
    public void updateBinaryStream(String columnLabel, InputStream x) throws SQLException {
        updateBinaryStream(findColumn(columnLabel), x);
    }

    @Override
    public void updateCharacterStream(String columnLabel, Reader reader) throws SQLException {
        updateCharacterStream(findColumn(columnLabel), reader);
    }

    @Override
    public void updateBlob(int columnIndex, InputStream inputStream) throws SQLException {
        updateColumn(columnIndex, readBinaryStreamValue(inputStream));
    }

    @Override
    public void updateBlob(String columnLabel, InputStream inputStream) throws SQLException {
        updateBlob(findColumn(columnLabel), inputStream);
    }

    @Override
    public void updateClob(int columnIndex, Reader reader) throws SQLException {
        updateColumn(columnIndex, readCharacterStreamValue(reader));
    }

    @Override
    public void updateClob(String columnLabel, Reader reader) throws SQLException {
        updateClob(findColumn(columnLabel), reader);
    }

    @Override
    public void updateNClob(int columnIndex, Reader reader) throws SQLException {
        updateColumn(columnIndex, readCharacterStreamValue(reader));
    }

    @Override
    public void updateNClob(String columnLabel, Reader reader) throws SQLException {
        updateNClob(findColumn(columnLabel), reader);
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

    // ==================== Helper Methods ====================

    private static final class UpdateTarget {
        private final String tableSql;
        private final Map<Integer, String> columnNamesByIndex;
        private final Map<Integer, String> tableSqlByIndex;
        private final Set<Integer> writableColumns;

        private UpdateTarget(String tableSql, Map<Integer, String> columnNamesByIndex,
                             Map<Integer, String> tableSqlByIndex,
                             Set<Integer> writableColumns) {
            this.tableSql = tableSql;
            this.columnNamesByIndex = columnNamesByIndex == null
                ? Collections.emptyMap()
                : Collections.unmodifiableMap(new LinkedHashMap<>(columnNamesByIndex));
            this.tableSqlByIndex = tableSqlByIndex == null
                ? Collections.emptyMap()
                : Collections.unmodifiableMap(new LinkedHashMap<>(tableSqlByIndex));
            this.writableColumns = writableColumns == null
                ? Collections.emptySet()
                : Collections.unmodifiableSet(new LinkedHashSet<>(writableColumns));
        }

        private boolean usesExplicitColumnMapping() {
            return !columnNamesByIndex.isEmpty();
        }

        private boolean hasWritableColumns() {
            return !writableColumns.isEmpty();
        }

        private boolean isColumnWritable(int columnIndex) {
            return writableColumns.contains(columnIndex);
        }

        private String mappedColumnName(int columnIndex) {
            return columnNamesByIndex.get(columnIndex);
        }

        private String mappedTableSql(int columnIndex) {
            String mapped = tableSqlByIndex.get(columnIndex);
            if (mapped != null && !mapped.isBlank()) {
                return mapped;
            }
            return tableSql;
        }
    }

    private static final class ProjectionTarget {
        private final String columnName;
        private final String tableQualifier;

        private ProjectionTarget(String columnName, String tableQualifier) {
            this.columnName = columnName;
            this.tableQualifier = tableQualifier;
        }
    }

    private static final class WithClauseParseResult {
        private final String mainQuery;
        private final Map<String, String> cteSqlByName;

        private WithClauseParseResult(String mainQuery, Map<String, String> cteSqlByName) {
            this.mainQuery = mainQuery;
            this.cteSqlByName = cteSqlByName == null ? Collections.emptyMap() : cteSqlByName;
        }
    }

    private static UpdateTarget resolveUpdateTarget(SBStatement statement, List<SBColumnInfo> columns) {
        if (statement == null) {
            return null;
        }
        UpdateTarget fromMetadata = resolveUpdateTargetFromMetadata(statement, columns);
        if (fromMetadata != null) {
            return fromMetadata;
        }
        return resolveUpdateTargetFromSql(statement, columns);
    }

    private static UpdateTarget resolveUpdateTargetFromSql(SBStatement statement, List<SBColumnInfo> columns) {
        String sql = statement.getLastExecutedSql();
        if (sql == null || sql.isBlank()) {
            return null;
        }
        UpdateTarget strict = resolveUpdateTargetFromSelectSql(statement, sql, columns, 0);
        if (strict != null) {
            return strict;
        }
        return resolveUpdateTargetLenientFromSingleTable(statement, sql, columns);
    }

    private static UpdateTarget resolveUpdateTargetLenientFromSingleTable(
            SBStatement statement, String sql, List<SBColumnInfo> columns) {
        if (statement == null || sql == null || sql.isBlank() || columns == null || columns.isEmpty()) {
            return null;
        }
        String collapsed = collapseWhitespace(sql);
        WithClauseParseResult withClause = parseLeadingWithClause(collapsed);
        if (withClause != null && withClause.mainQuery != null && !withClause.mainQuery.isBlank()) {
            collapsed = withClause.mainQuery;
        }
        String normalized = collapsed.toLowerCase(Locale.ROOT);
        int fromIndex = findTopLevelKeyword(normalized, " from ");
        if (fromIndex < 0) {
            return null;
        }
        String afterFromOriginal = collapsed.substring(fromIndex + 6).trim();
        if (afterFromOriginal.isEmpty()) {
            return null;
        }
        String tableToken = firstToken(afterFromOriginal);
        if (tableToken == null || tableToken.isBlank() || tableToken.startsWith("(")) {
            return null;
        }
        String primaryTableSql = resolvePrimaryTableSql(afterFromOriginal, tableToken);
        if (primaryTableSql == null || primaryTableSql.isBlank()) {
            return null;
        }

        Set<String> tableColumns = resolveTableColumnNames(statement, primaryTableSql);
        if (tableColumns.isEmpty()) {
            return null;
        }

        Map<Integer, String> namesByIndex = new LinkedHashMap<>();
        Map<Integer, String> tableByIndex = new LinkedHashMap<>();
        for (int i = 0; i < columns.size(); i++) {
            SBColumnInfo column = columns.get(i);
            if (column == null || column.getName() == null || column.getName().isBlank()) {
                continue;
            }
            String normalizedName = unquoteIdentifier(column.getName()).toLowerCase(Locale.ROOT);
            if (!tableColumns.contains(normalizedName)) {
                continue;
            }
            int index = i + 1;
            namesByIndex.put(index, unquoteIdentifier(column.getName()));
            tableByIndex.put(index, primaryTableSql);
        }
        if (namesByIndex.isEmpty()) {
            return null;
        }

        return new UpdateTarget(
            primaryTableSql,
            namesByIndex,
            tableByIndex,
            new LinkedHashSet<>(namesByIndex.keySet())
        );
    }

    private static UpdateTarget resolveUpdateTargetFromSelectSql(
            SBStatement statement, String sql, List<SBColumnInfo> columns, int depth) {
        return resolveUpdateTargetFromSelectSql(statement, sql, columns, depth, Collections.emptyMap());
    }

    private static UpdateTarget resolveUpdateTargetFromSelectSql(
            SBStatement statement,
            String sql,
            List<SBColumnInfo> columns,
            int depth,
            Map<String, String> cteSqlByName) {
        if (sql == null || sql.isBlank() || depth > 3) {
            return null;
        }
        String collapsed = collapseWhitespace(sql);
        String normalized = collapsed.toLowerCase(Locale.ROOT);
        WithClauseParseResult withClause = parseLeadingWithClause(collapsed);
        if (withClause != null) {
            Map<String, String> mergedCteMap = new LinkedHashMap<>();
            if (cteSqlByName != null && !cteSqlByName.isEmpty()) {
                mergedCteMap.putAll(cteSqlByName);
            }
            if (withClause.cteSqlByName != null && !withClause.cteSqlByName.isEmpty()) {
                mergedCteMap.putAll(withClause.cteSqlByName);
            }
            return resolveUpdateTargetFromSelectSql(
                statement,
                withClause.mainQuery,
                columns,
                depth + 1,
                mergedCteMap
            );
        }
        if (!normalized.startsWith("select ")) {
            return null;
        }
        if (normalized.contains(" union ")
            || normalized.contains(" intersect ")
            || normalized.contains(" except ")
            || normalized.contains(" group by ")
            || normalized.contains(" having ")
            || normalized.contains(" distinct ")) {
            return null;
        }

        int fromIndex = findTopLevelKeyword(normalized, " from ");
        if (fromIndex < 0) {
            return null;
        }
        String projectionSql = collapsed.substring("select ".length(), fromIndex).trim();
        if (projectionSql.isEmpty()) {
            return null;
        }

        String afterFromOriginal = collapsed.substring(fromIndex + 6).trim();
        if (afterFromOriginal.isEmpty()) {
            return null;
        }
        String tableToken = firstToken(afterFromOriginal);
        if (tableToken == null || tableToken.isBlank()) {
            return null;
        }
        String tableTokenKey = normalizeIdentifierKey(tableToken);
        if (cteSqlByName != null && !cteSqlByName.isEmpty()) {
            String cteSql = cteSqlByName.get(tableTokenKey);
            if (cteSql != null && !cteSql.isBlank()) {
                UpdateTarget cteTarget = resolveUpdateTargetFromSelectSql(
                    statement,
                    cteSql,
                    columns,
                    depth + 1,
                    cteSqlByName
                );
                if (cteTarget != null) {
                    return cteTarget;
                }
            }
        }
        if (tableToken.startsWith("(")) {
            String nestedSelect = extractLeadingParenthesizedSelect(afterFromOriginal);
            if (nestedSelect == null || nestedSelect.isBlank()) {
                return null;
            }
            UpdateTarget nestedTarget = resolveUpdateTargetFromSelectSql(
                statement,
                nestedSelect,
                columns,
                depth + 1,
                cteSqlByName
            );
            if (nestedTarget != null) {
                return nestedTarget;
            }
        }
        Map<String, String> tableSqlByQualifier = parseFromClauseTableMapping(afterFromOriginal);
        String primaryTableSql = resolvePrimaryTableSql(afterFromOriginal, tableToken);
        if (primaryTableSql == null || primaryTableSql.isBlank()) {
            primaryTableSql = tableToken;
        }

        Map<Integer, ProjectionTarget> projectionTargets = resolveProjectionTargets(projectionSql, columns);
        if (projectionTargets == null || projectionTargets.isEmpty()) {
            projectionTargets = resolveProjectionTargetsFromResultColumns(statement, columns, primaryTableSql);
            if (projectionTargets == null || projectionTargets.isEmpty()) {
                return null;
            }
        }

        Map<Integer, String> namesByIndex = new LinkedHashMap<>();
        Map<Integer, String> tableByIndex = new LinkedHashMap<>();
        for (Map.Entry<Integer, ProjectionTarget> entry : projectionTargets.entrySet()) {
            ProjectionTarget target = entry.getValue();
            if (target == null || target.columnName == null || target.columnName.isBlank()) {
                continue;
            }
            int columnIndex = entry.getKey();
            namesByIndex.put(columnIndex, target.columnName);

            String mappedTableSql = resolveTableSqlForQualifier(
                tableSqlByQualifier,
                target.tableQualifier,
                primaryTableSql
            );
            if (mappedTableSql != null && !mappedTableSql.isBlank()) {
                tableByIndex.put(columnIndex, mappedTableSql);
            }
        }
        if (namesByIndex.isEmpty()) {
            return null;
        }
        if (tableByIndex.isEmpty()) {
            for (Integer index : namesByIndex.keySet()) {
                tableByIndex.put(index, primaryTableSql);
            }
        }
        return new UpdateTarget(
            primaryTableSql,
            namesByIndex,
            tableByIndex,
            new LinkedHashSet<>(namesByIndex.keySet())
        );
    }

    private static Map<Integer, ProjectionTarget> resolveProjectionTargetsFromResultColumns(
            SBStatement statement, List<SBColumnInfo> columns, String primaryTableSql) {
        if (statement == null || columns == null || columns.isEmpty()
            || primaryTableSql == null || primaryTableSql.isBlank()) {
            return Collections.emptyMap();
        }
        Set<String> tableColumns = resolveTableColumnNames(statement, primaryTableSql);
        if (tableColumns.isEmpty()) {
            return Collections.emptyMap();
        }
        Map<Integer, ProjectionTarget> mapped = new LinkedHashMap<>();
        for (int i = 0; i < columns.size(); i++) {
            SBColumnInfo column = columns.get(i);
            if (column == null || column.getName() == null || column.getName().isBlank()) {
                continue;
            }
            String normalized = unquoteIdentifier(column.getName()).toLowerCase(Locale.ROOT);
            if (!tableColumns.contains(normalized)) {
                continue;
            }
            mapped.put(i + 1, new ProjectionTarget(unquoteIdentifier(column.getName()), null));
        }
        return mapped;
    }

    private static Set<String> resolveTableColumnNames(SBStatement statement, String tableSql) {
        if (statement == null || statement.connection == null || tableSql == null || tableSql.isBlank()) {
            return Collections.emptySet();
        }
        String[] parsed = parseQualifiedTableSql(tableSql);
        String schema = parsed[0] == null ? "" : parsed[0].trim();
        String table = parsed[1] == null ? "" : parsed[1].trim();
        if (table.isBlank()) {
            return Collections.emptySet();
        }
        String escapedTable = table.replace("'", "''");
        String sql;
        if (schema.isBlank()) {
            sql = "SELECT column_name FROM information_schema.columns "
                + "WHERE table_name = '" + escapedTable + "'";
        } else {
            String escapedSchema = schema.replace("'", "''");
            sql = "SELECT column_name FROM information_schema.columns "
                + "WHERE table_schema = '" + escapedSchema + "' "
                + "AND table_name = '" + escapedTable + "'";
        }
        try {
            SBQueryResult result = statement.connection.getProtocol().execute(sql, 0, 0);
            if (result == null || result.getRows() == null || result.getRows().isEmpty()) {
                return Collections.emptySet();
            }
            Set<String> names = new HashSet<>();
            for (Object[] row : result.getRows()) {
                if (row == null || row.length == 0 || row[0] == null) {
                    continue;
                }
                String value = row[0].toString().trim();
                if (!value.isBlank()) {
                    names.add(value.toLowerCase(Locale.ROOT));
                }
            }
            return names;
        } catch (SQLException ex) {
            return Collections.emptySet();
        }
    }

    private static String extractLeadingParenthesizedSelect(String sql) {
        if (sql == null) {
            return null;
        }
        String trimmed = sql.trim();
        if (trimmed.isEmpty() || trimmed.charAt(0) != '(') {
            return null;
        }
        boolean inSingle = false;
        boolean inDouble = false;
        int depth = 0;
        for (int i = 0; i < trimmed.length(); i++) {
            char c = trimmed.charAt(i);
            if (c == '\'' && !inDouble) {
                inSingle = !inSingle;
                continue;
            }
            if (c == '"' && !inSingle) {
                inDouble = !inDouble;
                continue;
            }
            if (inSingle || inDouble) {
                continue;
            }
            if (c == '(') {
                depth++;
                continue;
            }
            if (c == ')') {
                depth--;
                if (depth == 0) {
                    return trimmed.substring(1, i).trim();
                }
            }
        }
        return null;
    }

    private Set<Integer> writableMetadataColumns() {
        if (!updatable || columns == null || columns.isEmpty()) {
            return Collections.emptySet();
        }
        Set<Integer> writable = new HashSet<>();
        for (int i = 1; i <= columns.size(); i++) {
            if (canMutateColumn(i)) {
                writable.add(i);
            }
        }
        return writable;
    }

    private Set<Integer> autoIncrementMetadataColumns() {
        if (statement == null || statement.connection == null || columns == null || columns.isEmpty()) {
            return Collections.emptySet();
        }
        Set<Integer> autoIncrementColumns = new HashSet<>();
        Map<String, Boolean> cache = new HashMap<>();
        for (int i = 0; i < columns.size(); i++) {
            SBColumnInfo column = columns.get(i);
            if (column == null || column.getTableOid() <= 0 || column.getColumnNumber() <= 0) {
                continue;
            }
            int tableOid = column.getTableOid();
            int columnNumber = column.getColumnNumber();
            String cacheKey = tableOid + ":" + columnNumber;
            Boolean autoIncrement = cache.get(cacheKey);
            if (autoIncrement == null) {
                autoIncrement = resolveAutoIncrementColumn(statement, tableOid, columnNumber);
                cache.put(cacheKey, autoIncrement);
            }
            if (Boolean.TRUE.equals(autoIncrement)) {
                autoIncrementColumns.add(i + 1);
            }
        }
        return autoIncrementColumns;
    }

    private static boolean resolveAutoIncrementColumn(SBStatement statement, int tableOid, int columnNumber) {
        String sql = "SELECT a.attidentity, pg_get_expr(ad.adbin, ad.adrelid) "
            + "FROM pg_catalog.pg_attribute a "
            + "LEFT JOIN pg_catalog.pg_attrdef ad "
            + "  ON ad.adrelid = a.attrelid "
            + " AND ad.adnum = a.attnum "
            + "WHERE a.attrelid = " + tableOid + " "
            + "  AND a.attnum = " + columnNumber;
        try {
            SBQueryResult result = statement.connection.getProtocol().execute(sql, 1, 0);
            if (result == null || result.getRows() == null || result.getRows().isEmpty()) {
                return false;
            }
            Object[] row = result.getRows().get(0);
            if (row == null || row.length == 0) {
                return false;
            }
            String identity = row[0] != null ? row[0].toString().trim() : "";
            if (!identity.isEmpty() && !"0".equals(identity)) {
                return true;
            }
            if (row.length < 2 || row[1] == null) {
                return false;
            }
            String defaultExpr = row[1].toString().toLowerCase(Locale.ROOT);
            return defaultExpr.contains("nextval(")
                || (defaultExpr.contains("generated") && defaultExpr.contains("identity"));
        } catch (SQLException ex) {
            return false;
        }
    }

    private static String[] parseQualifiedTableSql(String tableSql) {
        if (tableSql == null || tableSql.isBlank()) {
            return new String[]{"", ""};
        }
        List<String> parts = splitIdentifierParts(tableSql);
        if (parts.isEmpty()) {
            return new String[]{"", unquoteIdentifier(tableSql)};
        }
        if (parts.size() == 1) {
            return new String[]{"", unquoteIdentifier(parts.get(0))};
        }
        StringBuilder schema = new StringBuilder();
        for (int i = 0; i < parts.size() - 1; i++) {
            if (i > 0) {
                schema.append('.');
            }
            schema.append(unquoteIdentifier(parts.get(i)));
        }
        return new String[]{
            schema.toString(),
            unquoteIdentifier(parts.get(parts.size() - 1))
        };
    }

    private static UpdateTarget resolveUpdateTargetFromMetadata(SBStatement statement, List<SBColumnInfo> columns) {
        if (statement == null || statement.connection == null || columns == null || columns.isEmpty()) {
            return null;
        }
        Map<Integer, Integer> tableCounts = new LinkedHashMap<>();
        for (SBColumnInfo column : columns) {
            if (column == null || column.getTableOid() <= 0 || column.getColumnNumber() <= 0) {
                continue;
            }
            tableCounts.merge(column.getTableOid(), 1, Integer::sum);
        }
        if (tableCounts.isEmpty()) {
            return null;
        }
        Integer tableOid = null;
        int tableCount = -1;
        for (Map.Entry<Integer, Integer> entry : tableCounts.entrySet()) {
            if (entry.getValue() > tableCount) {
                tableOid = entry.getKey();
                tableCount = entry.getValue();
            }
        }
        if (tableOid == null) {
            return null;
        }
        String tableSql = resolveTableSql(statement, tableOid);
        if (tableSql == null) {
            return null;
        }
        Map<Integer, String> namesByAttNum = resolveAttnumNames(statement, tableOid);
        if (namesByAttNum.isEmpty()) {
            return null;
        }
        Map<Integer, String> namesByIndex = new LinkedHashMap<>();
        Map<Integer, String> tableByIndex = new LinkedHashMap<>();
        for (int i = 0; i < columns.size(); i++) {
            SBColumnInfo column = columns.get(i);
            if (column == null
                || column.getColumnNumber() <= 0) {
                continue;
            }
            int mappedTableOid = column.getTableOid();
            if (mappedTableOid <= 0) {
                continue;
            }
            String mappedTableSql = resolveTableSql(statement, mappedTableOid);
            if (mappedTableSql == null || mappedTableSql.isBlank()) {
                continue;
            }
            Map<Integer, String> mappedAttnumNames = mappedTableOid == tableOid
                ? namesByAttNum
                : resolveAttnumNames(statement, mappedTableOid);
            if (mappedAttnumNames.isEmpty()) {
                continue;
            }
            String mapped = mappedAttnumNames.get((int) column.getColumnNumber());
            if (mapped != null && !mapped.isBlank()) {
                namesByIndex.put(i + 1, mapped);
                tableByIndex.put(i + 1, mappedTableSql);
            }
        }
        if (namesByIndex.isEmpty()) {
            return null;
        }
        return new UpdateTarget(
            tableSql,
            namesByIndex,
            tableByIndex,
            new LinkedHashSet<>(namesByIndex.keySet())
        );
    }

    private static String resolveTableSql(SBStatement statement, int tableOid) {
        if (tableOid <= 0) {
            return null;
        }
        TableCacheKey cacheKey = tableCacheKey(statement, tableOid);
        String cached = TABLE_SQL_BY_KEY.get(cacheKey);
        if (cached != null && !cached.isBlank()) {
            return cached;
        }
        String sql = "SELECT n.nspname, c.relname "
            + "FROM pg_catalog.pg_class c "
            + "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
            + "WHERE c.oid = " + tableOid;
        try {
            SBQueryResult result = statement.connection.getProtocol().execute(sql, 1, 0);
            if (result.getRows() == null || result.getRows().isEmpty()) {
                return null;
            }
            Object[] row = result.getRows().get(0);
            String schema = row != null && row.length > 0 && row[0] != null ? row[0].toString() : null;
            String name = row != null && row.length > 1 && row[1] != null ? row[1].toString() : null;
            if (schema == null || schema.isBlank() || name == null || name.isBlank()) {
                return null;
            }
            String tableSql = quoteIdentifier(schema) + "." + quoteIdentifier(name);
            TABLE_SQL_BY_KEY.put(cacheKey, tableSql);
            return tableSql;
        } catch (SQLException ex) {
            return null;
        }
    }

    private static Map<Integer, String> resolveAttnumNames(SBStatement statement, int tableOid) {
        if (tableOid <= 0) {
            return Collections.emptyMap();
        }
        TableCacheKey cacheKey = tableCacheKey(statement, tableOid);
        Map<Integer, String> cached = TABLE_COLUMN_BY_KEY_AND_ATTNUM.get(cacheKey);
        if (cached != null && !cached.isEmpty()) {
            return cached;
        }
        String sql = "SELECT attnum, attname "
            + "FROM pg_catalog.pg_attribute "
            + "WHERE attrelid = " + tableOid + " "
            + "  AND attnum > 0 "
            + "  AND NOT attisdropped";
        try {
            SBQueryResult result = statement.connection.getProtocol().execute(sql, 0, 0);
            Map<Integer, String> mapped = new HashMap<>();
            if (result.getRows() != null) {
                for (Object[] row : result.getRows()) {
                    if (row == null || row.length < 2 || row[0] == null || row[1] == null) {
                        continue;
                    }
                    int attNum;
                    try {
                        attNum = Integer.parseInt(row[0].toString());
                    } catch (NumberFormatException ex) {
                        continue;
                    }
                    String attName = row[1].toString();
                    if (!attName.isBlank()) {
                        mapped.put(attNum, attName);
                    }
                }
            }
            if (!mapped.isEmpty()) {
                TABLE_COLUMN_BY_KEY_AND_ATTNUM.put(cacheKey, mapped);
            }
            return mapped;
        } catch (SQLException ex) {
            return Collections.emptyMap();
        }
    }

    private static TableCacheKey tableCacheKey(SBStatement statement, int tableOid) {
        if (statement == null || statement.connection == null) {
            return new TableCacheKey("", tableOid);
        }
        SBConnectionProperties properties = statement.connection.getConnectionProperties();
        if (properties == null) {
            return new TableCacheKey("", tableOid);
        }
        String namespace = (properties.getHost() + ":" + properties.getPort() + "/" + properties.getDatabase())
            .toLowerCase(Locale.ROOT);
        return new TableCacheKey(namespace, tableOid);
    }

    private static String collapseWhitespace(String sql) {
        StringBuilder out = new StringBuilder(sql.length());
        boolean inSingle = false;
        boolean inDouble = false;
        boolean sawWhitespace = false;
        for (int i = 0; i < sql.length(); i++) {
            char c = sql.charAt(i);
            if (c == '\'' && !inDouble) {
                inSingle = !inSingle;
                out.append(c);
                sawWhitespace = false;
                continue;
            }
            if (c == '"' && !inSingle) {
                inDouble = !inDouble;
                out.append(c);
                sawWhitespace = false;
                continue;
            }
            if (!inSingle && !inDouble && Character.isWhitespace(c)) {
                if (!sawWhitespace) {
                    out.append(' ');
                    sawWhitespace = true;
                }
                continue;
            }
            out.append(c);
            sawWhitespace = false;
        }
        return out.toString().trim();
    }

    private static final class IdentifierTokenParseResult {
        private final String token;
        private final int nextIndex;

        private IdentifierTokenParseResult(String token, int nextIndex) {
            this.token = token;
            this.nextIndex = nextIndex;
        }
    }

    private static WithClauseParseResult parseLeadingWithClause(String sql) {
        if (sql == null || sql.isBlank()) {
            return null;
        }
        String trimmed = sql.trim();
        if (!startsWithWordAt(trimmed, 0, "with")) {
            return null;
        }

        int index = skipWhitespace(trimmed, "with".length());
        if (startsWithWordAt(trimmed, index, "recursive")) {
            index = skipWhitespace(trimmed, index + "recursive".length());
        }

        if (index >= trimmed.length()) {
            return null;
        }

        Map<String, String> cteSqlByName = new LinkedHashMap<>();
        while (index < trimmed.length()) {
            IdentifierTokenParseResult cteName = parseIdentifierToken(trimmed, index);
            if (cteName == null || cteName.token == null || cteName.token.isBlank()) {
                return null;
            }
            String cteKey = normalizeIdentifierKey(cteName.token);
            index = skipWhitespace(trimmed, cteName.nextIndex);

            if (index < trimmed.length() && trimmed.charAt(index) == '(') {
                int close = findMatchingParenthesis(trimmed, index);
                if (close < 0) {
                    return null;
                }
                index = skipWhitespace(trimmed, close + 1);
            }

            if (!startsWithWordAt(trimmed, index, "as")) {
                return null;
            }
            index = skipWhitespace(trimmed, index + 2);
            if (index >= trimmed.length() || trimmed.charAt(index) != '(') {
                return null;
            }
            int close = findMatchingParenthesis(trimmed, index);
            if (close < 0) {
                return null;
            }
            String cteSql = trimmed.substring(index + 1, close).trim();
            if (!cteSql.isBlank()) {
                cteSqlByName.put(cteKey, cteSql);
            }
            index = skipWhitespace(trimmed, close + 1);
            if (index < trimmed.length() && trimmed.charAt(index) == ',') {
                index = skipWhitespace(trimmed, index + 1);
                continue;
            }
            break;
        }

        String mainQuery = trimmed.substring(Math.min(index, trimmed.length())).trim();
        if (mainQuery.isBlank()) {
            return null;
        }
        return new WithClauseParseResult(mainQuery, cteSqlByName);
    }

    private static int skipWhitespace(String value, int index) {
        int i = Math.max(0, index);
        while (i < value.length() && Character.isWhitespace(value.charAt(i))) {
            i++;
        }
        return i;
    }

    private static IdentifierTokenParseResult parseIdentifierToken(String value, int start) {
        if (value == null || start >= value.length()) {
            return null;
        }
        int index = skipWhitespace(value, start);
        boolean inDouble = false;
        int tokenStart = index;
        while (index < value.length()) {
            char c = value.charAt(index);
            if (c == '"') {
                inDouble = !inDouble;
                index++;
                continue;
            }
            if (!inDouble && (Character.isWhitespace(c) || c == ',' || c == ';' || c == '(')) {
                break;
            }
            index++;
        }
        if (index <= tokenStart) {
            return null;
        }
        return new IdentifierTokenParseResult(value.substring(tokenStart, index), index);
    }

    private static int findMatchingParenthesis(String value, int openIndex) {
        if (value == null || openIndex < 0 || openIndex >= value.length() || value.charAt(openIndex) != '(') {
            return -1;
        }
        boolean inSingle = false;
        boolean inDouble = false;
        int depth = 0;
        for (int i = openIndex; i < value.length(); i++) {
            char c = value.charAt(i);
            if (c == '\'' && !inDouble) {
                inSingle = !inSingle;
                continue;
            }
            if (c == '"' && !inSingle) {
                inDouble = !inDouble;
                continue;
            }
            if (inSingle || inDouble) {
                continue;
            }
            if (c == '(') {
                depth++;
            } else if (c == ')') {
                depth--;
                if (depth == 0) {
                    return i;
                }
            }
        }
        return -1;
    }

    private static String firstToken(String sql) {
        if (sql == null || sql.isBlank()) {
            return null;
        }
        int idx = 0;
        boolean inDouble = false;
        while (idx < sql.length()) {
            char c = sql.charAt(idx);
            if (c == '"') {
                inDouble = !inDouble;
                idx++;
                continue;
            }
            if (!inDouble && (Character.isWhitespace(c) || c == ',' || c == ';')) {
                break;
            }
            idx++;
        }
        String token = sql.substring(0, idx);
        if ("only".equalsIgnoreCase(token)) {
            String remaining = sql.substring(Math.min(sql.length(), idx)).trim();
            return firstToken(remaining);
        }
        return token;
    }

    private static int findTopLevelKeyword(String normalizedSql, String keyword) {
        if (normalizedSql == null || keyword == null || keyword.isEmpty()) {
            return -1;
        }
        boolean inSingle = false;
        boolean inDouble = false;
        int depth = 0;
        for (int i = 0; i <= normalizedSql.length() - keyword.length(); i++) {
            char c = normalizedSql.charAt(i);
            if (c == '\'' && !inDouble) {
                inSingle = !inSingle;
                continue;
            }
            if (c == '"' && !inSingle) {
                inDouble = !inDouble;
                continue;
            }
            if (inSingle || inDouble) {
                continue;
            }
            if (c == '(') {
                depth++;
                continue;
            }
            if (c == ')') {
                depth = Math.max(0, depth - 1);
                continue;
            }
            if (depth == 0 && normalizedSql.startsWith(keyword, i)) {
                return i;
            }
        }
        return -1;
    }

    private static Map<Integer, String> resolveProjectionMapping(String projectionSql, List<SBColumnInfo> columns) {
        String trimmedProjection = projectionSql == null ? "" : projectionSql.trim();
        if (trimmedProjection.isEmpty()) {
            return Collections.emptyMap();
        }

        if (isStarProjection(trimmedProjection)) {
            return mapColumnsByIndex(columns);
        }

        List<String> projectionItems = splitTopLevel(projectionSql, ',');
        if (projectionItems.isEmpty()) {
            return Collections.emptyMap();
        }

        // Mixed star + explicit expressions are ambiguous without catalog metadata.
        for (String item : projectionItems) {
            if (isStarProjection(stripAlias(item))) {
                return null;
            }
        }

        int resultColumnCount = columns == null ? 0 : columns.size();
        int maxIndex = resultColumnCount == 0
            ? projectionItems.size()
            : Math.min(resultColumnCount, projectionItems.size());

        Map<Integer, String> mapping = new LinkedHashMap<>();
        for (int i = 0; i < maxIndex; i++) {
            String mapped = mappedColumnFromProjectionItem(projectionItems.get(i));
            if (mapped != null && !mapped.isBlank()) {
                mapping.put(i + 1, mapped);
            }
        }
        return mapping;
    }

    private static Map<Integer, ProjectionTarget> resolveProjectionTargets(
            String projectionSql, List<SBColumnInfo> columns) {
        String trimmedProjection = projectionSql == null ? "" : projectionSql.trim();
        if (trimmedProjection.isEmpty()) {
            return Collections.emptyMap();
        }

        if (isStarProjection(trimmedProjection)) {
            String starQualifier = starProjectionQualifier(trimmedProjection);
            Map<Integer, ProjectionTarget> mapped = new LinkedHashMap<>();
            if (columns != null) {
                for (int i = 0; i < columns.size(); i++) {
                    SBColumnInfo column = columns.get(i);
                    if (column == null || column.getName() == null || column.getName().isBlank()) {
                        continue;
                    }
                    mapped.put(i + 1, new ProjectionTarget(column.getName(), starQualifier));
                }
            }
            return mapped;
        }

        List<String> projectionItems = splitTopLevel(projectionSql, ',');
        if (projectionItems.isEmpty()) {
            return Collections.emptyMap();
        }

        for (String item : projectionItems) {
            if (isStarProjection(stripAlias(item))) {
                return null;
            }
        }

        int resultColumnCount = columns == null ? 0 : columns.size();
        int maxIndex = resultColumnCount == 0
            ? projectionItems.size()
            : Math.min(resultColumnCount, projectionItems.size());

        Map<Integer, ProjectionTarget> mapped = new LinkedHashMap<>();
        for (int i = 0; i < maxIndex; i++) {
            ProjectionTarget target = projectionTargetFromProjectionItem(projectionItems.get(i));
            if (target == null) {
                continue;
            }
            mapped.put(i + 1, target);
        }
        return mapped;
    }

    private static String starProjectionQualifier(String projection) {
        String trimmed = projection == null ? "" : projection.trim();
        if (!trimmed.endsWith(".*")) {
            return null;
        }
        String prefix = trimmed.substring(0, trimmed.length() - 2).trim();
        if (prefix.isEmpty()) {
            return null;
        }
        List<String> parts = splitIdentifierParts(prefix);
        if (parts.isEmpty()) {
            return null;
        }
        return qualifierFromParts(parts);
    }

    private static String qualifierFromParts(List<String> parts) {
        if (parts == null || parts.isEmpty()) {
            return "";
        }
        StringBuilder normalized = new StringBuilder();
        for (int i = 0; i < parts.size(); i++) {
            if (i > 0) {
                normalized.append('.');
            }
            normalized.append(unquoteIdentifier(parts.get(i)).toLowerCase(Locale.ROOT));
        }
        return normalized.toString();
    }

    private static List<String> qualifierKeysForTableToken(String tableToken) {
        List<String> parts = splitIdentifierParts(tableToken);
        if (parts.isEmpty()) {
            return Collections.emptyList();
        }
        List<String> keys = new ArrayList<>();
        for (int i = 0; i < parts.size(); i++) {
            keys.add(qualifierFromParts(parts.subList(i, parts.size())));
        }
        return keys;
    }

    private static Map<Integer, String> mapColumnsByIndex(List<SBColumnInfo> columns) {
        if (columns == null || columns.isEmpty()) {
            return Collections.emptyMap();
        }
        Map<Integer, String> mapping = new LinkedHashMap<>();
        for (int i = 0; i < columns.size(); i++) {
            SBColumnInfo column = columns.get(i);
            if (column == null || column.getName() == null || column.getName().isBlank()) {
                continue;
            }
            mapping.put(i + 1, column.getName());
        }
        return mapping;
    }

    private static String mappedColumnFromProjectionItem(String item) {
        String core = stripAlias(item).trim();
        if (core.isEmpty() || isStarProjection(core) || !isSimpleColumnReference(core)) {
            return null;
        }
        List<String> parts = splitIdentifierParts(core);
        if (parts.isEmpty()) {
            return null;
        }
        return unquoteIdentifier(parts.get(parts.size() - 1));
    }

    private static ProjectionTarget projectionTargetFromProjectionItem(String item) {
        String core = stripAlias(item).trim();
        if (core.isEmpty() || isStarProjection(core) || !isSimpleColumnReference(core)) {
            return null;
        }
        List<String> parts = splitIdentifierParts(core);
        if (parts.isEmpty()) {
            return null;
        }
        String columnName = unquoteIdentifier(parts.get(parts.size() - 1));
        String qualifier = null;
        if (parts.size() > 1) {
            qualifier = qualifierFromParts(parts.subList(0, parts.size() - 1));
        }
        return new ProjectionTarget(columnName, qualifier);
    }

    private static Map<String, String> parseFromClauseTableMapping(String afterFromOriginal) {
        String tableClause = extractTopLevelTableClause(afterFromOriginal);
        if (tableClause == null || tableClause.isBlank()) {
            return Collections.emptyMap();
        }
        List<String> references = splitTableReferences(tableClause);
        if (references.isEmpty()) {
            return Collections.emptyMap();
        }

        Map<String, String> mapped = new LinkedHashMap<>();
        for (String reference : references) {
            TableReference parsed = parseTableReference(reference);
            if (parsed == null || parsed.tableSql == null || parsed.tableSql.isBlank()) {
                continue;
            }
            if (parsed.alias != null && !parsed.alias.isBlank()) {
                mapped.put(normalizeIdentifierKey(parsed.alias), parsed.tableSql);
            }
            for (String key : qualifierKeysForTableToken(parsed.tableSql)) {
                mapped.putIfAbsent(key, parsed.tableSql);
            }
            if (parsed.tableName != null && !parsed.tableName.isBlank()) {
                mapped.putIfAbsent(normalizeIdentifierKey(parsed.tableName), parsed.tableSql);
            }
        }
        return mapped;
    }

    private static String resolvePrimaryTableSql(String afterFromOriginal, String fallback) {
        String tableClause = extractTopLevelTableClause(afterFromOriginal);
        if (tableClause == null || tableClause.isBlank()) {
            return fallback;
        }
        List<String> references = splitTableReferences(tableClause);
        if (references.isEmpty()) {
            return fallback;
        }
        for (String reference : references) {
            TableReference parsed = parseTableReference(reference);
            if (parsed != null && parsed.tableSql != null && !parsed.tableSql.isBlank()) {
                return parsed.tableSql;
            }
        }
        return fallback;
    }

    private static String resolveTableSqlForQualifier(Map<String, String> tableSqlByQualifier,
                                                      String qualifier,
                                                      String defaultTableSql) {
        if (qualifier == null || qualifier.isBlank()) {
            return defaultTableSql;
        }
        if (tableSqlByQualifier == null || tableSqlByQualifier.isEmpty()) {
            return defaultTableSql;
        }
        String mapped = tableSqlByQualifier.get(normalizeIdentifierKey(qualifier));
        return mapped != null && !mapped.isBlank() ? mapped : defaultTableSql;
    }

    private static String normalizeIdentifierKey(String identifier) {
        if (identifier == null) {
            return "";
        }
        List<String> parts = splitIdentifierParts(identifier);
        if (parts.isEmpty()) {
            return unquoteIdentifier(identifier).toLowerCase(Locale.ROOT);
        }
        return qualifierFromParts(parts);
    }

    private static String extractTopLevelTableClause(String afterFromOriginal) {
        if (afterFromOriginal == null || afterFromOriginal.isBlank()) {
            return null;
        }
        String collapsed = collapseWhitespace(afterFromOriginal);
        String normalized = collapsed.toLowerCase(Locale.ROOT);
        int end = normalized.length();
        String[] boundaryKeywords = {
            " where ",
            " group by ",
            " having ",
            " order by ",
            " limit ",
            " offset ",
            " fetch ",
            " for ",
            " union ",
            " intersect ",
            " except "
        };
        for (String keyword : boundaryKeywords) {
            int index = findTopLevelKeyword(normalized, keyword);
            if (index >= 0 && index < end) {
                end = index;
            }
        }
        return collapsed.substring(0, end).trim();
    }

    private static List<String> splitTableReferences(String tableClause) {
        if (tableClause == null || tableClause.isBlank()) {
            return Collections.emptyList();
        }
        List<String> references = new ArrayList<>();
        StringBuilder current = new StringBuilder();
        boolean inSingle = false;
        boolean inDouble = false;
        int depth = 0;
        for (int i = 0; i < tableClause.length(); i++) {
            char c = tableClause.charAt(i);
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
                if (c == '(') {
                    depth++;
                    current.append(c);
                    continue;
                }
                if (c == ')') {
                    depth = Math.max(0, depth - 1);
                    current.append(c);
                    continue;
                }
                if (depth == 0 && c == ',') {
                    String segment = current.toString().trim();
                    if (!segment.isEmpty()) {
                        references.add(stripJoinPredicate(segment));
                    }
                    current.setLength(0);
                    continue;
                }
                if (depth == 0 && startsWithWordAt(tableClause, i, "join")) {
                    String segment = current.toString().trim();
                    if (!segment.isEmpty()) {
                        references.add(stripJoinPredicate(segment));
                    }
                    current.setLength(0);
                    i += "join".length() - 1;
                    continue;
                }
            }
            current.append(c);
        }
        String trailing = current.toString().trim();
        if (!trailing.isEmpty()) {
            references.add(stripJoinPredicate(trailing));
        }
        return references;
    }

    private static boolean startsWithWordAt(String text, int index, String word) {
        if (text == null || word == null || index < 0) {
            return false;
        }
        int end = index + word.length();
        if (end > text.length()) {
            return false;
        }
        if (!text.regionMatches(true, index, word, 0, word.length())) {
            return false;
        }
        char before = index > 0 ? text.charAt(index - 1) : ' ';
        char after = end < text.length() ? text.charAt(end) : ' ';
        boolean beforeBoundary = Character.isWhitespace(before) || before == '(' || before == ')';
        boolean afterBoundary = Character.isWhitespace(after) || after == '(' || after == ')';
        return beforeBoundary && afterBoundary;
    }

    private static String stripJoinPredicate(String tableReference) {
        String collapsed = collapseWhitespace(tableReference);
        String normalized = collapsed.toLowerCase(Locale.ROOT);
        int onIndex = findTopLevelKeyword(normalized, " on ");
        if (onIndex >= 0) {
            return collapsed.substring(0, onIndex).trim();
        }
        int usingIndex = findTopLevelKeyword(normalized, " using ");
        if (usingIndex >= 0) {
            return collapsed.substring(0, usingIndex).trim();
        }
        return collapsed.trim();
    }

    private static final class TableReference {
        private final String tableSql;
        private final String tableName;
        private final String alias;

        private TableReference(String tableSql, String tableName, String alias) {
            this.tableSql = tableSql;
            this.tableName = tableName;
            this.alias = alias;
        }
    }

    private static TableReference parseTableReference(String referenceSql) {
        if (referenceSql == null || referenceSql.isBlank()) {
            return null;
        }
        String trimmed = referenceSql.trim();
        if (trimmed.startsWith("(")) {
            return null;
        }
        String tableToken = firstToken(trimmed);
        if (tableToken == null || tableToken.isBlank()) {
            return null;
        }

        String remaining = trimmed.substring(Math.min(trimmed.length(), tableToken.length())).trim();
        String alias = null;
        if (!remaining.isEmpty()) {
            String remLower = remaining.toLowerCase(Locale.ROOT);
            if (remLower.startsWith("as ")) {
                String candidate = firstToken(remaining.substring(3).trim());
                if (isUsableAliasToken(candidate)) {
                    alias = candidate;
                }
            } else {
                String candidate = firstToken(remaining);
                if (isUsableAliasToken(candidate)) {
                    alias = candidate;
                }
            }
        }

        List<String> tableParts = splitIdentifierParts(tableToken);
        String tableName;
        if (tableParts.isEmpty()) {
            tableName = unquoteIdentifier(tableToken);
        } else {
            tableName = unquoteIdentifier(tableParts.get(tableParts.size() - 1));
        }
        return new TableReference(tableToken, tableName, alias);
    }

    private static boolean isUsableAliasToken(String candidate) {
        if (candidate == null || candidate.isBlank()) {
            return false;
        }
        String trimmed = candidate.trim();
        if (trimmed.startsWith("\"") && trimmed.endsWith("\"") && trimmed.length() >= 2) {
            return true;
        }
        return !TABLE_REFERENCE_NON_ALIAS_TOKENS.contains(trimmed.toLowerCase(Locale.ROOT));
    }

    private static String stripAlias(String item) {
        String trimmed = item == null ? "" : item.trim();
        if (trimmed.isEmpty()) {
            return "";
        }

        String normalized = trimmed.toLowerCase(Locale.ROOT);
        int asIndex = findTopLevelKeyword(normalized, " as ");
        if (asIndex >= 0) {
            return trimmed.substring(0, asIndex).trim();
        }

        int split = findTopLevelTrailingTokenBoundary(trimmed);
        if (split <= 0 || split >= trimmed.length() - 1) {
            return trimmed;
        }
        String left = trimmed.substring(0, split).trim();
        String right = trimmed.substring(split).trim();
        if (isSimpleIdentifierToken(right)) {
            return left;
        }
        return trimmed;
    }

    private static int findTopLevelTrailingTokenBoundary(String value) {
        boolean inSingle = false;
        boolean inDouble = false;
        int depth = 0;
        for (int i = value.length() - 1; i >= 0; i--) {
            char c = value.charAt(i);
            if (c == '\'' && !inDouble) {
                inSingle = !inSingle;
                continue;
            }
            if (c == '"' && !inSingle) {
                inDouble = !inDouble;
                continue;
            }
            if (inSingle || inDouble) {
                continue;
            }
            if (c == ')') {
                depth++;
                continue;
            }
            if (c == '(') {
                depth = Math.max(0, depth - 1);
                continue;
            }
            if (depth == 0 && Character.isWhitespace(c)) {
                int j = i;
                while (j >= 0 && Character.isWhitespace(value.charAt(j))) {
                    j--;
                }
                return j + 1;
            }
        }
        return -1;
    }

    private static boolean isStarProjection(String projectionItem) {
        String trimmed = projectionItem == null ? "" : projectionItem.trim();
        if (trimmed.isEmpty()) {
            return false;
        }
        if ("*".equals(trimmed)) {
            return true;
        }
        if (trimmed.endsWith(".*")) {
            String prefix = trimmed.substring(0, trimmed.length() - 2).trim();
            return isSimpleColumnReference(prefix);
        }
        return false;
    }

    private static boolean isSimpleColumnReference(String token) {
        return !splitIdentifierParts(token).isEmpty();
    }

    private static List<String> splitIdentifierParts(String token) {
        String trimmed = token == null ? "" : token.trim();
        if (trimmed.isEmpty()) {
            return Collections.emptyList();
        }

        List<String> parts = new ArrayList<>();
        StringBuilder current = new StringBuilder();
        boolean inDouble = false;
        for (int i = 0; i < trimmed.length(); i++) {
            char c = trimmed.charAt(i);
            if (inDouble) {
                current.append(c);
                if (c == '"') {
                    if (i + 1 < trimmed.length() && trimmed.charAt(i + 1) == '"') {
                        current.append('"');
                        i++;
                    } else {
                        inDouble = false;
                    }
                }
                continue;
            }
            if (c == '"') {
                inDouble = true;
                current.append(c);
                continue;
            }
            if (c == '.') {
                String segment = current.toString().trim();
                if (!isSimpleIdentifierToken(segment)) {
                    return Collections.emptyList();
                }
                parts.add(segment);
                current.setLength(0);
                continue;
            }
            if (Character.isWhitespace(c)) {
                return Collections.emptyList();
            }
            if ("(),+-/*%<>=!|&[]{}:?".indexOf(c) >= 0) {
                return Collections.emptyList();
            }
            current.append(c);
        }
        String segment = current.toString().trim();
        if (!isSimpleIdentifierToken(segment)) {
            return Collections.emptyList();
        }
        parts.add(segment);
        return parts;
    }

    private static boolean isSimpleIdentifierToken(String token) {
        String trimmed = token == null ? "" : token.trim();
        if (trimmed.isEmpty()) {
            return false;
        }
        if (trimmed.startsWith("\"") && trimmed.endsWith("\"") && trimmed.length() >= 2) {
            return true;
        }
        for (int i = 0; i < trimmed.length(); i++) {
            char c = trimmed.charAt(i);
            if (!(Character.isLetterOrDigit(c) || c == '_' || c == '$')) {
                return false;
            }
        }
        return true;
    }

    private static String unquoteIdentifier(String identifier) {
        if (identifier == null) {
            return null;
        }
        String trimmed = identifier.trim();
        if (trimmed.startsWith("\"") && trimmed.endsWith("\"") && trimmed.length() >= 2) {
            trimmed = trimmed.substring(1, trimmed.length() - 1).replace("\"\"", "\"");
        }
        return trimmed;
    }

    private static List<String> splitTopLevel(String value, char delimiter) {
        List<String> items = new ArrayList<>();
        if (value == null || value.isBlank()) {
            return items;
        }
        StringBuilder current = new StringBuilder();
        boolean inSingle = false;
        boolean inDouble = false;
        int depth = 0;
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
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
                if (c == '(') {
                    depth++;
                } else if (c == ')') {
                    depth = Math.max(0, depth - 1);
                } else if (c == delimiter && depth == 0) {
                    items.add(current.toString().trim());
                    current.setLength(0);
                    continue;
                }
            }
            current.append(c);
        }
        String trailing = current.toString().trim();
        if (!trailing.isEmpty()) {
            items.add(trailing);
        }
        return items;
    }

    private void clearRowActionFlags() {
        rowUpdatedFlag = false;
        rowInsertedFlag = false;
        rowDeletedFlag = false;
    }

    private void ensureNotOnInsertRow() throws SQLException {
        if (onInsertRow) {
            throw new SQLException("Cursor is on insert row", "HY109");
        }
    }

    private boolean isScrollableCursor() {
        return resultSetType != ResultSet.TYPE_FORWARD_ONLY;
    }

    void assignCursorName(String cursorName) {
        this.cursorName = cursorName;
    }

    String assignedCursorName() {
        return cursorName;
    }

    Object[] firstBufferedRowSnapshot() {
        if (rows == null || rows.isEmpty()) {
            return null;
        }
        Object[] first = rows.get(0);
        return first == null ? null : first.clone();
    }

    private void ensureUpdatable() throws SQLException {
        if (!updatable) {
            throw new SQLFeatureNotSupportedException("Result set is read-only");
        }
    }

    String positionedWhereClause() throws SQLException {
        return positionedWhereClauseForTable(null);
    }

    String positionedWhereClauseForTable(String targetTableSql) throws SQLException {
        ensurePositionedCursorReady();
        Object[] keyRow = originalRowSnapshot != null ? originalRowSnapshot : currentRowData;
        try {
            if (targetTableSql == null || targetTableSql.isBlank()) {
                return buildWhereClause(keyRow);
            }
            return buildWhereClause(keyRow, targetTableSql);
        } catch (SQLException ex) {
            if ("HY000".equals(ex.getSQLState()) || "42000".equals(ex.getSQLState())) {
                throw new SQLException(
                    "Cursor row cannot be mapped to target table for positioned mutation: " + targetTableSql,
                    "34000",
                    ex
                );
            }
            throw ex;
        }
    }

    private void ensurePositionedCursorReady() throws SQLException {
        checkClosed();
        ensureNotOnInsertRow();
        if (updateTarget == null) {
            throw new SQLException("Cursor is not positioned on an updatable base table", "34000");
        }
        checkRow();
    }

    private boolean canMutateColumn(int columnIndex) {
        if (!updatable) {
            return false;
        }
        if (updateTarget == null) {
            return false;
        }
        return updateTarget.isColumnWritable(columnIndex);
    }

    private String targetColumnNameOrNull(int columnIndex) {
        if (updateTarget == null) {
            return columnName(columnIndex);
        }
        if (!updateTarget.usesExplicitColumnMapping()) {
            return columnName(columnIndex);
        }
        return updateTarget.mappedColumnName(columnIndex);
    }

    private String targetTableSqlOrNull(int columnIndex) {
        if (updateTarget == null) {
            return null;
        }
        if (!updateTarget.usesExplicitColumnMapping()) {
            return updateTarget.tableSql;
        }
        String columnName = updateTarget.mappedColumnName(columnIndex);
        if (columnName == null || columnName.isBlank()) {
            return null;
        }
        return updateTarget.mappedTableSql(columnIndex);
    }

    private String resolveTargetTableForMutationIndices(Collection<Integer> columnIndices)
            throws SQLException {
        if (updateTarget == null) {
            return null;
        }
        String targetTableSql = null;
        if (columnIndices != null) {
            for (Integer columnIndex : columnIndices) {
                if (columnIndex == null) {
                    continue;
                }
                String mappedTableSql = targetTableSqlOrNull(columnIndex);
                if (mappedTableSql == null || mappedTableSql.isBlank()) {
                    continue;
                }
                if (targetTableSql == null) {
                    targetTableSql = mappedTableSql;
                    continue;
                }
                if (!tableSqlEquivalent(targetTableSql, mappedTableSql)) {
                    // Favor the resolved primary table and ignore columns mapped to other targets.
                    continue;
                }
            }
        }
        if (targetTableSql == null || targetTableSql.isBlank()) {
            targetTableSql = updateTarget.tableSql;
        }
        return targetTableSql;
    }

    private Map<String, Map<Integer, Object>> partitionUpdatesByTargetTable(Map<Integer, Object> updates)
            throws SQLException {
        if (updateTarget == null) {
            return Collections.emptyMap();
        }
        Map<String, Map<Integer, Object>> grouped = new LinkedHashMap<>();
        for (Map.Entry<Integer, Object> entry : updates.entrySet()) {
            Integer columnIndex = entry.getKey();
            if (updateTarget.usesExplicitColumnMapping()) {
                String mappedColumn = updateTarget.mappedColumnName(columnIndex);
                if (mappedColumn == null || mappedColumn.isBlank()) {
                    continue;
                }
            }
            String targetTableSql = targetTableSqlOrNull(columnIndex);
            if (targetTableSql == null || targetTableSql.isBlank()) {
                targetTableSql = updateTarget.tableSql;
            }
            grouped.computeIfAbsent(targetTableSql, ignored -> new LinkedHashMap<>())
                .put(columnIndex, entry.getValue());
        }
        return grouped;
    }

    private void updateColumn(int columnIndex, Object value) throws SQLException {
        checkClosed();
        ensureUpdatable();
        checkColumnIndex(columnIndex);
        if (!canMutateColumn(columnIndex)) {
            throw new SQLFeatureNotSupportedException(
                "Column is not writable in this updatable ResultSet projection: " + columnName(columnIndex)
            );
        }
        clearRowActionFlags();
        if (onInsertRow) {
            if (insertRowBuffer == null) {
                insertRowBuffer = new Object[columns.size()];
            }
            insertRowBuffer[columnIndex - 1] = value;
            return;
        }
        checkRow();
        if (originalRowSnapshot == null) {
            originalRowSnapshot = currentRowData.clone();
        }
        currentRowData[columnIndex - 1] = value;
        pendingUpdates.put(columnIndex, value);
    }

    private void executeInsert(Map<Integer, Object> values) throws SQLException {
        if (updateTarget == null) {
            throw new SQLException("ResultSet mutation target is unresolved", "0A000");
        }
        if (!updateTarget.usesExplicitColumnMapping()) {
            String targetTableSql = resolveTargetTableForMutationIndices(values.keySet());
            executeInsertForTargetTable(targetTableSql, values);
            return;
        }
        Map<String, Map<Integer, Object>> grouped = partitionUpdatesByTargetTable(values);
        if (grouped.isEmpty()) {
            return;
        }
        for (Map.Entry<String, Map<Integer, Object>> entry : grouped.entrySet()) {
            executeInsertForTargetTable(entry.getKey(), entry.getValue());
        }
    }

    private void executeInsertForTargetTable(String targetTableSql, Map<Integer, Object> values)
            throws SQLException {
        StringBuilder columnSql = new StringBuilder();
        StringBuilder valueSql = new StringBuilder();
        boolean first = true;
        for (Map.Entry<Integer, Object> entry : values.entrySet()) {
            String mappedTableSql = targetTableSqlOrNull(entry.getKey());
            if (mappedTableSql != null && !tableSqlEquivalent(mappedTableSql, targetTableSql)) {
                continue;
            }
            String mappedColumnName = targetColumnNameOrNull(entry.getKey());
            if (mappedColumnName == null || mappedColumnName.isBlank()) {
                continue;
            }
            if (!first) {
                columnSql.append(", ");
                valueSql.append(", ");
            }
            first = false;
            columnSql.append(quoteIdentifier(mappedColumnName));
            valueSql.append(toSqlLiteral(entry.getValue()));
        }
        if (first) {
            return;
        }
        String sql = "INSERT INTO " + targetTableSql
            + " (" + columnSql + ") VALUES (" + valueSql + ")";
        executeMutation(sql);
    }

    private void executeUpdate(Object[] beforeRow, Map<Integer, Object> updates) throws SQLException {
        if (updateTarget == null) {
            throw new SQLException("ResultSet mutation target is unresolved", "0A000");
        }
        Map<String, Map<Integer, Object>> groupedUpdates = partitionUpdatesByTargetTable(updates);
        if (groupedUpdates.isEmpty()) {
            return;
        }
        for (Map.Entry<String, Map<Integer, Object>> groupedEntry : groupedUpdates.entrySet()) {
            String targetTableSql = groupedEntry.getKey();
            Map<Integer, Object> tableUpdates = groupedEntry.getValue();
            StringBuilder setSql = new StringBuilder();
            boolean first = true;
            for (Map.Entry<Integer, Object> entry : tableUpdates.entrySet()) {
                String mappedTableSql = targetTableSqlOrNull(entry.getKey());
                if (mappedTableSql != null && !tableSqlEquivalent(mappedTableSql, targetTableSql)) {
                    continue;
                }
                String targetColumn = targetColumnNameOrNull(entry.getKey());
                if (targetColumn == null || targetColumn.isBlank()) {
                    continue;
                }
                if (!first) {
                    setSql.append(", ");
                }
                first = false;
                setSql.append(quoteIdentifier(targetColumn))
                .append(" = ")
                .append(toSqlLiteral(entry.getValue()));
        }
        if (first) {
            continue;
        }
            String sql = "UPDATE " + targetTableSql
                + " SET " + setSql
                + " WHERE " + buildWhereClause(beforeRow, targetTableSql);
            executeMutation(sql);
        }
    }

    private void executeDelete(Object[] beforeRow) throws SQLException {
        if (updateTarget == null) {
            throw new SQLException("ResultSet mutation target is unresolved", "0A000");
        }
        if (!updateTarget.usesExplicitColumnMapping()) {
            String sql = "DELETE FROM " + updateTarget.tableSql
                + " WHERE " + buildWhereClause(beforeRow);
            executeMutation(sql);
            return;
        }
        Set<String> targetTables = mappedTargetTablesForRowMutation();
        if (targetTables.isEmpty()) {
            String sql = "DELETE FROM " + updateTarget.tableSql
                + " WHERE " + buildWhereClause(beforeRow);
            executeMutation(sql);
            return;
        }
        for (String targetTable : targetTables) {
            String sql = "DELETE FROM " + targetTable
                + " WHERE " + buildWhereClause(beforeRow, targetTable);
            executeMutation(sql);
        }
    }

    private Set<String> mappedTargetTablesForRowMutation() {
        if (updateTarget == null || !updateTarget.usesExplicitColumnMapping()) {
            return Collections.emptySet();
        }
        Set<String> tables = new LinkedHashSet<>();
        for (Integer columnIndex : updateTarget.columnNamesByIndex.keySet()) {
            String mappedTable = targetTableSqlOrNull(columnIndex);
            if (mappedTable == null || mappedTable.isBlank()) {
                continue;
            }
            tables.add(mappedTable);
        }
        return tables;
    }

    private Object[] executeRefresh(Object[] beforeRow) throws SQLException {
        if (updateTarget == null) {
            throw new SQLException("ResultSet mutation target is unresolved", "0A000");
        }
        Object[] base = currentRowData == null ? null : currentRowData.clone();
        if (base == null || base.length == 0) {
            return base;
        }

        if (!updateTarget.usesExplicitColumnMapping()) {
            return refreshColumnsForTable(beforeRow, base, updateTarget.tableSql, allColumnIndices());
        }

        Map<String, List<Integer>> tableColumns = new LinkedHashMap<>();
        for (int i = 1; i <= columns.size(); i++) {
            String mapped = targetColumnNameOrNull(i);
            if (mapped == null || mapped.isBlank()) {
                continue;
            }
            String mappedTable = targetTableSqlOrNull(i);
            if (mappedTable == null || mappedTable.isBlank()) {
                mappedTable = updateTarget.tableSql;
            }
            tableColumns.computeIfAbsent(mappedTable, ignored -> new ArrayList<>()).add(i);
        }
        if (tableColumns.isEmpty()) {
            return base;
        }

        for (Map.Entry<String, List<Integer>> entry : tableColumns.entrySet()) {
            Object[] refreshed = refreshColumnsForTable(beforeRow, base, entry.getKey(), entry.getValue());
            if (refreshed == null) {
                return null;
            }
            base = refreshed;
        }
        return base;
    }

    private List<Integer> allColumnIndices() {
        List<Integer> indices = new ArrayList<>(columns.size());
        for (int i = 1; i <= columns.size(); i++) {
            indices.add(i);
        }
        return indices;
    }

    private Object[] refreshColumnsForTable(Object[] beforeRow, Object[] baseline, String tableSql,
                                            List<Integer> columnIndices) throws SQLException {
        if (columnIndices == null || columnIndices.isEmpty()) {
            return baseline;
        }
        StringBuilder selectCols = new StringBuilder();
        List<Integer> projectedIndices = new ArrayList<>(columnIndices.size());
        for (Integer columnIndex : columnIndices) {
            if (columnIndex == null || columnIndex < 1 || columnIndex > columns.size()) {
                continue;
            }
            String mappedName = targetColumnNameOrNull(columnIndex);
            if (mappedName == null || mappedName.isBlank()) {
                mappedName = columnName(columnIndex);
            }
            if (selectCols.length() > 0) {
                selectCols.append(", ");
            }
            selectCols.append(quoteIdentifier(mappedName))
                .append(" AS ")
                .append(quoteIdentifier(columnName(columnIndex)));
            projectedIndices.add(columnIndex);
        }
        if (projectedIndices.isEmpty()) {
            return baseline;
        }

        String effectiveTableSql = tableSql == null || tableSql.isBlank()
            ? updateTarget.tableSql
            : tableSql;
        String sql = "SELECT " + selectCols
            + " FROM " + effectiveTableSql
            + " WHERE " + buildWhereClause(beforeRow, effectiveTableSql)
            + " LIMIT 1";
        SBQueryResult result = executeQuery(sql);
        if (result.getRows() == null || result.getRows().isEmpty()) {
            return null;
        }
        Object[] loaded = result.getRows().get(0);
        Object[] merged = baseline.clone();
        for (int i = 0; i < projectedIndices.size() && i < loaded.length; i++) {
            merged[projectedIndices.get(i) - 1] = loaded[i];
        }
        return merged;
    }

    private SBQueryResult executeMutation(String sql) throws SQLException {
        SBQueryResult result = executeQuery(sql);
        if (result.getUpdateCount() == 0) {
            throw new SQLException("No rows affected by updatable ResultSet operation", "02000");
        }
        return result;
    }

    private SBQueryResult executeQuery(String sql) throws SQLException {
        if (statement == null || statement.connection == null) {
            throw new SQLException("ResultSet is not associated with a live statement", "HY010");
        }
        return statement.connection.withResilience("resultset_mutation", sql,
            () -> statement.connection.getProtocol().execute(sql, 0, 0));
    }

    private String buildWhereClause(Object[] row) throws SQLException {
        return buildWhereClause(row, updateTarget != null ? updateTarget.tableSql : null);
    }

    private String buildWhereClause(Object[] row, String targetTableSql) throws SQLException {
        List<String> mappedPredicates = new ArrayList<>(columns.size());
        List<String> fallbackPredicates = new ArrayList<>(columns.size());
        for (int i = 0; i < columns.size(); i++) {
            Object value = i < row.length ? row[i] : null;
            String fallbackName = quoteIdentifier(columnName(i + 1));
            if (value == null) {
                fallbackPredicates.add(fallbackName + " IS NULL");
            } else {
                fallbackPredicates.add(fallbackName + " = " + toSqlLiteral(value));
            }

            String mappedName = targetColumnNameOrNull(i + 1);
            if (mappedName == null || mappedName.isBlank()) {
                continue;
            }
            String mappedTableSql = targetTableSqlOrNull(i + 1);
            if (targetTableSql != null && mappedTableSql != null
                && !tableSqlEquivalent(mappedTableSql, targetTableSql)) {
                continue;
            }
            String mapped = quoteIdentifier(mappedName);
            if (value == null) {
                mappedPredicates.add(mapped + " IS NULL");
            } else {
                mappedPredicates.add(mapped + " = " + toSqlLiteral(value));
            }
        }
        if (!mappedPredicates.isEmpty()) {
            return String.join(" AND ", mappedPredicates);
        }
        if (updateTarget != null && updateTarget.usesExplicitColumnMapping()) {
            throw new SQLException("Cannot build row identity predicate for target table", "HY000");
        }
        if (fallbackPredicates.isEmpty()) {
            throw new SQLException("Cannot build row identity predicate", "HY000");
        }
        return String.join(" AND ", fallbackPredicates);
    }

    private static boolean tableSqlEquivalent(String left, String right) {
        if (left == null || right == null) {
            return Objects.equals(left, right);
        }
        String normalizedLeft = normalizeTableSql(left);
        String normalizedRight = normalizeTableSql(right);
        if (normalizedLeft.equals(normalizedRight)) {
            return true;
        }
        if (!normalizedLeft.contains(".") && normalizedRight.endsWith("." + normalizedLeft)) {
            return true;
        }
        if (!normalizedRight.contains(".") && normalizedLeft.endsWith("." + normalizedRight)) {
            return true;
        }
        return false;
    }

    private static String normalizeTableSql(String tableSql) {
        return tableSql.replace("\"", "").trim().toLowerCase(Locale.ROOT);
    }

    private String columnName(int columnIndex) {
        return columns.get(columnIndex - 1).getName();
    }

    private static String quoteIdentifier(String identifier) {
        if (identifier == null || identifier.isBlank()) {
            return "\"\"";
        }
        if (identifier.indexOf('.') >= 0) {
            String[] parts = identifier.split("\\.");
            StringBuilder out = new StringBuilder();
            for (int i = 0; i < parts.length; i++) {
                if (i > 0) {
                    out.append('.');
                }
                out.append(quoteIdentifier(parts[i]));
            }
            return out.toString();
        }
        String unquoted = identifier;
        if (unquoted.startsWith("\"") && unquoted.endsWith("\"") && unquoted.length() >= 2) {
            return unquoted;
        }
        return "\"" + unquoted.replace("\"", "\"\"") + "\"";
    }

    private static String toSqlLiteral(Object value) throws SQLException {
        if (value == null) {
            return "NULL";
        }
        if (value instanceof Ref) {
            return toSqlLiteral(((Ref) value).getObject());
        }
        if (value instanceof RowId) {
            return "'" + new String(((RowId) value).getBytes(), StandardCharsets.UTF_8)
                .replace("'", "''") + "'";
        }
        if (value instanceof Boolean) {
            return (Boolean) value ? "TRUE" : "FALSE";
        }
        if (value instanceof Number) {
            return value.toString();
        }
        if (value instanceof byte[]) {
            byte[] bytes = (byte[]) value;
            StringBuilder sb = new StringBuilder("E'\\\\x");
            for (byte b : bytes) {
                sb.append(String.format("%02x", b & 0xff));
            }
            sb.append("'");
            return sb.toString();
        }
        if (value instanceof java.sql.Date) {
            return castTemporalLiteral(value.toString(), "DATE");
        }
        if (value instanceof java.sql.Time) {
            return castTemporalLiteral(value.toString(), "TIME");
        }
        if (value instanceof java.sql.Timestamp) {
            return castTemporalLiteral(value.toString(), "TIMESTAMP");
        }
        if (value instanceof LocalDate) {
            return castTemporalLiteral(value.toString(), "DATE");
        }
        if (value instanceof LocalTime) {
            return castTemporalLiteral(value.toString(), "TIME");
        }
        if (value instanceof LocalDateTime) {
            return castTemporalLiteral(value.toString().replace('T', ' '), "TIMESTAMP");
        }
        if (value instanceof OffsetTime) {
            return "TIMETZ '" + value + "'";
        }
        if (value instanceof OffsetDateTime) {
            return "TIMESTAMPTZ '" + value + "'";
        }
        if (value instanceof ZonedDateTime) {
            return "TIMESTAMPTZ '" + ((ZonedDateTime) value).toOffsetDateTime() + "'";
        }
        if (value instanceof Instant) {
            return "TIMESTAMPTZ '" + OffsetDateTime.ofInstant((Instant) value, ZoneOffset.UTC) + "'";
        }
        if (value instanceof Array) {
            Object arrayValue = ((Array) value).getArray();
            if (arrayValue instanceof Object[]) {
                return toSqlArrayLiteral((Object[]) arrayValue);
            }
            return "'" + arrayValue.toString().replace("'", "''") + "'";
        }
        if (value instanceof Object[]) {
            return toSqlArrayLiteral((Object[]) value);
        }
        if (value instanceof Collection<?>) {
            return toSqlArrayLiteral(((Collection<?>) value).toArray());
        }
        return "'" + value.toString().replace("'", "''") + "'";
    }

    private static String castTemporalLiteral(String value, String targetType) {
        return "CAST('" + value.replace("'", "''") + "' AS " + targetType + ")";
    }

    private static String toSqlArrayLiteral(Object[] elements) throws SQLException {
        StringBuilder sb = new StringBuilder("ARRAY[");
        for (int i = 0; i < elements.length; i++) {
            if (i > 0) {
                sb.append(", ");
            }
            sb.append(toSqlLiteral(elements[i]));
        }
        sb.append("]");
        return sb.toString();
    }

    private static String readCharacterStreamValue(Reader reader, int length) throws SQLException {
        if (reader == null) {
            return null;
        }
        if (length < 0) {
            throw new SQLException("Invalid stream length", "HY024");
        }
        try {
            char[] chars = new char[length];
            int totalRead = 0;
            while (totalRead < length) {
                int read = reader.read(chars, totalRead, length - totalRead);
                if (read < 0) {
                    break;
                }
                totalRead += read;
            }
            return new String(chars, 0, totalRead);
        } catch (IOException e) {
            throw new SQLException("Failed to read character stream", "HY000", e);
        }
    }

    private static String readCharacterStreamValue(Reader reader, long length) throws SQLException {
        if (length < 0 || length > Integer.MAX_VALUE) {
            throw new SQLException("Invalid stream length", "HY024");
        }
        return readCharacterStreamValue(reader, (int) length);
    }

    private static String readCharacterStreamValue(Reader reader) throws SQLException {
        if (reader == null) {
            return null;
        }
        try {
            StringBuilder sb = new StringBuilder();
            char[] buffer = new char[8192];
            int read;
            while ((read = reader.read(buffer)) >= 0) {
                sb.append(buffer, 0, read);
            }
            return sb.toString();
        } catch (IOException e) {
            throw new SQLException("Failed to read character stream", "HY000", e);
        }
    }

    private static String readAsciiStreamValue(InputStream input, int length) throws SQLException {
        byte[] bytes = readBinaryStreamValue(input, length);
        return bytes == null ? null : new String(bytes, StandardCharsets.US_ASCII);
    }

    private static String readAsciiStreamValue(InputStream input, long length) throws SQLException {
        byte[] bytes = readBinaryStreamValue(input, length);
        return bytes == null ? null : new String(bytes, StandardCharsets.US_ASCII);
    }

    private static String readAsciiStreamValue(InputStream input) throws SQLException {
        byte[] bytes = readBinaryStreamValue(input);
        return bytes == null ? null : new String(bytes, StandardCharsets.US_ASCII);
    }

    private static byte[] readBinaryStreamValue(InputStream input, int length) throws SQLException {
        if (input == null) {
            return null;
        }
        if (length < 0) {
            throw new SQLException("Invalid stream length", "HY024");
        }
        try {
            ByteArrayOutputStream out = new ByteArrayOutputStream(Math.max(length, 0));
            byte[] buffer = new byte[Math.max(1, Math.min(length, 8192))];
            int remaining = length;
            while (remaining > 0) {
                int read = input.read(buffer, 0, Math.min(buffer.length, remaining));
                if (read < 0) {
                    break;
                }
                out.write(buffer, 0, read);
                remaining -= read;
            }
            return out.toByteArray();
        } catch (IOException e) {
            throw new SQLException("Failed to read binary stream", "HY000", e);
        }
    }

    private static byte[] readBinaryStreamValue(InputStream input, long length) throws SQLException {
        if (length < 0 || length > Integer.MAX_VALUE) {
            throw new SQLException("Invalid stream length", "HY024");
        }
        return readBinaryStreamValue(input, (int) length);
    }

    private static byte[] readBinaryStreamValue(InputStream input) throws SQLException {
        if (input == null) {
            return null;
        }
        try {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[8192];
            int read;
            while ((read = input.read(buffer)) >= 0) {
                out.write(buffer, 0, read);
            }
            return out.toByteArray();
        } catch (IOException e) {
            throw new SQLException("Failed to read binary stream", "HY000", e);
        }
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
            if (element instanceof java.sql.Date || element instanceof LocalDate) return "date";
            if (element instanceof java.sql.Time || element instanceof LocalTime) return "time";
            if (element instanceof OffsetTime) return "timetz";
            if (element instanceof java.sql.Timestamp || element instanceof LocalDateTime) return "timestamp";
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

    private void checkClosed() throws SQLException {
        if (closed.get()) {
            throw new SQLException("ResultSet is closed", "HY010");
        }
    }

    private void checkRow() throws SQLException {
        if (currentRowData == null) {
            throw new SQLException("Cursor not on a valid row", "HY109");
        }
    }

    private void checkColumnIndex(int columnIndex) throws SQLException {
        if (columnIndex < 1 || columnIndex > columns.size()) {
            throw new SQLException("Column index out of range: " + columnIndex +
                " (expected 1-" + columns.size() + ")", "42703");
        }
    }

    private void syncColumns() {
        if (stream == null) {
            return;
        }
        List<SBColumnInfo> updated = stream.getColumns();
        if (updated == null) {
            return;
        }
        if (columns != updated) {
            columns = updated;
            rebuildColumnIndex();
            recomputeUpdatableState();
        }
    }

    private void recomputeUpdatableState() {
        if (statement == null || statement.resultSetConcurrency != ResultSet.CONCUR_UPDATABLE) {
            updateTarget = null;
            updatable = false;
            return;
        }
        UpdateTarget resolved = resolveUpdateTarget(statement, columns);
        updateTarget = resolved;
        updatable = resolved != null && resolved.hasWritableColumns();
    }

    private void rebuildColumnIndex() {
        columnNameIndex.clear();
        for (int i = 0; i < columns.size(); i++) {
            columnNameIndex.put(columns.get(i).getName().toLowerCase(), i + 1);
        }
    }

    private static final class ListRowStream implements SBRowStream {
        private final List<SBColumnInfo> columns;
        private final List<Object[]> rows;
        private int index = 0;

        ListRowStream(List<SBColumnInfo> columns, List<Object[]> rows) {
            this.columns = columns == null ? Collections.emptyList() : columns;
            this.rows = rows == null ? Collections.emptyList() : rows;
        }

        @Override
        public Object[] nextRow() {
            if (index >= rows.size()) {
                return null;
            }
            return rows.get(index++);
        }

        @Override
        public List<SBColumnInfo> getColumns() {
            return columns;
        }

        @Override
        public long getUpdateCount() {
            return -1;
        }

        @Override
        public String getCommandTag() {
            return null;
        }

        @Override
        public boolean isDone() {
            return index >= rows.size();
        }

        List<Object[]> getRows() {
            return rows;
        }

        void setIndex(int index) {
            if (index < 0) {
                this.index = 0;
            } else if (index > rows.size()) {
                this.index = rows.size();
            } else {
                this.index = index;
            }
        }
    }
}
