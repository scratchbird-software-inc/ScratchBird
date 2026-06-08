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
import java.util.*;
import java.util.Calendar;
import java.time.*;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicLong;

/**
 * JDBC PreparedStatement implementation for ScratchBird.
 */
public class SBPreparedStatement extends SBStatement implements PreparedStatement {
    private static final AtomicLong INLINE_QUERY_NONCE = new AtomicLong();

    // Original SQL
    protected final String originalSQL;

    // Parsed SQL with ? replaced by $1, $2, etc.
    protected String parsedSQL;

    // Parameters
    protected final List<Object> parameters = new ArrayList<>();
    protected final List<Integer> parameterTypes = new ArrayList<>();
    protected int parameterCount = 0;

    // Generated keys handling
    protected boolean returnGeneratedKeys = false;
    protected int[] generatedKeyColumnIndexes;
    protected String[] generatedKeyColumnNames;

    // Parameter metadata
    protected SBParameterMetaData parameterMetaData;

    // Batch parameters
    protected final List<List<Object>> batchParams = new ArrayList<>();

    // Named parameter mapping for :name / @name placeholders
    protected final Map<String, Integer> namedParameterIndexes = new HashMap<>();
    protected final Map<String, List<Integer>> namedParameterIndexGroups = new HashMap<>();
    protected final Map<Integer, List<Integer>> namedParameterAliasesByIndex = new HashMap<>();

    /**
     * Creates a new prepared statement.
     */
    public SBPreparedStatement(SBConnection connection, String sql, int resultSetType,
                               int resultSetConcurrency, int resultSetHoldability)
            throws SQLException {
        super(connection, resultSetType, resultSetConcurrency, resultSetHoldability);
        this.originalSQL = sql;
        parseSQL();
    }

    /**
     * Parses SQL and counts parameters.
     */
    private void parseSQL() throws SQLException {
        StringBuilder sb = new StringBuilder();
        int paramIndex = 0;
        boolean inQuote = false;
        boolean inDoubleQuote = false;
        boolean inLineComment = false;
        boolean inBlockComment = false;
        String dollarQuoteTag = null;

        for (int i = 0; i < originalSQL.length(); i++) {
            char c = originalSQL.charAt(i);
            char next = (i + 1) < originalSQL.length() ? originalSQL.charAt(i + 1) : '\0';

            if (dollarQuoteTag != null) {
                if (startsWithAt(originalSQL, i, dollarQuoteTag)) {
                    sb.append(dollarQuoteTag);
                    i += dollarQuoteTag.length() - 1;
                    dollarQuoteTag = null;
                } else {
                    sb.append(c);
                }
                continue;
            }

            if (inLineComment) {
                sb.append(c);
                if (c == '\n' || c == '\r') {
                    inLineComment = false;
                }
                continue;
            }

            if (inBlockComment) {
                sb.append(c);
                if (c == '*' && next == '/') {
                    sb.append(next);
                    i++;
                    inBlockComment = false;
                }
                continue;
            }

            if (!inQuote && !inDoubleQuote) {
                if (c == '-' && next == '-') {
                    sb.append(c).append(next);
                    i++;
                    inLineComment = true;
                    continue;
                }
                if (c == '/' && next == '*') {
                    sb.append(c).append(next);
                    i++;
                    inBlockComment = true;
                    continue;
                }
                String parsedTag = parseDollarQuoteTag(originalSQL, i);
                if (parsedTag != null) {
                    sb.append(parsedTag);
                    i += parsedTag.length() - 1;
                    dollarQuoteTag = parsedTag;
                    continue;
                }
            }

            if (c == '\'' && !inDoubleQuote) {
                if (inQuote && next == '\'') {
                    sb.append(c).append(next);
                    i++;
                    continue;
                }
                inQuote = !inQuote;
                sb.append(c);
                continue;
            }

            if (c == '"' && !inQuote) {
                if (inDoubleQuote && next == '"') {
                    sb.append(c).append(next);
                    i++;
                    continue;
                }
                inDoubleQuote = !inDoubleQuote;
                sb.append(c);
                continue;
            }

            if (c == '?' && !inQuote && !inDoubleQuote) {
                paramIndex++;
                sb.append('$').append(paramIndex);
                parameters.add(null);
                parameterTypes.add(Types.NULL);
                continue;
            }

            if (!inQuote && !inDoubleQuote && isNamedParameterStart(c, i)) {
                int nameStart = i + 1;
                int nameEnd = nameStart;
                while (nameEnd < originalSQL.length() && isNamedParameterPart(originalSQL.charAt(nameEnd))) {
                    nameEnd++;
                }
                String parameterName = originalSQL.substring(nameStart, nameEnd);
                paramIndex++;
                sb.append('$').append(paramIndex);
                parameters.add(null);
                parameterTypes.add(Types.NULL);
                registerNamedParameter(parameterName, paramIndex);
                i = nameEnd - 1;
                continue;
            }
            sb.append(c);
        }

        this.parsedSQL = sb.toString();
        this.parameterCount = paramIndex;
    }

    private static boolean startsWithAt(String text, int index, String token) {
        if (text == null || token == null || index < 0) {
            return false;
        }
        int end = index + token.length();
        if (end > text.length()) {
            return false;
        }
        return text.regionMatches(index, token, 0, token.length());
    }

    private static String parseDollarQuoteTag(String sql, int index) {
        if (sql == null || index < 0 || index >= sql.length() || sql.charAt(index) != '$') {
            return null;
        }
        int end = sql.indexOf('$', index + 1);
        if (end < 0) {
            return null;
        }
        if (end == index + 1) {
            return "$$";
        }
        for (int i = index + 1; i < end; i++) {
            char ch = sql.charAt(i);
            if (!(Character.isLetterOrDigit(ch) || ch == '_')) {
                return null;
            }
        }
        return sql.substring(index, end + 1);
    }

    private boolean isNamedParameterStart(char token, int index) {
        if (token != ':' && token != '@') {
            return false;
        }
        if (token == ':') {
            if (index + 1 < originalSQL.length() && originalSQL.charAt(index + 1) == ':') {
                return false;
            }
            if (index > 0 && originalSQL.charAt(index - 1) == ':') {
                return false;
            }
        }
        if (index + 1 >= originalSQL.length()) {
            return false;
        }
        return isNamedParameterPart(originalSQL.charAt(index + 1));
    }

    private boolean isNamedParameterPart(char ch) {
        return Character.isLetterOrDigit(ch) || ch == '_';
    }

    private void registerNamedParameter(String parameterName, int index) {
        if (parameterName == null || parameterName.isBlank()) {
            return;
        }
        String normalized = parameterName.trim().toLowerCase(Locale.ROOT);
        namedParameterIndexes.putIfAbsent(normalized, index);
        List<Integer> aliases = namedParameterIndexGroups.computeIfAbsent(
            normalized, ignored -> new ArrayList<>());
        aliases.add(index);
        for (Integer aliasIndex : aliases) {
            namedParameterAliasesByIndex.put(aliasIndex, aliases);
        }
    }

    protected Integer resolveNamedParameterIndex(String parameterName) {
        if (parameterName == null || parameterName.isBlank()) {
            return null;
        }
        String normalized = parameterName.trim().toLowerCase(Locale.ROOT);
        Integer direct = namedParameterIndexes.get(normalized);
        if (direct != null) {
            return direct;
        }
        if (normalized.startsWith(":") || normalized.startsWith("@")) {
            direct = namedParameterIndexes.get(normalized.substring(1));
            if (direct != null) {
                return direct;
            }
        }
        return null;
    }

    /**
     * Gets SQL with RETURNING clause if needed.
     */
    protected String getFinalSQL() {
        String sql = parsedSQL;

        // Add RETURNING clause for generated keys
        if (returnGeneratedKeys) {
            if (!sql.toUpperCase().contains("RETURNING")) {
                if (generatedKeyColumnNames != null && generatedKeyColumnNames.length > 0) {
                    StringBuilder returning = new StringBuilder(" RETURNING ");
                    for (int i = 0; i < generatedKeyColumnNames.length; i++) {
                        if (i > 0) returning.append(", ");
                        returning.append(generatedKeyColumnNames[i]);
                    }
                    sql = sql + returning.toString();
                } else {
                    sql = sql + " RETURNING *";
                }
            }
        }

        return sql;
    }

    protected int logicalParameterCount() {
        return parameterCount;
    }

    protected int mapParameterIndex(int parameterIndex) throws SQLException {
        return parameterIndex;
    }

    /**
     * Builds final SQL with parameter values.
     */
    protected String buildFinalSQL() throws SQLException {
        StringBuilder sb = new StringBuilder();
        String sql = getFinalSQL();
        int paramIndex = 0;

        int i = 0;
        while (i < sql.length()) {
            char c = sql.charAt(i);

            if (c == '$' && i + 1 < sql.length() && Character.isDigit(sql.charAt(i + 1))) {
                // Find parameter number
                int j = i + 1;
                while (j < sql.length() && Character.isDigit(sql.charAt(j))) {
                    j++;
                }
                int paramNum = Integer.parseInt(sql.substring(i + 1, j));
                sb.append(formatParameter(paramNum - 1));
                i = j;
            } else {
                sb.append(c);
                i++;
            }
        }

        return sb.toString();
    }

    /**
     * Formats a parameter value for SQL.
     */
    protected String formatParameter(int index) throws SQLException {
        if (index < 0 || index >= parameters.size()) {
            throw new SQLException("Parameter index out of range: " + (index + 1), "07001");
        }

        Object value = parameters.get(index);
        if (value == null) {
            return "NULL";
        }

        int type = parameterTypes.get(index);

        if (value instanceof Boolean) {
            return (Boolean) value ? "TRUE" : "FALSE";
        } else if (value instanceof Ref) {
            Object refValue = ((Ref) value).getObject();
            if (refValue == null) {
                return "NULL";
            }
            return "'" + refValue.toString().replace("'", "''") + "'";
        } else if (value instanceof Number) {
            return value.toString();
        } else if (value instanceof String) {
            return "'" + ((String) value).replace("'", "''") + "'";
        } else if (value instanceof byte[]) {
            byte[] bytes = (byte[]) value;
            StringBuilder sb = new StringBuilder("X'");
            for (byte b : bytes) {
                sb.append(String.format("%02x", b & 0xff));
            }
            sb.append("'");
            return sb.toString();
        } else if (value instanceof java.sql.Date) {
            return castTemporalLiteral(value.toString(), "DATE");
        } else if (value instanceof java.sql.Time) {
            return castTemporalLiteral(value.toString(), "TIME");
        } else if (value instanceof java.sql.Timestamp) {
            return castTemporalLiteral(value.toString(), "TIMESTAMP");
        } else if (value instanceof java.util.UUID) {
            return "UUID '" + value.toString() + "'";
        } else if (value instanceof Array) {
            return formatArray((Array) value);
        } else if (value instanceof Object[]) {
            return formatArray(new SBArray("text", (Object[]) value));
        } else if (value instanceof Collection) {
            Object[] elements = ((Collection<?>) value).toArray();
            return formatArray(new SBArray("text", elements));
        } else if (value instanceof java.time.LocalDate) {
            return castTemporalLiteral(value.toString(), "DATE");
        } else if (value instanceof java.time.LocalTime) {
            return castTemporalLiteral(value.toString(), "TIME");
        } else if (value instanceof java.time.LocalDateTime) {
            return castTemporalLiteral(value.toString().replace('T', ' '), "TIMESTAMP");
        } else if (value instanceof java.time.OffsetTime) {
            return "TIMETZ '" + value.toString() + "'";
        } else if (value instanceof java.time.ZonedDateTime) {
            return "TIMESTAMPTZ '" + ((java.time.ZonedDateTime) value).toOffsetDateTime() + "'";
        } else if (value instanceof java.time.OffsetDateTime) {
            return "TIMESTAMPTZ '" + value.toString() + "'";
        } else if (value instanceof java.time.Instant) {
            java.time.OffsetDateTime odt =
                java.time.OffsetDateTime.ofInstant((java.time.Instant) value,
                    java.time.ZoneOffset.UTC);
            return "TIMESTAMPTZ '" + odt.toString() + "'";
        } else {
            return "'" + value.toString().replace("'", "''") + "'";
        }
    }

    private String castTemporalLiteral(String value, String targetType) {
        return "CAST('" + value.replace("'", "''") + "' AS " + targetType + ")";
    }

    /**
     * Formats an array parameter.
     */
    protected String formatArray(Array array) throws SQLException {
        Object[] elements = (Object[]) array.getArray();
        StringBuilder sb = new StringBuilder("ARRAY[");
        for (int i = 0; i < elements.length; i++) {
            if (i > 0) sb.append(", ");
            if (elements[i] == null) {
                sb.append("NULL");
            } else if (elements[i] instanceof String) {
                sb.append("'").append(((String) elements[i]).replace("'", "''")).append("'");
            } else {
                sb.append(elements[i].toString());
            }
        }
        sb.append("]");
        return sb.toString();
    }

    @Override
    public ResultSet executeQuery() throws SQLException {
        checkClosed();
        clearResults();

        boolean inlineLiterals = shouldInlineLiteralExecution();
        String sql = inlineLiterals ? buildInlineQuerySQL() : getFinalSQL();
        lastExecutedSql = sql;
        int pageSize = fetchSize > 0 ? fetchSize : 0;
        if (maxRows > 0 && pageSize > 0) {
            pageSize = Math.min(pageSize, maxRows);
        }

        if (pageSize > 0 && shouldUseStreamingResultSet()) {
            final int streamPageSize = pageSize;
            SBQueryResult result;
            if (inlineLiterals) {
                result = connection.withResilience("query_stream", sql, () ->
                    connection.getProtocol().executeStreaming(sql, streamPageSize, queryTimeout * 1000)
                    , true
                );
            } else {
                result = connection.withResilience("query_stream", sql, () ->
                    connection.getProtocol().executeStreaming(sql, parameters, parameterTypes,
                        streamPageSize, queryTimeout * 1000)
                    , true
                );
            }
            if (result.getStream() == null) {
                throw new SQLException("Query did not return a result set", "02000");
            }
            currentResultSet = new SBResultSet(this, result.getStream(), maxRows);
            bindCurrentResultSetCursor();
            return currentResultSet;
        }

        SBQueryResult result;
        if (inlineLiterals) {
            result = connection.withResilience("query", sql, () ->
                connection.getProtocol().executeNoCache(sql, maxRows, queryTimeout * 1000)
                , true
            );
        } else {
            result = connection.withResilience("query", sql, () ->
                connection.getProtocol().execute(sql, parameters, parameterTypes,
                    maxRows, queryTimeout * 1000)
                , true
            );
        }
        if (result.getColumns() == null || result.getColumns().isEmpty()) {
            throw new SQLException("Query did not return a result set", "02000");
        }
        currentResultSet = new SBResultSet(this, result.getColumns(), result.getRows());
        bindCurrentResultSetCursor();
        return currentResultSet;
    }

    @Override
    public int executeUpdate() throws SQLException {
        return (int) executeLargeUpdate();
    }

    @Override
    public long executeLargeUpdate() throws SQLException {
        checkClosed();
        clearResults();

        boolean inlineLiterals = shouldInlineLiteralExecution();
        String sql = inlineLiterals ? buildFinalSQL() : getFinalSQL();
        lastExecutedSql = sql;
        SBQueryResult result;
        if (inlineLiterals) {
            result = connection.withResilience("update", sql, () ->
                connection.getProtocol().executeNoCache(sql, maxRows, queryTimeout * 1000)
            );
        } else {
            result = connection.withResilience("update", sql, () ->
                connection.getProtocol().execute(sql, parameters, parameterTypes,
                    maxRows, queryTimeout * 1000)
            );
        }

        if (returnGeneratedKeys && result.getColumns() != null && !result.getColumns().isEmpty()) {
            generatedKeys = new SBResultSet(this, result.getColumns(), result.getRows());
        }

        updateCount = result.getUpdateCount();
        return updateCount;
    }

    @Override
    public boolean execute() throws SQLException {
        checkClosed();
        clearResults();

        boolean inlineLiterals = shouldInlineLiteralExecution();
        String sql = inlineLiterals ? buildFinalSQL() : getFinalSQL();
        lastExecutedSql = sql;
        SBQueryResult result;
        if (inlineLiterals) {
            result = connection.withResilience("execute", sql, () ->
                connection.getProtocol().executeNoCache(sql, maxRows, queryTimeout * 1000)
            );
        } else {
            result = connection.withResilience("execute", sql, () ->
                connection.getProtocol().execute(sql, parameters, parameterTypes,
                    maxRows, queryTimeout * 1000)
            );
        }

        if (result.getColumns() != null && !result.getColumns().isEmpty()) {
            if (returnGeneratedKeys) {
                generatedKeys = new SBResultSet(this, result.getColumns(), result.getRows());
                updateCount = result.getUpdateCount();
                return false;
            } else {
                currentResultSet = new SBResultSet(this, result.getColumns(), result.getRows());
                bindCurrentResultSetCursor();
                updateCount = -1;
                return true;
            }
        } else {
            updateCount = result.getUpdateCount();
            return false;
        }
    }

    private boolean shouldInlineLiteralExecution() {
        SBProtocolHandler protocol = connection.getProtocol();
        if (protocol == null || !protocol.isConnected()) {
            return false;
        }
        // Parameterless PreparedStatements are common in DBeaver's table-data
        // reader. Keep them on the normal execution lane instead of forcing the
        // inline/no-cache path so forward-only fetches behave the same as plain
        // Statement reads.
        return parameterCount > 0;
    }

    private String buildInlineQuerySQL() throws SQLException {
        String sql = buildFinalSQL().trim();
        if (sql.endsWith(";")) {
            sql = sql.substring(0, sql.length() - 1).trim();
        }
        if (sql.regionMatches(true, 0, "SELECT", 0, 6)) {
            long nonce = INLINE_QUERY_NONCE.incrementAndGet();
            String noncePredicate = nonce + " = " + nonce;
            String upper = sql.toUpperCase(Locale.ROOT);
            int wherePos = upper.indexOf(" WHERE ");
            if (wherePos >= 0) {
                return sql + " AND " + noncePredicate;
            }

            int insertPos = firstClausePosition(upper,
                " GROUP BY ",
                " ORDER BY ",
                " LIMIT ",
                " OFFSET ",
                " FETCH ",
                " FOR "
            );
            if (insertPos >= 0) {
                return sql.substring(0, insertPos) + " WHERE " + noncePredicate + sql.substring(insertPos);
            }
            return sql + " WHERE " + noncePredicate;
        }
        return sql;
    }

    private int firstClausePosition(String upperSql, String... clauses) {
        int best = -1;
        for (String clause : clauses) {
            int idx = upperSql.indexOf(clause);
            if (idx >= 0 && (best < 0 || idx < best)) {
                best = idx;
            }
        }
        return best;
    }

    private boolean requiresInlineLiteral(int sqlType, Object value) {
        switch (sqlType) {
            case Types.CLOB:
            case Types.NCLOB:
            case Types.LONGVARCHAR:
            case Types.LONGNVARCHAR:
                return true;
            default:
                break;
        }

        // Avoid oversized bind payloads for very large text/binary parameters.
        if (value instanceof String) {
            return ((String) value).length() >= (64 * 1024);
        }
        return false;
    }

    @Override
    public void clearParameters() throws SQLException {
        checkClosed();
        for (int i = 0; i < parameters.size(); i++) {
            parameters.set(i, null);
            parameterTypes.set(i, Types.NULL);
        }
    }

    // ==================== Set Methods ====================

    private void setParameter(int parameterIndex, Object value, int sqlType) throws SQLException {
        checkClosed();
        int logicalCount = logicalParameterCount();
        if (parameterIndex < 1 || parameterIndex > logicalCount) {
            throw new SQLException("Parameter index out of range: " + parameterIndex +
                " (expected 1-" + logicalCount + ")", "07001");
        }
        int mappedIndex = mapParameterIndex(parameterIndex);
        if (mappedIndex < 1 || mappedIndex > parameterCount) {
            throw new SQLException("Parameter index out of range: " + parameterIndex +
                " (expected 1-" + logicalCount + ")", "07001");
        }
        List<Integer> aliasIndexes = namedParameterAliasesByIndex.get(mappedIndex);
        if (aliasIndexes == null || aliasIndexes.isEmpty()) {
            parameters.set(mappedIndex - 1, value);
            parameterTypes.set(mappedIndex - 1, sqlType);
            return;
        }
        for (Integer aliasIndex : aliasIndexes) {
            if (aliasIndex == null || aliasIndex < 1 || aliasIndex > parameterCount) {
                continue;
            }
            parameters.set(aliasIndex - 1, value);
            parameterTypes.set(aliasIndex - 1, sqlType);
        }
    }

    @Override
    public void setNull(int parameterIndex, int sqlType) throws SQLException {
        setParameter(parameterIndex, null, sqlType);
    }

    @Override
    public void setNull(int parameterIndex, int sqlType, String typeName) throws SQLException {
        setParameter(parameterIndex, null, sqlType);
    }

    @Override
    public void setBoolean(int parameterIndex, boolean x) throws SQLException {
        setParameter(parameterIndex, x, Types.BOOLEAN);
    }

    @Override
    public void setByte(int parameterIndex, byte x) throws SQLException {
        setParameter(parameterIndex, (int) x, Types.TINYINT);
    }

    @Override
    public void setShort(int parameterIndex, short x) throws SQLException {
        setParameter(parameterIndex, (int) x, Types.SMALLINT);
    }

    @Override
    public void setInt(int parameterIndex, int x) throws SQLException {
        setParameter(parameterIndex, x, Types.INTEGER);
    }

    @Override
    public void setLong(int parameterIndex, long x) throws SQLException {
        setParameter(parameterIndex, x, Types.BIGINT);
    }

    @Override
    public void setFloat(int parameterIndex, float x) throws SQLException {
        setParameter(parameterIndex, x, Types.REAL);
    }

    @Override
    public void setDouble(int parameterIndex, double x) throws SQLException {
        setParameter(parameterIndex, x, Types.DOUBLE);
    }

    @Override
    public void setBigDecimal(int parameterIndex, BigDecimal x) throws SQLException {
        setParameter(parameterIndex, x, Types.NUMERIC);
    }

    @Override
    public void setString(int parameterIndex, String x) throws SQLException {
        setParameter(parameterIndex, x, Types.VARCHAR);
    }

    @Override
    public void setBytes(int parameterIndex, byte[] x) throws SQLException {
        setParameter(parameterIndex, x, Types.VARBINARY);
    }

    @Override
    public void setDate(int parameterIndex, java.sql.Date x) throws SQLException {
        setParameter(parameterIndex, x, Types.DATE);
    }

    @Override
    public void setDate(int parameterIndex, java.sql.Date x, Calendar cal) throws SQLException {
        setParameter(parameterIndex, applyCalendarDate(x, cal), Types.DATE);
    }

    @Override
    public void setTime(int parameterIndex, java.sql.Time x) throws SQLException {
        setParameter(parameterIndex, x, Types.TIME);
    }

    @Override
    public void setTime(int parameterIndex, java.sql.Time x, Calendar cal) throws SQLException {
        setParameter(parameterIndex, applyCalendarTime(x, cal), Types.TIME);
    }

    @Override
    public void setTimestamp(int parameterIndex, java.sql.Timestamp x) throws SQLException {
        setParameter(parameterIndex, x, Types.TIMESTAMP);
    }

    @Override
    public void setTimestamp(int parameterIndex, java.sql.Timestamp x, Calendar cal) throws SQLException {
        setParameter(parameterIndex, applyCalendarTimestamp(x, cal), Types.TIMESTAMP);
    }

    private static java.sql.Date applyCalendarDate(java.sql.Date value, Calendar cal) {
        if (value == null || cal == null) {
            return value;
        }
        LocalDate localDate = value.toLocalDate();
        ZonedDateTime zoned = localDate.atStartOfDay(cal.getTimeZone().toZoneId());
        return new java.sql.Date(zoned.toInstant().toEpochMilli());
    }

    private static java.sql.Time applyCalendarTime(java.sql.Time value, Calendar cal) {
        if (value == null || cal == null) {
            return value;
        }
        LocalTime localTime = value.toLocalTime();
        LocalDate base = LocalDate.of(1970, 1, 1);
        ZonedDateTime zoned = ZonedDateTime.of(base, localTime, cal.getTimeZone().toZoneId());
        return new java.sql.Time(zoned.toInstant().toEpochMilli());
    }

    private static java.sql.Timestamp applyCalendarTimestamp(java.sql.Timestamp value, Calendar cal) {
        if (value == null || cal == null) {
            return value;
        }
        LocalDateTime localDateTime = value.toLocalDateTime();
        ZonedDateTime zoned = ZonedDateTime.of(localDateTime, cal.getTimeZone().toZoneId());
        java.sql.Timestamp adjusted = java.sql.Timestamp.from(zoned.toInstant());
        adjusted.setNanos(value.getNanos());
        return adjusted;
    }

    @Override
    public void setAsciiStream(int parameterIndex, InputStream x, int length) throws SQLException {
        validateStreamLength(parameterIndex, length);
        byte[] bytes = readExactBytes(parameterIndex, x, length, "ASCII");
        setString(parameterIndex, new String(bytes, StandardCharsets.US_ASCII));
    }

    @Override
    public void setAsciiStream(int parameterIndex, InputStream x, long length) throws SQLException {
        if (length < 0 || length > Integer.MAX_VALUE) {
            throw new SQLException("Stream length out of range", "HY024");
        }
        setAsciiStream(parameterIndex, x, (int) length);
    }

    @Override
    public void setAsciiStream(int parameterIndex, InputStream x) throws SQLException {
        byte[] bytes = readAllBytes(parameterIndex, x, "ASCII");
        setString(parameterIndex, new String(bytes, StandardCharsets.US_ASCII));
    }

    @Override
    @Deprecated
    public void setUnicodeStream(int parameterIndex, InputStream x, int length) throws SQLException {
        validateStreamLength(parameterIndex, length);
        byte[] bytes = readExactBytes(parameterIndex, x, length, "Unicode");
        setString(parameterIndex, new String(bytes, StandardCharsets.UTF_8));
    }

    @Override
    public void setBinaryStream(int parameterIndex, InputStream x, int length) throws SQLException {
        validateStreamLength(parameterIndex, length);
        setBytes(parameterIndex, readExactBytes(parameterIndex, x, length, "binary"));
    }

    @Override
    public void setBinaryStream(int parameterIndex, InputStream x, long length) throws SQLException {
        if (length < 0 || length > Integer.MAX_VALUE) {
            throw new SQLException("Stream length out of range", "HY024");
        }
        setBinaryStream(parameterIndex, x, (int) length);
    }

    @Override
    public void setBinaryStream(int parameterIndex, InputStream x) throws SQLException {
        setBytes(parameterIndex, readAllBytes(parameterIndex, x, "binary"));
    }

    @Override
    public void setObject(int parameterIndex, Object x, int targetSqlType) throws SQLException {
        setObject(parameterIndex, x, targetSqlType, 0);
    }

    @Override
    public void setObject(int parameterIndex, Object x, SQLType targetSqlType, int scaleOrLength)
            throws SQLException {
        if (targetSqlType == null) {
            setObject(parameterIndex, x);
            return;
        }
        setObject(parameterIndex, x, targetSqlType.getVendorTypeNumber(), scaleOrLength);
    }

    @Override
    public void setObject(int parameterIndex, Object x, SQLType targetSqlType) throws SQLException {
        if (targetSqlType == null) {
            setObject(parameterIndex, x);
            return;
        }
        setObject(parameterIndex, x, targetSqlType.getVendorTypeNumber());
    }

    @Override
    public void setObject(int parameterIndex, Object x, int targetSqlType, int scaleOrLength)
            throws SQLException {
        if (x == null) {
            setNull(parameterIndex, targetSqlType);
            return;
        }

        // Convert based on target type
        switch (targetSqlType) {
            case Types.BOOLEAN:
            case Types.BIT:
                if (x instanceof Boolean) {
                    setBoolean(parameterIndex, (Boolean) x);
                } else if (x instanceof Number) {
                    setBoolean(parameterIndex, ((Number) x).intValue() != 0);
                } else {
                    setBoolean(parameterIndex, Boolean.parseBoolean(x.toString()));
                }
                break;
            case Types.TINYINT:
            case Types.SMALLINT:
                if (x instanceof Number) {
                    setShort(parameterIndex, ((Number) x).shortValue());
                } else {
                    setShort(parameterIndex, Short.parseShort(x.toString()));
                }
                break;
            case Types.INTEGER:
                if (x instanceof Number) {
                    setInt(parameterIndex, ((Number) x).intValue());
                } else {
                    setInt(parameterIndex, Integer.parseInt(x.toString()));
                }
                break;
            case Types.BIGINT:
                if (x instanceof Number) {
                    setLong(parameterIndex, ((Number) x).longValue());
                } else {
                    setLong(parameterIndex, Long.parseLong(x.toString()));
                }
                break;
            case Types.REAL:
            case Types.FLOAT:
                if (x instanceof Number) {
                    setFloat(parameterIndex, ((Number) x).floatValue());
                } else {
                    setFloat(parameterIndex, Float.parseFloat(x.toString()));
                }
                break;
            case Types.DOUBLE:
                if (x instanceof Number) {
                    setDouble(parameterIndex, ((Number) x).doubleValue());
                } else {
                    setDouble(parameterIndex, Double.parseDouble(x.toString()));
                }
                break;
            case Types.NUMERIC:
            case Types.DECIMAL:
                if (x instanceof BigDecimal) {
                    setBigDecimal(parameterIndex, (BigDecimal) x);
                } else if (x instanceof Number) {
                    setBigDecimal(parameterIndex, BigDecimal.valueOf(((Number) x).doubleValue()));
                } else {
                    setBigDecimal(parameterIndex, new BigDecimal(x.toString()));
                }
                break;
            case Types.CHAR:
            case Types.VARCHAR:
            case Types.LONGVARCHAR:
                setString(parameterIndex, x.toString());
                break;
            case Types.BINARY:
            case Types.VARBINARY:
            case Types.LONGVARBINARY:
                if (x instanceof byte[]) {
                    setBytes(parameterIndex, (byte[]) x);
                } else if (x instanceof Blob) {
                    setBlob(parameterIndex, (Blob) x);
                } else {
                    throw new SQLException("Cannot convert to binary", "HY000");
                }
                break;
            case Types.BLOB:
                if (x instanceof Blob) {
                    setBlob(parameterIndex, (Blob) x);
                } else if (x instanceof byte[]) {
                    setBytes(parameterIndex, (byte[]) x);
                } else {
                    throw new SQLException("Cannot convert to blob", "HY000");
                }
                break;
            case Types.CLOB:
            case Types.NCLOB:
                if (x instanceof Clob) {
                    setClob(parameterIndex, (Clob) x);
                } else {
                    setString(parameterIndex, x.toString());
                }
                break;
            case Types.ROWID:
                if (x instanceof RowId) {
                    setRowId(parameterIndex, (RowId) x);
                } else {
                    setParameter(parameterIndex, x.toString(), Types.ROWID);
                }
                break;
            case Types.REF:
                if (x instanceof Ref) {
                    setRef(parameterIndex, (Ref) x);
                } else {
                    setParameter(parameterIndex, x, Types.REF);
                }
                break;
            case Types.STRUCT:
                if (x instanceof Struct) {
                    setParameter(parameterIndex, x, Types.STRUCT);
                } else {
                    throw new SQLException("Cannot convert to struct", "HY000");
                }
                break;
            case Types.SQLXML:
                if (x instanceof SQLXML) {
                    setSQLXML(parameterIndex, (SQLXML) x);
                } else {
                    setString(parameterIndex, x.toString());
                }
                break;
            case Types.DATE:
                if (x instanceof java.sql.Date) {
                    setDate(parameterIndex, (java.sql.Date) x);
                } else if (x instanceof java.util.Date) {
                    setDate(parameterIndex, new java.sql.Date(((java.util.Date) x).getTime()));
                } else {
                    setDate(parameterIndex, java.sql.Date.valueOf(x.toString()));
                }
                break;
            case Types.TIME:
                if (x instanceof java.sql.Time) {
                    setTime(parameterIndex, (java.sql.Time) x);
                } else if (x instanceof java.util.Date) {
                    setTime(parameterIndex, new java.sql.Time(((java.util.Date) x).getTime()));
                } else {
                    setTime(parameterIndex, java.sql.Time.valueOf(x.toString()));
                }
                break;
            case Types.TIME_WITH_TIMEZONE:
                if (x instanceof OffsetTime) {
                    setParameter(parameterIndex, x, Types.TIME_WITH_TIMEZONE);
                } else if (x instanceof OffsetDateTime) {
                    setParameter(parameterIndex, ((OffsetDateTime) x).toOffsetTime(), Types.TIME_WITH_TIMEZONE);
                } else if (x instanceof ZonedDateTime) {
                    setParameter(parameterIndex,
                        ((ZonedDateTime) x).toOffsetDateTime().toOffsetTime(),
                        Types.TIME_WITH_TIMEZONE);
                } else if (x instanceof java.sql.Time) {
                    setParameter(parameterIndex,
                        OffsetTime.of(((java.sql.Time) x).toLocalTime(), ZoneOffset.UTC),
                        Types.TIME_WITH_TIMEZONE);
                } else if (x instanceof LocalTime) {
                    setParameter(parameterIndex, OffsetTime.of((LocalTime) x, ZoneOffset.UTC),
                        Types.TIME_WITH_TIMEZONE);
                } else {
                    setParameter(parameterIndex, parseOffsetTimeLiteral(x.toString()),
                        Types.TIME_WITH_TIMEZONE);
                }
                break;
            case Types.TIMESTAMP:
                if (x instanceof java.sql.Timestamp) {
                    setTimestamp(parameterIndex, (java.sql.Timestamp) x);
                } else if (x instanceof java.util.Date) {
                    setTimestamp(parameterIndex, new java.sql.Timestamp(((java.util.Date) x).getTime()));
                } else {
                    setTimestamp(parameterIndex, java.sql.Timestamp.valueOf(x.toString()));
                }
                break;
            case Types.TIMESTAMP_WITH_TIMEZONE:
                if (x instanceof OffsetDateTime) {
                    setParameter(parameterIndex, x, Types.TIMESTAMP_WITH_TIMEZONE);
                } else if (x instanceof ZonedDateTime) {
                    setParameter(parameterIndex, ((ZonedDateTime) x).toOffsetDateTime(),
                        Types.TIMESTAMP_WITH_TIMEZONE);
                } else if (x instanceof Instant) {
                    setParameter(parameterIndex, x, Types.TIMESTAMP_WITH_TIMEZONE);
                } else if (x instanceof java.sql.Timestamp) {
                    setParameter(parameterIndex, ((java.sql.Timestamp) x).toInstant(),
                        Types.TIMESTAMP_WITH_TIMEZONE);
                } else if (x instanceof java.util.Date) {
                    setParameter(parameterIndex, ((java.util.Date) x).toInstant(),
                        Types.TIMESTAMP_WITH_TIMEZONE);
                } else if (x instanceof LocalDateTime) {
                    setParameter(parameterIndex, ((LocalDateTime) x).atOffset(ZoneOffset.UTC),
                        Types.TIMESTAMP_WITH_TIMEZONE);
                } else {
                    setParameter(parameterIndex, parseOffsetDateTimeLiteral(x.toString()),
                        Types.TIMESTAMP_WITH_TIMEZONE);
                }
                break;
            default:
                setParameter(parameterIndex, x, targetSqlType);
                break;
        }
    }

    @Override
    public void setObject(int parameterIndex, Object x) throws SQLException {
        if (x == null) {
            setNull(parameterIndex, Types.NULL);
        } else if (x instanceof Boolean) {
            setBoolean(parameterIndex, (Boolean) x);
        } else if (x instanceof Byte) {
            setByte(parameterIndex, (Byte) x);
        } else if (x instanceof Short) {
            setShort(parameterIndex, (Short) x);
        } else if (x instanceof Integer) {
            setInt(parameterIndex, (Integer) x);
        } else if (x instanceof Long) {
            setLong(parameterIndex, (Long) x);
        } else if (x instanceof Float) {
            setFloat(parameterIndex, (Float) x);
        } else if (x instanceof Double) {
            setDouble(parameterIndex, (Double) x);
        } else if (x instanceof BigDecimal) {
            setBigDecimal(parameterIndex, (BigDecimal) x);
        } else if (x instanceof String) {
            setString(parameterIndex, (String) x);
        } else if (x instanceof byte[]) {
            setBytes(parameterIndex, (byte[]) x);
        } else if (x instanceof java.sql.Date) {
            setDate(parameterIndex, (java.sql.Date) x);
        } else if (x instanceof java.sql.Time) {
            setTime(parameterIndex, (java.sql.Time) x);
        } else if (x instanceof java.sql.Timestamp) {
            setTimestamp(parameterIndex, (java.sql.Timestamp) x);
        } else if (x instanceof Array) {
            setArray(parameterIndex, (Array) x);
        } else if (x instanceof Struct) {
            setParameter(parameterIndex, x, Types.STRUCT);
        } else if (x instanceof Blob) {
            setBlob(parameterIndex, (Blob) x);
        } else if (x instanceof Clob) {
            setClob(parameterIndex, (Clob) x);
        } else if (x instanceof SQLXML) {
            setSQLXML(parameterIndex, (SQLXML) x);
        } else if (x instanceof Ref) {
            setRef(parameterIndex, (Ref) x);
        } else if (x instanceof RowId) {
            setRowId(parameterIndex, (RowId) x);
        } else if (x instanceof java.util.UUID) {
            setParameter(parameterIndex, x, Types.OTHER);
        } else if (x instanceof java.time.LocalDate) {
            setParameter(parameterIndex, x, Types.DATE);
        } else if (x instanceof java.time.LocalTime) {
            setParameter(parameterIndex, x, Types.TIME);
        } else if (x instanceof java.time.LocalDateTime) {
            setParameter(parameterIndex, x, Types.TIMESTAMP);
        } else if (x instanceof java.time.OffsetTime) {
            setParameter(parameterIndex, x, Types.TIME_WITH_TIMEZONE);
        } else if (x instanceof java.time.OffsetDateTime) {
            setParameter(parameterIndex, x, Types.TIMESTAMP_WITH_TIMEZONE);
        } else if (x instanceof java.time.ZonedDateTime) {
            setParameter(parameterIndex, x, Types.TIMESTAMP_WITH_TIMEZONE);
        } else if (x instanceof java.time.Instant) {
            setParameter(parameterIndex, x, Types.TIMESTAMP_WITH_TIMEZONE);
        } else if (x instanceof Object[]) {
            setArray(parameterIndex, new SBArray("text", (Object[]) x));
        } else if (x instanceof Collection) {
            Object[] elements = ((Collection<?>) x).toArray();
            setArray(parameterIndex, new SBArray("text", elements));
        } else if (x instanceof java.net.URL) {
            setString(parameterIndex, x.toString());
        } else if (x instanceof Enum<?>) {
            setString(parameterIndex, ((Enum<?>) x).name());
        } else if (x instanceof CharSequence) {
            setString(parameterIndex, x.toString());
        } else {
            setParameter(parameterIndex, x, Types.OTHER);
        }
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

    private static OffsetDateTime parseOffsetDateTimeLiteral(String value) throws SQLException {
        if (value == null) {
            throw new SQLException("TIMESTAMP WITH TIME ZONE literal cannot be null", "22007");
        }
        String normalized = value.trim();
        if (normalized.contains(" ") && !normalized.contains("T")) {
            normalized = normalized.replace(' ', 'T');
        }
        if (normalized.matches(".*[+-]\\d{2}$")) {
            normalized = normalized + ":00";
        }
        try {
            return OffsetDateTime.parse(normalized);
        } catch (RuntimeException ignored) {
            try {
                return Instant.parse(normalized).atOffset(ZoneOffset.UTC);
            } catch (RuntimeException ignored2) {
                try {
                    return LocalDateTime.parse(normalized).atOffset(ZoneOffset.UTC);
                } catch (RuntimeException ex) {
                    throw new SQLException("Invalid TIMESTAMP WITH TIME ZONE literal: " + value,
                        "22007", ex);
                }
            }
        }
    }

    @Override
    public void setCharacterStream(int parameterIndex, Reader reader, int length) throws SQLException {
        if (length < 0) {
            throw new SQLException("Stream length must be >= 0", "HY024");
        }
        if (length > Integer.MAX_VALUE) {
            throw new SQLException("Stream length out of range", "HY024");
        }
        try {
            char[] chars = new char[length];
            int totalRead = 0;
            while (totalRead < length) {
                int read = reader.read(chars, totalRead, length - totalRead);
                if (read == -1) {
                    break;
                }
                totalRead += read;
            }

            if (totalRead != length) {
                setString(parameterIndex, new String(chars, 0, totalRead));
                return;
            }

            setString(parameterIndex, new String(chars));
        } catch (IOException e) {
            throw new SQLException("Failed to read character stream", "HY000", e);
        }
    }

    @Override
    public void setCharacterStream(int parameterIndex, Reader reader, long length) throws SQLException {
        if (length < 0 || length > Integer.MAX_VALUE) {
            throw new SQLException("Stream length out of range", "HY024");
        }
        setCharacterStream(parameterIndex, reader, (int) length);
    }

    @Override
    public void setCharacterStream(int parameterIndex, Reader reader) throws SQLException {
        setString(parameterIndex, readAllChars(parameterIndex, reader, "character"));
    }

    private void validateStreamLength(int parameterIndex, int length) throws SQLException {
        if (length < 0) {
            throw new SQLException("Stream length must be >= 0", "HY024");
        }
        if (length > Integer.MAX_VALUE) {
            throw new SQLException("Stream length out of range", "HY024");
        }
        int logicalCount = logicalParameterCount();
        if (parameterIndex < 1 || parameterIndex > logicalCount) {
            throw new SQLException("Parameter index out of range: " + parameterIndex +
                " (expected 1-" + logicalCount + ")", "07001");
        }
    }

    private byte[] readExactBytes(int parameterIndex, InputStream input, int expectedLength,
                                 String label) throws SQLException {
        if (expectedLength < 0) {
            throw new SQLException(label + " stream length must be non-negative", "HY024");
        }
        checkClosed();

        ByteArrayOutputStream output = new ByteArrayOutputStream(Math.max(0, expectedLength));
        byte[] buffer = new byte[Math.max(1, Math.min(expectedLength, 8192))];
        int remaining = expectedLength;
        int read;
        try {
            while (remaining > 0 && (read = input.read(buffer, 0,
                Math.min(buffer.length, remaining))) != -1) {
                output.write(buffer, 0, read);
                remaining -= read;
            }
        } catch (IOException e) {
            throw new SQLException("Failed to read " + label + " stream", "HY000", e);
        }

        return output.toByteArray();
    }

    private byte[] readAllBytes(int parameterIndex, InputStream input, String label) throws SQLException {
        checkClosed();
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[8192];
        int read;
        try {
            while ((read = input.read(buffer)) != -1) {
                output.write(buffer, 0, read);
            }
        } catch (IOException e) {
            throw new SQLException("Failed to read " + label + " stream", "HY000", e);
        }
        return output.toByteArray();
    }

    private String readAllChars(int parameterIndex, Reader reader, String label) throws SQLException {
        checkClosed();
        StringBuilder sb = new StringBuilder();
        char[] buffer = new char[8192];
        int read;
        try {
            while ((read = reader.read(buffer)) != -1) {
                sb.append(buffer, 0, read);
            }
        } catch (IOException e) {
            throw new SQLException("Failed to read " + label + " stream", "HY000", e);
        }
        return sb.toString();
    }

    @Override
    public void setRef(int parameterIndex, Ref x) throws SQLException {
        if (x == null) {
            setNull(parameterIndex, Types.REF);
            return;
        }
        setParameter(parameterIndex, x.getObject(), Types.REF);
    }

    @Override
    public void setBlob(int parameterIndex, Blob x) throws SQLException {
        if (x == null) {
            setNull(parameterIndex, Types.BLOB);
        } else {
            setBytes(parameterIndex, x.getBytes(1, (int) x.length()));
        }
    }

    @Override
    public void setBlob(int parameterIndex, InputStream inputStream) throws SQLException {
        setBinaryStream(parameterIndex, inputStream);
    }

    @Override
    public void setBlob(int parameterIndex, InputStream inputStream, long length) throws SQLException {
        setBinaryStream(parameterIndex, inputStream, length);
    }

    @Override
    public void setClob(int parameterIndex, Clob x) throws SQLException {
        if (x == null) {
            setNull(parameterIndex, Types.CLOB);
        } else {
            setString(parameterIndex, x.getSubString(1, (int) x.length()));
        }
    }

    @Override
    public void setClob(int parameterIndex, Reader reader) throws SQLException {
        setCharacterStream(parameterIndex, reader);
    }

    @Override
    public void setClob(int parameterIndex, Reader reader, long length) throws SQLException {
        setCharacterStream(parameterIndex, reader, length);
    }

    @Override
    public void setArray(int parameterIndex, Array x) throws SQLException {
        setParameter(parameterIndex, x, Types.ARRAY);
    }

    @Override
    public void setURL(int parameterIndex, URL x) throws SQLException {
        setString(parameterIndex, x != null ? x.toString() : null);
    }

    @Override
    public void setRowId(int parameterIndex, RowId x) throws SQLException {
        if (x == null) {
            setNull(parameterIndex, Types.ROWID);
            return;
        }
        setObject(parameterIndex, new String(x.getBytes(), StandardCharsets.UTF_8), Types.ROWID);
    }

    @Override
    public void setNString(int parameterIndex, String value) throws SQLException {
        setString(parameterIndex, value);
    }

    @Override
    public void setNCharacterStream(int parameterIndex, Reader value, long length) throws SQLException {
        setCharacterStream(parameterIndex, value, length);
    }

    @Override
    public void setNCharacterStream(int parameterIndex, Reader value) throws SQLException {
        setCharacterStream(parameterIndex, value);
    }

    @Override
    public void setNClob(int parameterIndex, NClob value) throws SQLException {
        setClob(parameterIndex, value);
    }

    @Override
    public void setNClob(int parameterIndex, Reader reader, long length) throws SQLException {
        setClob(parameterIndex, reader, length);
    }

    @Override
    public void setNClob(int parameterIndex, Reader reader) throws SQLException {
        setClob(parameterIndex, reader);
    }

    @Override
    public void setSQLXML(int parameterIndex, SQLXML xmlObject) throws SQLException {
        if (xmlObject == null) {
            setNull(parameterIndex, Types.SQLXML);
        } else {
            setParameter(parameterIndex, xmlObject.getString(), Types.SQLXML);
        }
    }

    // ==================== Metadata ====================

    @Override
    public ResultSetMetaData getMetaData() throws SQLException {
        checkClosed();
        // Return null if not executed yet
        if (currentResultSet != null) {
            return currentResultSet.getMetaData();
        }
        return null;
    }

    @Override
    public ParameterMetaData getParameterMetaData() throws SQLException {
        checkClosed();
        if (parameterMetaData == null) {
            parameterMetaData = new SBParameterMetaData(parameterCount, parameterTypes);
        }
        return parameterMetaData;
    }

    // ==================== Batch Operations ====================

    @Override
    public void addBatch() throws SQLException {
        checkClosed();
        batchParams.add(new ArrayList<>(parameters));
    }

    @Override
    public void clearBatch() throws SQLException {
        checkClosed();
        batchParams.clear();
    }

    @Override
    public int[] executeBatch() throws SQLException {
        checkClosed();

        int[] results = new int[batchParams.size()];
        SQLException firstException = null;

        for (int i = 0; i < batchParams.size(); i++) {
            try {
                // Restore parameters for this batch
                List<Object> params = batchParams.get(i);
                for (int j = 0; j < params.size(); j++) {
                    parameters.set(j, params.get(j));
                }
                int count = executeUpdate();
                results[i] = count;
            } catch (SQLException e) {
                results[i] = Statement.EXECUTE_FAILED;
                if (firstException == null) {
                    firstException = e;
                }
            }
        }

        batchParams.clear();

        if (firstException != null) {
            throw new BatchUpdateException(firstException.getMessage(),
                firstException.getSQLState(), firstException.getErrorCode(),
                results, firstException);
        }

        return results;
    }

    // ==================== Generated Keys ====================

    public void setReturnGeneratedKeys(boolean returnGeneratedKeys) {
        this.returnGeneratedKeys = returnGeneratedKeys;
    }

    public void setGeneratedKeyColumnIndexes(int[] columnIndexes) {
        this.generatedKeyColumnIndexes = columnIndexes;
        this.returnGeneratedKeys = columnIndexes != null && columnIndexes.length > 0;
    }

    public void setGeneratedKeyColumnNames(String[] columnNames) {
        this.generatedKeyColumnNames = columnNames;
        this.returnGeneratedKeys = columnNames != null && columnNames.length > 0;
    }

    // ==================== Not Supported (Statement) ====================

    @Override
    public ResultSet executeQuery(String sql) throws SQLException {
        throw new SQLException("Cannot call executeQuery(String) on PreparedStatement", "HY000");
    }

    @Override
    public int executeUpdate(String sql) throws SQLException {
        throw new SQLException("Cannot call executeUpdate(String) on PreparedStatement", "HY000");
    }

    @Override
    public boolean execute(String sql) throws SQLException {
        throw new SQLException("Cannot call execute(String) on PreparedStatement", "HY000");
    }

    @Override
    public void addBatch(String sql) throws SQLException {
        throw new SQLException("Cannot call addBatch(String) on PreparedStatement", "HY000");
    }
}
