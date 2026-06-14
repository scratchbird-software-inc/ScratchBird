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
import java.util.*;
import java.util.regex.Pattern;

/**
 * JDBC DatabaseMetaData implementation for ScratchBird.
 */
public class SBDatabaseMetaData implements DatabaseMetaData {

    private static final Map<String, List<String>> SYNTHETIC_SYSTEM_VIEWS = Map.of(
        "sys.catalog", List.of("columns", "object_resolver", "schemas", "tables", "views")
    );
    private static final List<String> SYNTHETIC_SYSTEM_SCHEMAS = List.copyOf(SYNTHETIC_SYSTEM_VIEWS.keySet());

    private final SBConnection connection;

    public SBDatabaseMetaData(SBConnection connection) {
        this.connection = connection;
    }

    @Override
    public boolean allProceduresAreCallable() throws SQLException {
        return true;
    }

    @Override
    public boolean allTablesAreSelectable() throws SQLException {
        return true;
    }

    @Override
    public String getURL() throws SQLException {
        SBConnectionProperties props = connection.getConnectionProperties();
        return "jdbc:scratchbird://" + props.getHost() + ":" + props.getPort() + "/" + props.getDatabase();
    }

    @Override
    public String getUserName() throws SQLException {
        return connection.getConnectionProperties().getUser();
    }

    @Override
    public boolean isReadOnly() throws SQLException {
        return connection.isReadOnly();
    }

    @Override
    public boolean nullsAreSortedHigh() throws SQLException {
        return true;  // ScratchBird sorts nulls high
    }

    @Override
    public boolean nullsAreSortedLow() throws SQLException {
        return false;
    }

    @Override
    public boolean nullsAreSortedAtStart() throws SQLException {
        return false;
    }

    @Override
    public boolean nullsAreSortedAtEnd() throws SQLException {
        return true;
    }

    @Override
    public String getDatabaseProductName() throws SQLException {
        return "ScratchBird";
    }

    @Override
    public String getDatabaseProductVersion() throws SQLException {
        String serverVersion = serverVersionFromConnection();
        if (serverVersion != null && !serverVersion.isBlank()) {
            return serverVersion;
        }
        return "1.0.0";
    }

    @Override
    public String getDriverName() throws SQLException {
        return SBDriver.DRIVER_NAME;
    }

    @Override
    public String getDriverVersion() throws SQLException {
        return SBDriver.VERSION;
    }

    @Override
    public int getDriverMajorVersion() {
        return SBDriver.MAJOR_VERSION;
    }

    @Override
    public int getDriverMinorVersion() {
        return SBDriver.MINOR_VERSION;
    }

    @Override
    public boolean usesLocalFiles() throws SQLException {
        return false;
    }

    @Override
    public boolean usesLocalFilePerTable() throws SQLException {
        return false;
    }

    @Override
    public boolean supportsMixedCaseIdentifiers() throws SQLException {
        String identifierCase = normalizedServerParameter("identifier_case");
        return "mixed".equals(identifierCase) || "preserve".equals(identifierCase);
    }

    @Override
    public boolean storesUpperCaseIdentifiers() throws SQLException {
        return "upper".equals(normalizedServerParameter("identifier_case"));
    }

    @Override
    public boolean storesLowerCaseIdentifiers() throws SQLException {
        String identifierCase = normalizedServerParameter("identifier_case");
        return identifierCase.isEmpty() || "lower".equals(identifierCase);
    }

    @Override
    public boolean storesMixedCaseIdentifiers() throws SQLException {
        String identifierCase = normalizedServerParameter("identifier_case");
        return "mixed".equals(identifierCase) || "preserve".equals(identifierCase);
    }

    @Override
    public boolean supportsMixedCaseQuotedIdentifiers() throws SQLException {
        return true;
    }

    @Override
    public boolean storesUpperCaseQuotedIdentifiers() throws SQLException {
        return "upper".equals(normalizedServerParameter("quoted_identifier_case"));
    }

    @Override
    public boolean storesLowerCaseQuotedIdentifiers() throws SQLException {
        return "lower".equals(normalizedServerParameter("quoted_identifier_case"));
    }

    @Override
    public boolean storesMixedCaseQuotedIdentifiers() throws SQLException {
        String quotedCase = normalizedServerParameter("quoted_identifier_case");
        return quotedCase.isEmpty() || "mixed".equals(quotedCase) || "preserve".equals(quotedCase);
    }

    @Override
    public String getIdentifierQuoteString() throws SQLException {
        return "\"";
    }

    @Override
    public String getSQLKeywords() throws SQLException {
        return "ABORT,ANALYSE,ANALYZE,ARRAY,BIGINT,BINARY,BIT,BOOLEAN,BOTH,CASE," +
               "CAST,CHAR,CHARACTER,CLUSTER,COALESCE,COLLATION,CONSTRAINT,COPY,CROSS," +
               "CURRENT,DATABASE,DEFAULT,DEFERRABLE,DESC,DISTINCT,DO,ELSE,END,EXCEPT," +
               "EXISTS,EXPLAIN,EXTEND,EXTRACT,FALSE,FETCH,FLOAT,FOR,FOREIGN,FROM,FULL," +
               "FUNCTION,GRANT,GROUP,HAVING,ILIKE,IN,INDEX,INITIALLY,INNER,INOUT,INTERSECT," +
               "INTO,IS,ISNULL,JOIN,LEADING,LEFT,LIKE,LIMIT,LISTEN,LOAD,LOCAL,LOCK,MOVE," +
               "NATURAL,NCHAR,NEW,NOT,NOTNULL,NULL,NULLIF,NUMERIC,OFF,OFFSET,OLD,ON,ONLY," +
               "OR,ORDER,OUTER,OVERLAPS,OVERLAY,PARTIAL,POSITION,PRECISION,PRIMARY,PRIVILEGES," +
               "PROCEDURE,PUBLIC,REFERENCES,REINDEX,RESET,RESTRICT,RETURNING,REVOKE,RIGHT," +
               "ROLLBACK,ROW,SAVEPOINT,SCHEMA,SELECT,SESSION,SETOF,SIMILAR,SOME,SUBSTRING," +
               "TABLE,THEN,TO,TRAILING,TRANSACTION,TREAT,TRIGGER,TRIM,TRUE,TRUNCATE,UNION," +
               "UNIQUE,UNKNOWN,UPDATE,USER,USING,VALUES,VARCHAR,VARYING,VERBOSE,VIEW,WHEN," +
               "WHERE,WITH";
    }

    @Override
    public String getNumericFunctions() throws SQLException {
        return "ABS,ACOS,ASIN,ATAN,ATAN2,CEILING,COS,COT,DEGREES,EXP,FLOOR,LOG,LOG10," +
               "MOD,PI,POWER,RADIANS,RAND,ROUND,SIGN,SIN,SQRT,TAN,TRUNCATE";
    }

    @Override
    public String getStringFunctions() throws SQLException {
        return "ASCII,CHAR,CONCAT,INSERT,LCASE,LEFT,LENGTH,LOCATE,LTRIM,REPEAT,REPLACE," +
               "RIGHT,RTRIM,SPACE,SUBSTRING,UCASE";
    }

    @Override
    public String getSystemFunctions() throws SQLException {
        return "DATABASE,IFNULL,USER";
    }

    @Override
    public String getTimeDateFunctions() throws SQLException {
        return "CURDATE,CURTIME,DAYNAME,DAYOFMONTH,DAYOFWEEK,DAYOFYEAR,HOUR,MINUTE," +
               "MONTH,MONTHNAME,NOW,QUARTER,SECOND,TIMESTAMPADD,TIMESTAMPDIFF,WEEK,YEAR";
    }

    @Override
    public String getSearchStringEscape() throws SQLException {
        return "\\";
    }

    @Override
    public String getExtraNameCharacters() throws SQLException {
        return "";
    }

    @Override
    public boolean supportsAlterTableWithAddColumn() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsAlterTableWithDropColumn() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsColumnAliasing() throws SQLException {
        return true;
    }

    @Override
    public boolean nullPlusNonNullIsNull() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsConvert() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsConvert(int fromType, int toType) throws SQLException {
        if (fromType == toType) {
            return true;
        }
        if (isNumericType(fromType) && isNumericType(toType)) {
            return true;
        }
        if (isCharacterType(fromType) || isCharacterType(toType)) {
            return true;
        }
        if (isTemporalType(fromType) && isTemporalType(toType)) {
            return true;
        }
        if (isBinaryType(fromType) && isBinaryType(toType)) {
            return true;
        }
        if (fromType == Types.BOOLEAN && (isNumericType(toType) || isCharacterType(toType))) {
            return true;
        }
        if (toType == Types.BOOLEAN && (isNumericType(fromType) || isCharacterType(fromType))) {
            return true;
        }
        return false;
    }

    @Override
    public boolean supportsTableCorrelationNames() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsDifferentTableCorrelationNames() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsExpressionsInOrderBy() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsOrderByUnrelated() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsGroupBy() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsGroupByUnrelated() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsGroupByBeyondSelect() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsLikeEscapeClause() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsMultipleResultSets() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsMultipleTransactions() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsNonNullableColumns() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsMinimumSQLGrammar() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsCoreSQLGrammar() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsExtendedSQLGrammar() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsANSI92EntryLevelSQL() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsANSI92IntermediateSQL() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsANSI92FullSQL() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsIntegrityEnhancementFacility() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsOuterJoins() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsFullOuterJoins() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsLimitedOuterJoins() throws SQLException {
        return true;
    }

    @Override
    public String getSchemaTerm() throws SQLException {
        return "schema";
    }

    @Override
    public String getProcedureTerm() throws SQLException {
        return "function";
    }

    @Override
    public String getCatalogTerm() throws SQLException {
        return "database";
    }

    @Override
    public boolean isCatalogAtStart() throws SQLException {
        return true;
    }

    @Override
    public String getCatalogSeparator() throws SQLException {
        return ".";
    }

    @Override
    public boolean supportsSchemasInDataManipulation() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsSchemasInProcedureCalls() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsSchemasInTableDefinitions() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsSchemasInIndexDefinitions() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsSchemasInPrivilegeDefinitions() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsCatalogsInDataManipulation() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsCatalogsInProcedureCalls() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsCatalogsInTableDefinitions() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsCatalogsInIndexDefinitions() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsCatalogsInPrivilegeDefinitions() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsPositionedDelete() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsPositionedUpdate() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsSelectForUpdate() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsStoredProcedures() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsSubqueriesInComparisons() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsSubqueriesInExists() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsSubqueriesInIns() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsSubqueriesInQuantifieds() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsCorrelatedSubqueries() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsUnion() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsUnionAll() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsOpenCursorsAcrossCommit() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsOpenCursorsAcrossRollback() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsOpenStatementsAcrossCommit() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsOpenStatementsAcrossRollback() throws SQLException {
        return true;
    }

    @Override
    public int getMaxBinaryLiteralLength() throws SQLException {
        return 0;  // No limit
    }

    @Override
    public int getMaxCharLiteralLength() throws SQLException {
        return 0;  // No limit
    }

    @Override
    public int getMaxColumnNameLength() throws SQLException {
        return 63;
    }

    @Override
    public int getMaxColumnsInGroupBy() throws SQLException {
        return 0;  // No limit
    }

    @Override
    public int getMaxColumnsInIndex() throws SQLException {
        return 32;
    }

    @Override
    public int getMaxColumnsInOrderBy() throws SQLException {
        return 0;  // No limit
    }

    @Override
    public int getMaxColumnsInSelect() throws SQLException {
        return 0;  // No limit
    }

    @Override
    public int getMaxColumnsInTable() throws SQLException {
        return 1600;
    }

    @Override
    public int getMaxConnections() throws SQLException {
        return 0;  // No limit
    }

    @Override
    public int getMaxCursorNameLength() throws SQLException {
        return 63;
    }

    @Override
    public int getMaxIndexLength() throws SQLException {
        return 0;  // No limit
    }

    @Override
    public int getMaxSchemaNameLength() throws SQLException {
        return 63;
    }

    @Override
    public int getMaxProcedureNameLength() throws SQLException {
        return 63;
    }

    @Override
    public int getMaxCatalogNameLength() throws SQLException {
        return 63;
    }

    @Override
    public int getMaxRowSize() throws SQLException {
        return 1073741823;  // 1GB - 1
    }

    @Override
    public boolean doesMaxRowSizeIncludeBlobs() throws SQLException {
        return false;
    }

    @Override
    public int getMaxStatementLength() throws SQLException {
        return 0;  // No limit
    }

    @Override
    public int getMaxStatements() throws SQLException {
        return 0;  // No limit
    }

    @Override
    public int getMaxTableNameLength() throws SQLException {
        return 63;
    }

    @Override
    public int getMaxTablesInSelect() throws SQLException {
        return 0;  // No limit
    }

    @Override
    public int getMaxUserNameLength() throws SQLException {
        return 63;
    }

    @Override
    public int getDefaultTransactionIsolation() throws SQLException {
        return Connection.TRANSACTION_READ_COMMITTED;
    }

    @Override
    public boolean supportsTransactions() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsTransactionIsolationLevel(int level) throws SQLException {
        return level == Connection.TRANSACTION_READ_UNCOMMITTED ||
               level == Connection.TRANSACTION_READ_COMMITTED ||
               level == Connection.TRANSACTION_REPEATABLE_READ ||
               level == Connection.TRANSACTION_SERIALIZABLE ||
               level == SBConnection.TRANSACTION_SNAPSHOT;
    }

    @Override
    public boolean supportsDataDefinitionAndDataManipulationTransactions() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsDataManipulationTransactionsOnly() throws SQLException {
        return false;
    }

    @Override
    public boolean dataDefinitionCausesTransactionCommit() throws SQLException {
        return false;
    }

    @Override
    public boolean dataDefinitionIgnoredInTransactions() throws SQLException {
        return false;
    }

    @Override
    public ResultSet getProcedures(String catalog, String schemaPattern, String procedureNamePattern)
            throws SQLException {
        String currentCatalog = connection.getConnectionProperties().getDatabase();
        if (catalog != null && currentCatalog != null && !catalog.equalsIgnoreCase(currentCatalog)) {
            return createEmptyResultSet(
                new String[]{"PROCEDURE_CAT", "PROCEDURE_SCHEM", "PROCEDURE_NAME", "RESERVED1",
                             "RESERVED2", "RESERVED3", "REMARKS", "PROCEDURE_TYPE", "SPECIFIC_NAME"},
                new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR,
                          Types.VARCHAR, Types.VARCHAR, Types.SMALLINT, Types.VARCHAR}
            );
        }

        List<Object[]> rows = new ArrayList<>();
        try {
            for (Object[] row : queryRows(
                "SELECT routine_schema, routine_name, routine_name, data_type " +
                "FROM information_schema.routines " +
                "WHERE routine_type = 'PROCEDURE'"
            )) {
                String schemaName = toStringValue(row, 0);
                String procedureName = toStringValue(row, 1);
                if (!matchesPattern(schemaName, schemaPattern)
                        || !matchesPattern(procedureName, procedureNamePattern)) {
                    continue;
                }

                String specificName = toStringValue(row, 2);
                String dataType = toStringValue(row, 3);
                rows.add(new Object[]{
                    currentCatalog,
                    schemaName,
                    procedureName,
                    null,
                    null,
                    null,
                    null,
                    mapProcedureTypeFromDataType(dataType),
                    specificName
                });
            }
        } catch (SQLException ex) {
            return createEmptyResultSet(
                new String[]{"PROCEDURE_CAT", "PROCEDURE_SCHEM", "PROCEDURE_NAME", "RESERVED1",
                             "RESERVED2", "RESERVED3", "REMARKS", "PROCEDURE_TYPE", "SPECIFIC_NAME"},
                new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR,
                          Types.VARCHAR, Types.VARCHAR, Types.SMALLINT, Types.VARCHAR}
            );
        }

        List<SBColumnInfo> cols = new ArrayList<>();
        cols.add(column("PROCEDURE_CAT", 25));
        cols.add(column("PROCEDURE_SCHEM", 25));
        cols.add(column("PROCEDURE_NAME", 25));
        cols.add(column("RESERVED1", 25));
        cols.add(column("RESERVED2", 25));
        cols.add(column("RESERVED3", 25));
        cols.add(column("REMARKS", 25));
        cols.add(column("PROCEDURE_TYPE", 21));
        cols.add(column("SPECIFIC_NAME", 25));
        return new SBResultSet(null, cols, rows);
    }

    @Override
    public ResultSet getProcedureColumns(String catalog, String schemaPattern,
            String procedureNamePattern, String columnNamePattern) throws SQLException {
        String currentCatalog = connection.getConnectionProperties().getDatabase();
        if (catalog != null && currentCatalog != null && !catalog.equalsIgnoreCase(currentCatalog)) {
            return createEmptyResultSet(
                new String[]{"PROCEDURE_CAT", "PROCEDURE_SCHEM", "PROCEDURE_NAME", "COLUMN_NAME",
                             "COLUMN_TYPE", "DATA_TYPE", "TYPE_NAME", "PRECISION", "LENGTH", "SCALE",
                             "RADIX", "NULLABLE", "REMARKS", "COLUMN_DEF", "SQL_DATA_TYPE",
                             "SQL_DATETIME_SUB", "CHAR_OCTET_LENGTH", "ORDINAL_POSITION", "IS_NULLABLE",
                             "SPECIFIC_NAME"},
                new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.SMALLINT,
                          Types.INTEGER, Types.VARCHAR, Types.INTEGER, Types.INTEGER, Types.SMALLINT,
                          Types.SMALLINT, Types.SMALLINT, Types.VARCHAR, Types.VARCHAR, Types.INTEGER,
                          Types.INTEGER, Types.INTEGER, Types.INTEGER, Types.VARCHAR, Types.VARCHAR}
            );
        }

        List<Object[]> rows = new ArrayList<>();
        try {
            for (Object[] row : queryRows(
                "SELECT p.specific_schema, p.specific_name, p.parameter_mode, p.parameter_name, " +
                "       p.data_type, p.character_maximum_length, p.numeric_precision, p.numeric_scale, " +
                "       p.ordinal_position " +
                "FROM information_schema.parameters p " +
                "JOIN information_schema.routines r " +
                "  ON r.routine_schema = p.specific_schema " +
                " AND r.routine_name = p.specific_name " +
                "WHERE r.routine_type = 'PROCEDURE'"
            )) {
                String schemaName = toStringValue(row, 0);
                String procedureName = toStringValue(row, 1);
                if (!matchesPattern(schemaName, schemaPattern)
                        || !matchesPattern(procedureName, procedureNamePattern)) {
                    continue;
                }

                String parameterName = toStringValue(row, 3);
                if (!matchesPattern(parameterName, columnNamePattern)) {
                    continue;
                }

                String mode = toStringValue(row, 2);
                String typeName = normalizeTypeName(toStringValue(row, 4));
                int jdbcType = jdbcTypeFromTypeName(typeName);
                int precision = toIntValue(row[6], 0);
                int length = toIntValue(row[5], 0);
                int scale = toIntValue(row[7], 0);
                short nullable = DatabaseMetaData.columnNullable;

                rows.add(new Object[]{
                    currentCatalog,
                    schemaName,
                    procedureName,
                    parameterName,
                    mapProcedureColumnType(mode),
                    jdbcType,
                    typeName,
                    precision,
                    length,
                    scale,
                    10,
                    nullable,
                    null,
                    null,
                    null,
                    toIntValue(row[5], 0),
                    toShortValue(row[8]),
                    nullable == DatabaseMetaData.columnNullable ? "YES" : "NO",
                    procedureName
                });
            }
        } catch (SQLException ex) {
            return createEmptyResultSet(
                new String[]{"PROCEDURE_CAT", "PROCEDURE_SCHEM", "PROCEDURE_NAME", "COLUMN_NAME",
                             "COLUMN_TYPE", "DATA_TYPE", "TYPE_NAME", "PRECISION", "LENGTH", "SCALE",
                             "RADIX", "NULLABLE", "REMARKS", "COLUMN_DEF", "SQL_DATA_TYPE",
                             "SQL_DATETIME_SUB", "CHAR_OCTET_LENGTH", "ORDINAL_POSITION", "IS_NULLABLE",
                             "SPECIFIC_NAME"},
                new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.SMALLINT,
                          Types.INTEGER, Types.VARCHAR, Types.INTEGER, Types.INTEGER, Types.SMALLINT,
                          Types.SMALLINT, Types.SMALLINT, Types.VARCHAR, Types.VARCHAR, Types.INTEGER,
                          Types.INTEGER, Types.INTEGER, Types.INTEGER, Types.VARCHAR, Types.VARCHAR}
            );
        }

        List<SBColumnInfo> cols = new ArrayList<>();
        cols.add(column("PROCEDURE_CAT", 25));
                cols.add(column("PROCEDURE_SCHEM", 25));
                cols.add(column("PROCEDURE_NAME", 25));
                cols.add(column("COLUMN_NAME", 25));
                cols.add(column("COLUMN_TYPE", 21));
                cols.add(column("DATA_TYPE", 23));
        cols.add(column("TYPE_NAME", 25));
        cols.add(column("PRECISION", 23));
        cols.add(column("LENGTH", 23));
        cols.add(column("SCALE", 21));
        cols.add(column("RADIX", 21));
        cols.add(column("NULLABLE", 21));
        cols.add(column("REMARKS", 25));
        cols.add(column("COLUMN_DEF", 25));
        cols.add(column("SQL_DATA_TYPE", 23));
        cols.add(column("SQL_DATETIME_SUB", 23));
        cols.add(column("CHAR_OCTET_LENGTH", 23));
        cols.add(column("ORDINAL_POSITION", 23));
        cols.add(column("IS_NULLABLE", 25));
        cols.add(column("SPECIFIC_NAME", 25));
        return new SBResultSet(null, cols, rows);
    }

    @Override
    public ResultSet getTables(String catalog, String schemaPattern, String tableNamePattern, String[] types)
            throws SQLException {
        String currentCatalog = currentCatalogName();
        if (catalog != null && currentCatalog != null && !catalog.equalsIgnoreCase(currentCatalog)) {
            return createEmptyResultSet(
                new String[]{"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "TABLE_TYPE", "REMARKS",
                             "TYPE_CAT", "TYPE_SCHEM", "TYPE_NAME", "SELF_REFERENCING_COL_NAME",
                             "REF_GENERATION"},
                new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR,
                          Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR}
            );
        }

        Set<String> typeFilter = normalizeTypes(types);
        List<Object[]> rows = new ArrayList<>();

        boolean loadedBaseTables = false;
        try {
            for (Object[] row : queryRows(
                "SELECT t.table_name, t.table_type, s.schema_name " +
                "FROM sys.tables t " +
                "JOIN sys.schemas s ON s.schema_id = t.schema_id " +
                "WHERE t.is_valid = 1 AND s.is_valid = 1"
            )) {
                String tableName = toStringValue(row, 0);
                String schemaName = toStringValue(row, 2);
                if (!matchesPattern(schemaName, schemaPattern) || !matchesPattern(tableName, tableNamePattern)) {
                    continue;
                }
                String tableType = mapTableType(row[1], schemaName, false);
                if (!matchesTypeFilter(tableType, typeFilter)) {
                    continue;
                }
                loadedBaseTables = true;
                rows.add(new Object[]{
                    currentCatalog,
                    schemaName,
                    tableName,
                    tableType,
                    null,
                    null,
                    null,
                    null,
                    null,
                    null
                });
            }
        } catch (SQLException ignored) {
            // Fall back to information_schema for older/minimal runtime builds.
        }

        if (!loadedBaseTables) {
            try {
                for (Object[] row : queryRows(
                    "SELECT table_name, table_type, table_schema " +
                    "FROM information_schema.tables"
                )) {
                    String tableName = toStringValue(row, 0);
                    String schemaName = toStringValue(row, 2);
                    if (!matchesPattern(schemaName, schemaPattern) || !matchesPattern(tableName, tableNamePattern)) {
                        continue;
                    }
                    String tableType = mapInfoSchemaTableType(row[1], schemaName);
                    if (!matchesTypeFilter(tableType, typeFilter)) {
                        continue;
                    }
                    rows.add(new Object[]{
                        currentCatalog,
                        schemaName,
                        tableName,
                        tableType,
                        null,
                        null,
                        null,
                        null,
                        null,
                        null
                    });
                }
            } catch (SQLException ignored) {
                // Leave result empty; caller receives stable metadata shape.
            }
        }

        try {
            for (Object[] row : queryRows(
                "SELECT v.view_name, v.is_materialized, s.schema_name " +
                "FROM sys.views v " +
                "JOIN sys.schemas s ON s.schema_id = v.schema_id " +
                "WHERE v.is_valid = 1 AND s.is_valid = 1"
            )) {
                String viewName = toStringValue(row, 0);
                String schemaName = toStringValue(row, 2);
                if (!matchesPattern(schemaName, schemaPattern) || !matchesPattern(viewName, tableNamePattern)) {
                    continue;
                }
                String tableType = mapTableType(row[1], schemaName, true);
                if (!matchesTypeFilter(tableType, typeFilter)) {
                    continue;
                }
                rows.add(new Object[]{
                    currentCatalog,
                    schemaName,
                    viewName,
                    tableType,
                    null,
                    null,
                    null,
                    null,
                    null,
                    null
                });
            }
        } catch (SQLException ignored) {
            // Older/minimal runtime builds may not expose sys.views. Keep table
            // metadata available and fall back to synthetic monitoring views below.
        }

        if (matchesTypeFilter("SYSTEM VIEW", typeFilter)) {
            Set<String> existing = new HashSet<>();
            for (Object[] row : rows) {
                if (row[2] != null && row[1] != null) {
                    existing.add(row[1].toString().toLowerCase() + "." + row[2].toString().toLowerCase());
                }
            }
            for (String name : monitoringViews()) {
                if (!matchesPattern("sys", schemaPattern) || !matchesPattern(name, tableNamePattern)) {
                    continue;
                }
                String key = "sys." + name.toLowerCase();
                if (existing.contains(key)) {
                    continue;
                }
                rows.add(new Object[]{
                    currentCatalog,
                    "sys",
                    name,
                    "SYSTEM VIEW",
                    null,
                    null,
                    null,
                    null,
                    null,
                    null
                });
            }
            appendSyntheticSystemViews(rows, existing, currentCatalog, schemaPattern, tableNamePattern);
        }

        List<SBColumnInfo> cols = new ArrayList<>();
        cols.add(column("TABLE_CAT", 25));
        cols.add(column("TABLE_SCHEM", 25));
        cols.add(column("TABLE_NAME", 25));
        cols.add(column("TABLE_TYPE", 25));
        cols.add(column("REMARKS", 25));
        cols.add(column("TYPE_CAT", 25));
        cols.add(column("TYPE_SCHEM", 25));
        cols.add(column("TYPE_NAME", 25));
        cols.add(column("SELF_REFERENCING_COL_NAME", 25));
        cols.add(column("REF_GENERATION", 25));
        return new SBResultSet(null, cols, rows);
    }

    @Override
    public ResultSet getSchemas() throws SQLException {
        return getSchemas(null, null);
    }

    @Override
    public ResultSet getSchemas(String catalog, String schemaPattern) throws SQLException {
        String currentCatalog = currentCatalogName();
        if (catalog != null && currentCatalog != null && !catalog.equalsIgnoreCase(currentCatalog)) {
            return createEmptyResultSet(
                new String[]{"TABLE_SCHEM", "TABLE_CATALOG"},
                new int[]{Types.VARCHAR, Types.VARCHAR}
            );
        }
        boolean expandSchemaParents = expandSchemaParentNodesInMetadata();
        LinkedHashSet<String> schemaNames = new LinkedHashSet<>();
        List<Object[]> sourceRows = Collections.emptyList();
        try {
            sourceRows = queryRows("SELECT schema_name FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name");
        } catch (SQLException ignored) {
            try {
                sourceRows = queryRows("SELECT schema_name FROM information_schema.schemata ORDER BY schema_name");
            } catch (SQLException ignoredAgain) {
                sourceRows = Collections.emptyList();
            }
        }

        for (Object[] row : sourceRows) {
            appendSchemaName(schemaNames, toStringValue(row, 0), schemaPattern, expandSchemaParents);
        }
        appendSyntheticSystemSchemas(schemaNames, schemaPattern, expandSchemaParents);

        List<Object[]> rows = new ArrayList<>(schemaNames.size());
        for (String schemaName : schemaNames) {
            rows.add(new Object[]{schemaName, currentCatalog});
        }
        List<SBColumnInfo> cols = new ArrayList<>();
        cols.add(column("TABLE_SCHEM", 25));
        cols.add(column("TABLE_CATALOG", 25));
        return new SBResultSet(null, cols, rows);
    }

    @Override
    public ResultSet getCatalogs() throws SQLException {
        List<Object[]> rows = new ArrayList<>();
        String currentCatalog = connection.getConnectionProperties().getDatabase();
        if (currentCatalog != null && !currentCatalog.isEmpty()) {
            rows.add(new Object[]{currentCatalog});
        }
        List<SBColumnInfo> cols = new ArrayList<>();
        cols.add(column("TABLE_CAT", 25));
        return new SBResultSet(null, cols, rows);
    }

    @Override
    public ResultSet getTableTypes() throws SQLException {
        List<SBColumnInfo> cols = new ArrayList<>();
        SBColumnInfo col = new SBColumnInfo();
        col.setName("TABLE_TYPE");
        col.setTypeOid(25);  // text
        cols.add(col);

        List<Object[]> rows = new ArrayList<>();
        rows.add(new Object[]{"TABLE"});
        rows.add(new Object[]{"VIEW"});
        rows.add(new Object[]{"SYSTEM TABLE"});
        rows.add(new Object[]{"SYSTEM VIEW"});
        rows.add(new Object[]{"FOREIGN TABLE"});
        rows.add(new Object[]{"MATERIALIZED VIEW"});

        return new SBResultSet(null, cols, rows);
    }

    @Override
    public ResultSet getColumns(String catalog, String schemaPattern, String tableNamePattern,
            String columnNamePattern) throws SQLException {
        String currentCatalog = currentCatalogName();
        if (catalog != null && currentCatalog != null && !catalog.equalsIgnoreCase(currentCatalog)) {
            return createEmptyResultSet(
                new String[]{"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "COLUMN_NAME", "DATA_TYPE",
                             "TYPE_NAME", "COLUMN_SIZE", "BUFFER_LENGTH", "DECIMAL_DIGITS",
                             "NUM_PREC_RADIX", "NULLABLE", "REMARKS", "COLUMN_DEF", "SQL_DATA_TYPE",
                             "SQL_DATETIME_SUB", "CHAR_OCTET_LENGTH", "ORDINAL_POSITION", "IS_NULLABLE",
                             "SCOPE_CATALOG", "SCOPE_SCHEMA", "SCOPE_TABLE", "SOURCE_DATA_TYPE",
                             "IS_AUTOINCREMENT", "IS_GENERATEDCOLUMN"},
                new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.INTEGER,
                          Types.VARCHAR, Types.INTEGER, Types.INTEGER, Types.INTEGER, Types.INTEGER,
                          Types.INTEGER, Types.VARCHAR, Types.VARCHAR, Types.INTEGER, Types.INTEGER,
                          Types.INTEGER, Types.INTEGER, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR,
                          Types.VARCHAR, Types.SMALLINT, Types.VARCHAR, Types.VARCHAR}
            );
        }

        List<Object[]> rows = new ArrayList<>();
        boolean loadedColumns = false;
        try {
            for (Object[] row : queryRows(
                "SELECT c.column_name, c.data_type_id, c.ordinal_position, c.is_nullable, c.default_value, " +
                "t.table_name, s.schema_name " +
                "FROM sys.columns c " +
                "JOIN sys.tables t ON t.table_id = c.table_id " +
                "JOIN sys.schemas s ON s.schema_id = t.schema_id " +
                "WHERE c.is_valid = 1 AND t.is_valid = 1 AND s.is_valid = 1"
            )) {
                String columnName = toStringValue(row, 0);
                String tableName = toStringValue(row, 5);
                String schemaName = toStringValue(row, 6);
                if (!matchesPattern(schemaName, schemaPattern) ||
                    !matchesPattern(tableName, tableNamePattern) ||
                    !matchesPattern(columnName, columnNamePattern)) {
                    continue;
                }

                Object typeValue = row[1];
                Integer oid = parseOid(typeValue);
                String typeName = oid != null ? typeNameFromOid(oid) : toStringValue(typeValue);
                int jdbcType = oid != null ? jdbcTypeFromOid(oid) : jdbcTypeFromTypeName(typeName);
                int columnSize = columnSizeForType(typeName, jdbcType);
                Integer decimalDigits = decimalDigitsForType(typeName, jdbcType);
                Integer numPrecRadix = numPrecRadixForType(jdbcType);
                Integer charOctetLength = charOctetLengthForType(typeName, jdbcType, columnSize);

                int ordinal = toIntValue(row[2], 0);
                boolean nullable = toBooleanValue(row[3]);
                String nullableText = nullable ? "YES" : "NO";
                int nullableFlag = nullable ? DatabaseMetaData.columnNullable : DatabaseMetaData.columnNoNulls;
                String defaultValue = toStringValue(row, 4);
                String autoIncrementFlag = isAutoIncrementDefaultExpression(defaultValue) ? "YES" : "NO";
                String generatedFlag = isGeneratedColumnExpression(defaultValue) ? "YES" : "NO";

                loadedColumns = true;
                rows.add(new Object[]{
                    currentCatalog,
                    schemaName,
                    tableName,
                    columnName,
                    jdbcType,
                    typeName,
                    columnSize,
                    0,
                    decimalDigits,
                    numPrecRadix,
                    nullableFlag,
                    null,
                    defaultValue,
                    null,
                    null,
                    charOctetLength,
                    ordinal,
                    nullableText,
                    null,
                    null,
                    null,
                    null,
                    autoIncrementFlag,
                    generatedFlag
                });
            }
        } catch (SQLException ignored) {
            // Fall back to information_schema for older/minimal runtime builds.
        }

        if (!loadedColumns) {
            try {
                for (Object[] row : queryRows(
                    "SELECT column_name, data_type, ordinal_position, is_nullable, column_default, " +
                    "table_name, table_schema " +
                    "FROM information_schema.columns"
                )) {
                    String columnName = toStringValue(row, 0);
                    String tableName = toStringValue(row, 5);
                    String schemaName = toStringValue(row, 6);
                    if (!matchesPattern(schemaName, schemaPattern) ||
                        !matchesPattern(tableName, tableNamePattern) ||
                        !matchesPattern(columnName, columnNamePattern)) {
                        continue;
                    }

                    String typeName = toStringValue(row, 1);
                    int jdbcType = jdbcTypeFromTypeName(typeName);
                    int columnSize = columnSizeForType(typeName, jdbcType);
                    Integer decimalDigits = decimalDigitsForType(typeName, jdbcType);
                    Integer numPrecRadix = numPrecRadixForType(jdbcType);
                    Integer charOctetLength = charOctetLengthForType(typeName, jdbcType, columnSize);

                    int ordinal = toIntValue(row[2], 0);
                    String nullableToken = toStringValue(row, 3);
                    boolean nullable = nullableToken == null
                        || "YES".equalsIgnoreCase(nullableToken)
                        || "TRUE".equalsIgnoreCase(nullableToken);
                    String nullableText = nullable ? "YES" : "NO";
                    int nullableFlag = nullable ? DatabaseMetaData.columnNullable : DatabaseMetaData.columnNoNulls;
                    String defaultValue = toStringValue(row, 4);
                    String autoIncrementFlag = isAutoIncrementDefaultExpression(defaultValue) ? "YES" : "NO";
                    String generatedFlag = isGeneratedColumnExpression(defaultValue) ? "YES" : "NO";

                    rows.add(new Object[]{
                        currentCatalog,
                        schemaName,
                        tableName,
                        columnName,
                        jdbcType,
                        typeName,
                        columnSize,
                        0,
                        decimalDigits,
                        numPrecRadix,
                        nullableFlag,
                        null,
                        defaultValue,
                        null,
                        null,
                        charOctetLength,
                        ordinal,
                        nullableText,
                        null,
                        null,
                        null,
                        null,
                        autoIncrementFlag,
                        generatedFlag
                    });
                }
            } catch (SQLException ignored) {
                // Leave empty on fallback failure; metadata shape remains valid.
            }
        }

        if (rows.isEmpty()) {
            appendLiveProbeColumns(rows, currentCatalog, schemaPattern, tableNamePattern, columnNamePattern);
        }

        List<SBColumnInfo> cols = new ArrayList<>();
        cols.add(column("TABLE_CAT", 25));
        cols.add(column("TABLE_SCHEM", 25));
        cols.add(column("TABLE_NAME", 25));
        cols.add(column("COLUMN_NAME", 25));
        cols.add(column("DATA_TYPE", 23));
        cols.add(column("TYPE_NAME", 25));
        cols.add(column("COLUMN_SIZE", 23));
        cols.add(column("BUFFER_LENGTH", 23));
        cols.add(column("DECIMAL_DIGITS", 23));
        cols.add(column("NUM_PREC_RADIX", 23));
        cols.add(column("NULLABLE", 23));
        cols.add(column("REMARKS", 25));
        cols.add(column("COLUMN_DEF", 25));
        cols.add(column("SQL_DATA_TYPE", 23));
        cols.add(column("SQL_DATETIME_SUB", 23));
        cols.add(column("CHAR_OCTET_LENGTH", 23));
        cols.add(column("ORDINAL_POSITION", 23));
        cols.add(column("IS_NULLABLE", 25));
        cols.add(column("SCOPE_CATALOG", 25));
        cols.add(column("SCOPE_SCHEMA", 25));
        cols.add(column("SCOPE_TABLE", 25));
        cols.add(column("SOURCE_DATA_TYPE", 21));
        cols.add(column("IS_AUTOINCREMENT", 25));
        cols.add(column("IS_GENERATEDCOLUMN", 25));
        return new SBResultSet(null, cols, rows);
    }

    @Override
    public ResultSet getColumnPrivileges(String catalog, String schema, String table,
            String columnNamePattern) throws SQLException {
        String currentCatalog = currentCatalogName();
        if (catalog != null && currentCatalog != null && !catalog.equalsIgnoreCase(currentCatalog)) {
            return emptyColumnPrivilegesResultSet();
        }

        List<Object[]> rows = new ArrayList<>();
        try {
            for (Object[] row : queryRows(
                "SELECT table_schema, table_name, column_name, grantor, grantee, privilege_type, is_grantable " +
                "FROM information_schema.column_privileges"
            )) {
                String schemaName = toStringValue(row, 0);
                String tableName = toStringValue(row, 1);
                String columnName = toStringValue(row, 2);
                if (!matchesPattern(schemaName, schema)
                        || !matchesPattern(tableName, table)
                        || !matchesPattern(columnName, columnNamePattern)) {
                    continue;
                }
                rows.add(new Object[]{
                    currentCatalog,
                    schemaName,
                    tableName,
                    columnName,
                    toStringValue(row, 3),
                    toStringValue(row, 4),
                    toStringValue(row, 5),
                    normalizeGrantableValue(toStringValue(row, 6))
                });
            }
        } catch (SQLException ex) {
            return emptyColumnPrivilegesResultSet();
        }

        List<SBColumnInfo> cols = new ArrayList<>();
        cols.add(column("TABLE_CAT", 25));
        cols.add(column("TABLE_SCHEM", 25));
        cols.add(column("TABLE_NAME", 25));
        cols.add(column("COLUMN_NAME", 25));
        cols.add(column("GRANTOR", 25));
        cols.add(column("GRANTEE", 25));
        cols.add(column("PRIVILEGE", 25));
        cols.add(column("IS_GRANTABLE", 25));
        return new SBResultSet(null, cols, rows);
    }

    @Override
    public ResultSet getTablePrivileges(String catalog, String schemaPattern, String tableNamePattern)
            throws SQLException {
        String currentCatalog = currentCatalogName();
        if (catalog != null && currentCatalog != null && !catalog.equalsIgnoreCase(currentCatalog)) {
            return emptyTablePrivilegesResultSet();
        }

        List<Object[]> rows = new ArrayList<>();
        try {
            for (Object[] row : queryRows(
                "SELECT table_schema, table_name, grantor, grantee, privilege_type, is_grantable " +
                "FROM information_schema.table_privileges"
            )) {
                String schemaName = toStringValue(row, 0);
                String tableName = toStringValue(row, 1);
                if (!matchesPattern(schemaName, schemaPattern)
                        || !matchesPattern(tableName, tableNamePattern)) {
                    continue;
                }
                rows.add(new Object[]{
                    currentCatalog,
                    schemaName,
                    tableName,
                    toStringValue(row, 2),
                    toStringValue(row, 3),
                    toStringValue(row, 4),
                    normalizeGrantableValue(toStringValue(row, 5))
                });
            }
        } catch (SQLException ex) {
            return emptyTablePrivilegesResultSet();
        }

        List<SBColumnInfo> cols = new ArrayList<>();
        cols.add(column("TABLE_CAT", 25));
        cols.add(column("TABLE_SCHEM", 25));
        cols.add(column("TABLE_NAME", 25));
        cols.add(column("GRANTOR", 25));
        cols.add(column("GRANTEE", 25));
        cols.add(column("PRIVILEGE", 25));
        cols.add(column("IS_GRANTABLE", 25));
        return new SBResultSet(null, cols, rows);
    }

    @Override
    public ResultSet getBestRowIdentifier(String catalog, String schema, String table,
            int scope, boolean nullable) throws SQLException {
        String currentCatalog = currentCatalogName();
        if (catalog != null && currentCatalog != null && !catalog.equalsIgnoreCase(currentCatalog)) {
            return createEmptyResultSet(
                new String[]{"SCOPE", "COLUMN_NAME", "DATA_TYPE", "TYPE_NAME", "COLUMN_SIZE",
                             "BUFFER_LENGTH", "DECIMAL_DIGITS", "PSEUDO_COLUMN"},
                new int[]{Types.SMALLINT, Types.VARCHAR, Types.INTEGER, Types.VARCHAR, Types.INTEGER,
                          Types.INTEGER, Types.SMALLINT, Types.SMALLINT}
            );
        }

        short rowScope = switch (scope) {
            case DatabaseMetaData.bestRowTemporary,
                 DatabaseMetaData.bestRowTransaction,
                 DatabaseMetaData.bestRowSession -> (short) scope;
            default -> DatabaseMetaData.bestRowSession;
        };

        List<Object[]> rows = new ArrayList<>();
        try {
            for (Object[] row : queryRows(
                "SELECT tc.table_schema, tc.table_name, kcu.column_name, c.data_type, " +
                "       c.character_maximum_length, c.numeric_precision, c.numeric_scale, c.is_nullable " +
                "FROM information_schema.table_constraints tc " +
                "JOIN information_schema.key_column_usage kcu " +
                "  ON tc.constraint_name = kcu.constraint_name " +
                " AND tc.table_schema = kcu.table_schema " +
                " AND tc.table_name = kcu.table_name " +
                "LEFT JOIN information_schema.columns c " +
                "  ON c.table_schema = tc.table_schema " +
                " AND c.table_name = tc.table_name " +
                " AND c.column_name = kcu.column_name " +
                "WHERE tc.constraint_type = 'PRIMARY KEY' " +
                "ORDER BY tc.table_schema, tc.table_name, kcu.ordinal_position"
            )) {
                String schemaName = toStringValue(row, 0);
                String tableName = toStringValue(row, 1);
                if (!matchesPattern(schemaName, schema) || !matchesPattern(tableName, table)) {
                    continue;
                }

                String nullableText = toStringValue(row, 7);
                boolean isNullable = nullableText != null && nullableText.equalsIgnoreCase("YES");
                if (!nullable && isNullable) {
                    continue;
                }

                String columnName = toStringValue(row, 2);
                String typeName = normalizeTypeName(toStringValue(row, 3));
                int jdbcType = jdbcTypeFromTypeName(typeName);
                int columnSize = toIntValue(row[4], 0);
                if (columnSize <= 0) {
                    columnSize = toIntValue(row[5], 0);
                }
                if (columnSize <= 0) {
                    columnSize = columnSizeForType(typeName, jdbcType);
                }
                short decimalDigits = toShortValue(row[6]);
                if (decimalDigits < 0) {
                    decimalDigits = 0;
                }
                Integer defaultScale = decimalDigitsForType(typeName, jdbcType);
                if (decimalDigits == 0 && defaultScale != null) {
                    decimalDigits = defaultScale.shortValue();
                }

                rows.add(new Object[]{
                    rowScope,
                    columnName,
                    jdbcType,
                    typeName,
                    columnSize,
                    0,
                    decimalDigits,
                    DatabaseMetaData.bestRowNotPseudo
                });
            }
        } catch (SQLException ex) {
            return createEmptyResultSet(
                new String[]{"SCOPE", "COLUMN_NAME", "DATA_TYPE", "TYPE_NAME", "COLUMN_SIZE",
                             "BUFFER_LENGTH", "DECIMAL_DIGITS", "PSEUDO_COLUMN"},
                new int[]{Types.SMALLINT, Types.VARCHAR, Types.INTEGER, Types.VARCHAR, Types.INTEGER,
                          Types.INTEGER, Types.SMALLINT, Types.SMALLINT}
            );
        }

        List<SBColumnInfo> cols = new ArrayList<>();
        cols.add(column("SCOPE", 21));
        cols.add(column("COLUMN_NAME", 25));
        cols.add(column("DATA_TYPE", 23));
        cols.add(column("TYPE_NAME", 25));
        cols.add(column("COLUMN_SIZE", 23));
        cols.add(column("BUFFER_LENGTH", 23));
        cols.add(column("DECIMAL_DIGITS", 21));
        cols.add(column("PSEUDO_COLUMN", 21));
        return new SBResultSet(null, cols, rows);
    }

    @Override
    public ResultSet getVersionColumns(String catalog, String schema, String table) throws SQLException {
        String currentCatalog = currentCatalogName();
        if (catalog != null && currentCatalog != null && !catalog.equalsIgnoreCase(currentCatalog)) {
            return emptyVersionColumnsResultSet();
        }
        if (table == null || table.isBlank()) {
            return emptyVersionColumnsResultSet();
        }

        List<Object[]> rows = new ArrayList<>();
        try {
            for (Object[] row : queryRows(
                "SELECT ns.nspname, c.relname, a.atttypid " +
                "FROM pg_catalog.pg_class c " +
                "JOIN pg_catalog.pg_namespace ns ON ns.oid = c.relnamespace " +
                "JOIN pg_catalog.pg_attribute a ON a.attrelid = c.oid " +
                "WHERE c.relkind IN ('r','p') " +
                "  AND ns.nspname NOT IN ('pg_catalog','pg_toast','information_schema') " +
                "  AND a.attnum < 0 " +
                "  AND a.attname = 'xmin' " +
                "ORDER BY ns.nspname, c.relname"
            )) {
                String schemaName = toStringValue(row, 0);
                String tableName = toStringValue(row, 1);
                if (!matchesPattern(schemaName, schema) || !matchesPattern(tableName, table)) {
                    continue;
                }

                Integer typeOid = parseOid(row[2]);
                int dataType = typeOid != null ? jdbcTypeFromOid(typeOid) : Types.BIGINT;
                String typeName = typeOid != null ? typeNameFromOid(typeOid) : "xid";
                int columnSize = columnSizeForType(typeName, dataType);
                short decimalDigits = 0;

                rows.add(new Object[]{
                    DatabaseMetaData.versionColumnUnknown,
                    "xmin",
                    dataType,
                    typeName,
                    columnSize,
                    0,
                    decimalDigits,
                    DatabaseMetaData.versionColumnPseudo
                });
            }
        } catch (SQLException ex) {
            return emptyVersionColumnsResultSet();
        }

        List<SBColumnInfo> cols = new ArrayList<>();
        cols.add(column("SCOPE", 21));
        cols.add(column("COLUMN_NAME", 25));
        cols.add(column("DATA_TYPE", 23));
        cols.add(column("TYPE_NAME", 25));
        cols.add(column("COLUMN_SIZE", 23));
        cols.add(column("BUFFER_LENGTH", 23));
        cols.add(column("DECIMAL_DIGITS", 21));
        cols.add(column("PSEUDO_COLUMN", 21));
        return new SBResultSet(null, cols, rows);
    }

    @Override
    public ResultSet getPrimaryKeys(String catalog, String schema, String table) throws SQLException {
        String currentCatalog = currentCatalogName();
        List<Object[]> source;
        try {
            source = queryRows(
                "SELECT tc.table_schema, tc.table_name, kcu.column_name, kcu.ordinal_position, tc.constraint_name " +
                    "FROM information_schema.table_constraints tc " +
                    "JOIN information_schema.key_column_usage kcu " +
                    "  ON tc.constraint_name = kcu.constraint_name " +
                    " AND tc.table_schema = kcu.table_schema " +
                    " AND tc.table_name = kcu.table_name " +
                    "WHERE tc.constraint_type = 'PRIMARY KEY'"
            );
        } catch (SQLException e) {
            source = Collections.emptyList();
        }

        List<Object[]> rows = new ArrayList<>();
        for (Object[] row : source) {
            String schemaName = toStringValue(row, 0);
            String tableName = toStringValue(row, 1);
            if (!matchesPattern(schemaName, schema) || !matchesPattern(tableName, table)) {
                continue;
            }
            String columnName = toStringValue(row, 2);
            short keySeq = toShortValue(row, 3);
            String pkName = toStringValue(row, 4);
            rows.add(new Object[]{currentCatalog, schemaName, tableName, columnName, keySeq, pkName});
        }

        List<SBColumnInfo> cols = new ArrayList<>();
        cols.add(column("TABLE_CAT", 25));
        cols.add(column("TABLE_SCHEM", 25));
        cols.add(column("TABLE_NAME", 25));
        cols.add(column("COLUMN_NAME", 25));
        cols.add(column("KEY_SEQ", 21));
        cols.add(column("PK_NAME", 25));
        return new SBResultSet(null, cols, rows);
    }

    @Override
    public ResultSet getImportedKeys(String catalog, String schema, String table) throws SQLException {
        String currentCatalog = currentCatalogName();
        if (catalog != null && currentCatalog != null && !catalog.equalsIgnoreCase(currentCatalog)) {
            return emptyForeignKeysResultSet();
        }

        List<Object[]> source = foreignKeyRows();
        List<Object[]> rows = new ArrayList<>();
        for (Object[] row : source) {
            String fkSchema = toStringValue(row, 0);
            String fkTable = toStringValue(row, 1);
            if (!matchesPattern(fkSchema, schema) || !matchesPattern(fkTable, table)) {
                continue;
            }
            String fkColumn = toStringValue(row, 2);
            String pkSchema = toStringValue(row, 3);
            String pkTable = toStringValue(row, 4);
            String pkColumn = toStringValue(row, 5);
            short keySeq = toShortValue(row, 6);
            short updateRule = mapRule(toStringValue(row, 7));
            short deleteRule = mapRule(toStringValue(row, 8));
            String fkName = toStringValue(row, 9);
            String pkName = toStringValue(row, 10);
            short deferrability = mapDeferrable(toStringValue(row, 11));

            rows.add(new Object[]{
                currentCatalog, pkSchema, pkTable, pkColumn,
                currentCatalog, fkSchema, fkTable, fkColumn,
                keySeq, updateRule, deleteRule, fkName, pkName, deferrability
            });
        }

        List<SBColumnInfo> cols = new ArrayList<>();
        cols.add(column("PKTABLE_CAT", 25));
        cols.add(column("PKTABLE_SCHEM", 25));
        cols.add(column("PKTABLE_NAME", 25));
        cols.add(column("PKCOLUMN_NAME", 25));
        cols.add(column("FKTABLE_CAT", 25));
        cols.add(column("FKTABLE_SCHEM", 25));
        cols.add(column("FKTABLE_NAME", 25));
        cols.add(column("FKCOLUMN_NAME", 25));
        cols.add(column("KEY_SEQ", 21));
        cols.add(column("UPDATE_RULE", 21));
        cols.add(column("DELETE_RULE", 21));
        cols.add(column("FK_NAME", 25));
        cols.add(column("PK_NAME", 25));
        cols.add(column("DEFERRABILITY", 21));
        return new SBResultSet(null, cols, rows);
    }

    @Override
    public ResultSet getExportedKeys(String catalog, String schema, String table) throws SQLException {
        String currentCatalog = currentCatalogName();
        if (catalog != null && currentCatalog != null && !catalog.equalsIgnoreCase(currentCatalog)) {
            return emptyForeignKeysResultSet();
        }

        List<Object[]> rows = new ArrayList<>();
        for (Object[] row : foreignKeyRows()) {
            String fkSchema = toStringValue(row, 0);
            String fkTable = toStringValue(row, 1);
            String fkColumn = toStringValue(row, 2);
            String pkSchema = toStringValue(row, 3);
            String pkTable = toStringValue(row, 4);
            if (!matchesPattern(pkSchema, schema) || !matchesPattern(pkTable, table)) {
                continue;
            }
            String pkColumn = toStringValue(row, 5);
            short keySeq = toShortValue(row, 6);
            short updateRule = mapRule(toStringValue(row, 7));
            short deleteRule = mapRule(toStringValue(row, 8));
            String fkName = toStringValue(row, 9);
            String pkName = toStringValue(row, 10);
            short deferrability = mapDeferrable(toStringValue(row, 11));

            rows.add(new Object[]{
                currentCatalog, pkSchema, pkTable, pkColumn,
                currentCatalog, fkSchema, fkTable, fkColumn,
                keySeq, updateRule, deleteRule, fkName, pkName, deferrability
            });
        }

        return foreignKeysResultSet(rows);
    }

    @Override
    public ResultSet getCrossReference(String parentCatalog, String parentSchema, String parentTable,
            String foreignCatalog, String foreignSchema, String foreignTable) throws SQLException {
        String currentCatalog = currentCatalogName();
        if ((parentCatalog != null && currentCatalog != null && !parentCatalog.equalsIgnoreCase(currentCatalog))
                || (foreignCatalog != null && currentCatalog != null && !foreignCatalog.equalsIgnoreCase(currentCatalog))) {
            return emptyForeignKeysResultSet();
        }

        List<Object[]> rows = new ArrayList<>();
        for (Object[] row : foreignKeyRows()) {
            String fkSchema = toStringValue(row, 0);
            String fkTable = toStringValue(row, 1);
            String fkColumn = toStringValue(row, 2);
            String pkSchema = toStringValue(row, 3);
            String pkTable = toStringValue(row, 4);
            if (!matchesPattern(pkSchema, parentSchema) || !matchesPattern(pkTable, parentTable)
                    || !matchesPattern(fkSchema, foreignSchema) || !matchesPattern(fkTable, foreignTable)) {
                continue;
            }
            String pkColumn = toStringValue(row, 5);
            short keySeq = toShortValue(row, 6);
            short updateRule = mapRule(toStringValue(row, 7));
            short deleteRule = mapRule(toStringValue(row, 8));
            String fkName = toStringValue(row, 9);
            String pkName = toStringValue(row, 10);
            short deferrability = mapDeferrable(toStringValue(row, 11));

            rows.add(new Object[]{
                currentCatalog, pkSchema, pkTable, pkColumn,
                currentCatalog, fkSchema, fkTable, fkColumn,
                keySeq, updateRule, deleteRule, fkName, pkName, deferrability
            });
        }

        return foreignKeysResultSet(rows);
    }

    @Override
    public ResultSet getTypeInfo() throws SQLException {
        List<Object[]> rows = new ArrayList<>();
        rows.add(typeInfoRow("BOOLEAN", Types.BOOLEAN, 1, null));
        rows.add(typeInfoRow("SMALLINT", Types.SMALLINT, 5, null));
        rows.add(typeInfoRow("INTEGER", Types.INTEGER, 10, null));
        rows.add(typeInfoRow("BIGINT", Types.BIGINT, 19, null));
        rows.add(typeInfoRow("REAL", Types.REAL, 24, null));
        rows.add(typeInfoRow("DOUBLE", Types.DOUBLE, 53, null));
        rows.add(typeInfoRow("NUMERIC", Types.NUMERIC, 38, "precision,scale"));
        rows.add(typeInfoRow("DECIMAL", Types.DECIMAL, 38, "precision,scale"));
        rows.add(typeInfoRow("CHAR", Types.CHAR, 255, "length"));
        rows.add(typeInfoRow("VARCHAR", Types.VARCHAR, 65535, "length"));
        rows.add(typeInfoRow("TEXT", Types.LONGVARCHAR, 2147483647, null));
        rows.add(typeInfoRow("BYTEA", Types.BINARY, 2147483647, null));
        rows.add(typeInfoRow("DATE", Types.DATE, 10, null));
        rows.add(typeInfoRow("TIME", Types.TIME, 15, null));
        rows.add(typeInfoRow("TIMESTAMP", Types.TIMESTAMP, 29, null));
        rows.add(typeInfoRow("TIMESTAMPTZ", Types.TIMESTAMP_WITH_TIMEZONE, 35, null));
        rows.add(typeInfoRow("UUID", Types.OTHER, 16, null));
        rows.add(typeInfoRow("JSON", Types.OTHER, 2147483647, null));
        rows.add(typeInfoRow("JSONB", Types.OTHER, 2147483647, null));

        List<SBColumnInfo> cols = new ArrayList<>();
        cols.add(column("TYPE_NAME", 25));
        cols.add(column("DATA_TYPE", 23));
        cols.add(column("PRECISION", 23));
        cols.add(column("LITERAL_PREFIX", 25));
        cols.add(column("LITERAL_SUFFIX", 25));
        cols.add(column("CREATE_PARAMS", 25));
        cols.add(column("NULLABLE", 21));
        cols.add(column("CASE_SENSITIVE", 16));
        cols.add(column("SEARCHABLE", 21));
        cols.add(column("UNSIGNED_ATTRIBUTE", 16));
        cols.add(column("FIXED_PREC_SCALE", 16));
        cols.add(column("AUTO_INCREMENT", 16));
        cols.add(column("LOCAL_TYPE_NAME", 25));
        cols.add(column("MINIMUM_SCALE", 21));
        cols.add(column("MAXIMUM_SCALE", 21));
        cols.add(column("SQL_DATA_TYPE", 23));
        cols.add(column("SQL_DATETIME_SUB", 23));
        cols.add(column("NUM_PREC_RADIX", 23));
        return new SBResultSet(null, cols, rows);
    }

    @Override
    public ResultSet getIndexInfo(String catalog, String schema, String table, boolean unique,
            boolean approximate) throws SQLException {
        String currentCatalog = currentCatalogName();
        if (catalog != null && currentCatalog != null && !catalog.equalsIgnoreCase(currentCatalog)) {
            return createEmptyResultSet(
                new String[]{"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "NON_UNIQUE", "INDEX_QUALIFIER",
                             "INDEX_NAME", "TYPE", "ORDINAL_POSITION", "COLUMN_NAME", "ASC_OR_DESC",
                             "CARDINALITY", "PAGES", "FILTER_CONDITION"},
                new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.BOOLEAN, Types.VARCHAR,
                          Types.VARCHAR, Types.SMALLINT, Types.SMALLINT, Types.VARCHAR, Types.VARCHAR,
                          Types.BIGINT, Types.BIGINT, Types.VARCHAR}
            );
        }

        List<Object[]> rows = new ArrayList<>();
        Iterable<Object[]> resultRows;
        try {
            resultRows = queryRows(
                "SELECT i.index_name, i.index_type, i.is_unique, " +
                "t.table_name, s.schema_name, ic.ordinal_position, c.column_name, " +
                "ic.is_descending, i.estimated_cardinality, i.estimated_pages, i.filter_condition " +
                "FROM sys.indexes i " +
                "JOIN sys.tables t ON t.table_id = i.table_id " +
                "JOIN sys.schemas s ON s.schema_id = t.schema_id " +
                "LEFT JOIN sys.index_columns ic ON ic.index_id = i.index_id " +
                "LEFT JOIN sys.columns c ON c.column_id = ic.column_id " +
                "WHERE i.is_valid = 1 AND t.is_valid = 1 AND s.is_valid = 1 " +
                "ORDER BY i.index_name, ic.ordinal_position"
            );
        } catch (SQLException ex) {
            resultRows = queryRows(
                "SELECT i.index_name, i.index_type, i.is_unique, " +
                "t.table_name, s.schema_name " +
                "FROM sys.indexes i " +
                "JOIN sys.tables t ON t.table_id = i.table_id " +
                "JOIN sys.schemas s ON s.schema_id = t.schema_id " +
                "WHERE i.is_valid = 1 AND t.is_valid = 1 AND s.is_valid = 1"
            );
        }

        for (Object[] row : resultRows) {
            String indexName = toStringValue(row, 0);
            String tableName = toStringValue(row, 3);
            String schemaName = toStringValue(row, 4);
            if (!matchesPattern(schemaName, schema) || !matchesPattern(tableName, table)) {
                continue;
            }
            boolean isUnique = toBooleanValue(row[2]);
            if (unique && !isUnique) {
                continue;
            }

            Short ordinal = row.length > 5 ? toShortValue(row, 5) : 0;
            String columnName = row.length > 6 ? toStringValue(row, 6) : null;
            String ascOrDesc = row.length > 7 ? normalizeIndexSortDirection(row[7]) : null;
            Long cardinality = row.length > 8 ? toLongValue(row[8], 0L) : 0L;
            Long pages = row.length > 9 ? toLongValue(row[9], 0L) : 0L;
            String filterCondition = row.length > 10 ? toStringValue(row, 10) : null;
            short indexType = mapIndexType(row[1]);

            if (!approximate) {
                // No exact-count lane available yet; keep conservative zeros when estimates are absent.
                if (cardinality == null) {
                    cardinality = 0L;
                }
                if (pages == null) {
                    pages = 0L;
                }
            }

            rows.add(new Object[]{
                currentCatalog,
                schemaName,
                tableName,
                !isUnique,
                null,
                indexName,
                indexType,
                ordinal,
                columnName,
                ascOrDesc,
                cardinality,
                pages,
                filterCondition
            });
        }

        List<SBColumnInfo> cols = new ArrayList<>();
        cols.add(column("TABLE_CAT", 25));
        cols.add(column("TABLE_SCHEM", 25));
        cols.add(column("TABLE_NAME", 25));
        cols.add(column("NON_UNIQUE", 16));
        cols.add(column("INDEX_QUALIFIER", 25));
        cols.add(column("INDEX_NAME", 25));
        cols.add(column("TYPE", 21));
        cols.add(column("ORDINAL_POSITION", 21));
        cols.add(column("COLUMN_NAME", 25));
        cols.add(column("ASC_OR_DESC", 25));
        cols.add(column("CARDINALITY", 20));
        cols.add(column("PAGES", 20));
        cols.add(column("FILTER_CONDITION", 25));
        return new SBResultSet(null, cols, rows);
    }

    protected List<Object[]> queryRows(String sql) throws SQLException {
        SBQueryResult result = connection.withResilience("metadata_query", sql, () ->
            connection.getProtocol().execute(sql), true
        );
        if (result == null || result.getRows() == null) {
            return Collections.emptyList();
        }
        return result.getRows();
    }

    protected String currentCatalogName() {
        if (connection == null || connection.getConnectionProperties() == null) {
            return null;
        }
        return connection.getConnectionProperties().getDatabase();
    }

    protected boolean expandSchemaParentNodesInMetadata() {
        if (connection == null || connection.getConnectionProperties() == null) {
            return false;
        }
        return connection.getConnectionProperties().isMetadataExpandSchemaParents();
    }

    private void appendSchemaWithParents(Set<String> out, String schemaName, String schemaPattern) {
        String[] segments = schemaName.split("\\.");
        StringBuilder current = new StringBuilder();
        for (String segment : segments) {
            if (segment == null || segment.isBlank()) {
                continue;
            }
            if (current.length() > 0) {
                current.append('.');
            }
            current.append(segment);
            String candidate = current.toString();
            if (matchesPattern(candidate, schemaPattern)) {
                out.add(candidate);
            }
        }
    }

    private void appendSchemaName(Set<String> out, String schemaName, String schemaPattern, boolean expandParents) {
        if (schemaName == null || schemaName.isBlank()) {
            return;
        }
        if (expandParents) {
            appendSchemaWithParents(out, schemaName, schemaPattern);
        } else if (matchesPattern(schemaName, schemaPattern)) {
            out.add(schemaName);
        }
    }

    private void appendSyntheticSystemSchemas(Set<String> out, String schemaPattern, boolean expandParents) {
        for (String schemaName : SYNTHETIC_SYSTEM_SCHEMAS) {
            appendSchemaName(out, schemaName, schemaPattern, expandParents);
        }
    }

    private List<Object[]> foreignKeyRows() {
        try {
            return queryRows(
                "SELECT tc.table_schema AS fk_schema, tc.table_name AS fk_table, " +
                    "kcu.column_name AS fk_column, ccu.table_schema AS pk_schema, " +
                    "ccu.table_name AS pk_table, ccu.column_name AS pk_column, " +
                    "kcu.ordinal_position AS key_seq, rc.update_rule, rc.delete_rule, " +
                    "tc.constraint_name AS fk_name, rc.unique_constraint_name AS pk_name, " +
                    "rc.deferrable AS deferrable " +
                    "FROM information_schema.table_constraints tc " +
                    "JOIN information_schema.key_column_usage kcu " +
                    "  ON tc.constraint_name = kcu.constraint_name " +
                    " AND tc.table_schema = kcu.table_schema " +
                    " AND tc.table_name = kcu.table_name " +
                    "JOIN information_schema.constraint_column_usage ccu " +
                    "  ON ccu.constraint_name = tc.constraint_name " +
                    " AND ccu.constraint_schema = tc.table_schema " +
                    "LEFT JOIN information_schema.referential_constraints rc " +
                    "  ON rc.constraint_name = tc.constraint_name " +
                    " AND rc.constraint_schema = tc.table_schema " +
                    "WHERE tc.constraint_type = 'FOREIGN KEY'"
            );
        } catch (SQLException e) {
            return Collections.emptyList();
        }
    }

    private ResultSet emptyForeignKeysResultSet() throws SQLException {
        return createEmptyResultSet(
            new String[]{"PKTABLE_CAT", "PKTABLE_SCHEM", "PKTABLE_NAME", "PKCOLUMN_NAME",
                         "FKTABLE_CAT", "FKTABLE_SCHEM", "FKTABLE_NAME", "FKCOLUMN_NAME",
                         "KEY_SEQ", "UPDATE_RULE", "DELETE_RULE", "FK_NAME", "PK_NAME",
                         "DEFERRABILITY"},
            new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR,
                      Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.SMALLINT,
                      Types.SMALLINT, Types.SMALLINT, Types.VARCHAR, Types.VARCHAR,
                      Types.SMALLINT}
        );
    }

    private ResultSet emptyTablePrivilegesResultSet() throws SQLException {
        return createEmptyResultSet(
            new String[]{"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "GRANTOR", "GRANTEE",
                         "PRIVILEGE", "IS_GRANTABLE"},
            new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR,
                      Types.VARCHAR, Types.VARCHAR}
        );
    }

    private ResultSet emptyColumnPrivilegesResultSet() throws SQLException {
        return createEmptyResultSet(
            new String[]{"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "COLUMN_NAME", "GRANTOR",
                         "GRANTEE", "PRIVILEGE", "IS_GRANTABLE"},
            new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR,
                      Types.VARCHAR, Types.VARCHAR, Types.VARCHAR}
        );
    }

    private ResultSet emptyVersionColumnsResultSet() throws SQLException {
        return createEmptyResultSet(
            new String[]{"SCOPE", "COLUMN_NAME", "DATA_TYPE", "TYPE_NAME", "COLUMN_SIZE",
                         "BUFFER_LENGTH", "DECIMAL_DIGITS", "PSEUDO_COLUMN"},
            new int[]{Types.SMALLINT, Types.VARCHAR, Types.INTEGER, Types.VARCHAR, Types.INTEGER,
                      Types.INTEGER, Types.SMALLINT, Types.SMALLINT}
        );
    }

    private ResultSet foreignKeysResultSet(List<Object[]> rows) throws SQLException {
        List<SBColumnInfo> cols = new ArrayList<>();
        cols.add(column("PKTABLE_CAT", 25));
        cols.add(column("PKTABLE_SCHEM", 25));
        cols.add(column("PKTABLE_NAME", 25));
        cols.add(column("PKCOLUMN_NAME", 25));
        cols.add(column("FKTABLE_CAT", 25));
        cols.add(column("FKTABLE_SCHEM", 25));
        cols.add(column("FKTABLE_NAME", 25));
        cols.add(column("FKCOLUMN_NAME", 25));
        cols.add(column("KEY_SEQ", 21));
        cols.add(column("UPDATE_RULE", 21));
        cols.add(column("DELETE_RULE", 21));
        cols.add(column("FK_NAME", 25));
        cols.add(column("PK_NAME", 25));
        cols.add(column("DEFERRABILITY", 21));
        return new SBResultSet(null, cols, rows);
    }

    private boolean matchesPattern(String value, String pattern) {
        if (pattern == null || pattern.isEmpty()) {
            return true;
        }
        if (value == null) {
            return false;
        }
        return Pattern.compile(patternToRegex(pattern), Pattern.CASE_INSENSITIVE).matcher(value).matches();
    }

    private String patternToRegex(String pattern) {
        StringBuilder out = new StringBuilder("^");
        boolean escaped = false;
        for (int i = 0; i < pattern.length(); i++) {
            char ch = pattern.charAt(i);
            if (escaped) {
                out.append(Pattern.quote(String.valueOf(ch)));
                escaped = false;
                continue;
            }
            if (ch == '\\') {
                escaped = true;
                continue;
            }
            if (ch == '%') {
                out.append(".*");
            } else if (ch == '_') {
                out.append('.');
            } else {
                out.append(Pattern.quote(String.valueOf(ch)));
            }
        }
        out.append('$');
        return out.toString();
    }

    private SBColumnInfo column(String name, int typeOid) {
        SBColumnInfo col = new SBColumnInfo();
        col.setName(name);
        col.setTypeOid(typeOid);
        return col;
    }

    private String toStringValue(Object[] row, int index) {
        if (row == null || index < 0 || index >= row.length) {
            return null;
        }
        return toStringValue(row[index]);
    }

    private String toStringValue(Object value) {
        if (value == null) {
            return null;
        }
        return value.toString();
    }

    private int toIntValue(Object value, int fallback) {
        if (value == null) {
            return fallback;
        }
        if (value instanceof Number) {
            return ((Number) value).intValue();
        }
        try {
            return Integer.parseInt(value.toString());
        } catch (NumberFormatException ex) {
            return fallback;
        }
    }

    private short toShortValue(Object[] row, int index) {
        if (row == null || index < 0 || index >= row.length) {
            return 0;
        }
        return toShortValue(row[index]);
    }

    private short toShortValue(Object value) {
        if (value == null) {
            return 0;
        }
        if (value instanceof Number) {
            return ((Number) value).shortValue();
        }
        try {
            return Short.parseShort(value.toString());
        } catch (NumberFormatException ex) {
            return 0;
        }
    }

    private boolean toBooleanValue(Object value) {
        if (value == null) {
            return false;
        }
        if (value instanceof Boolean) {
            return (Boolean) value;
        }
        if (value instanceof Number) {
            return ((Number) value).intValue() != 0;
        }
        String text = value.toString().trim();
        if ("1".equals(text)) {
            return true;
        }
        if ("0".equals(text)) {
            return false;
        }
        return Boolean.parseBoolean(text);
    }

    private long toLongValue(Object value, long fallback) {
        if (value == null) {
            return fallback;
        }
        if (value instanceof Number) {
            return ((Number) value).longValue();
        }
        try {
            return Long.parseLong(value.toString());
        } catch (NumberFormatException ex) {
            return fallback;
        }
    }

    private String normalizeGrantableValue(String value) {
        if (value == null || value.isBlank()) {
            return "NO";
        }
        String normalized = value.trim().toUpperCase(Locale.ROOT);
        if ("YES".equals(normalized) || "Y".equals(normalized) || "TRUE".equals(normalized) ||
                "1".equals(normalized)) {
            return "YES";
        }
        return "NO";
    }

    private String normalizeIndexSortDirection(Object rawValue) {
        if (rawValue == null) {
            return null;
        }
        if (rawValue instanceof Boolean) {
            return ((Boolean) rawValue) ? "D" : "A";
        }
        if (rawValue instanceof Number) {
            return ((Number) rawValue).intValue() != 0 ? "D" : "A";
        }
        String normalized = rawValue.toString().trim().toUpperCase(Locale.ROOT);
        if (normalized.isEmpty()) {
            return null;
        }
        if ("D".equals(normalized) || "DESC".equals(normalized) || "DESCENDING".equals(normalized) ||
                "TRUE".equals(normalized) || "Y".equals(normalized) || "1".equals(normalized)) {
            return "D";
        }
        if ("A".equals(normalized) || "ASC".equals(normalized) || "ASCENDING".equals(normalized) ||
                "FALSE".equals(normalized) || "N".equals(normalized) || "0".equals(normalized)) {
            return "A";
        }
        return null;
    }

    private short mapIndexType(Object rawType) {
        if (rawType instanceof Number) {
            int type = ((Number) rawType).intValue();
            return switch (type) {
                case 1 -> DatabaseMetaData.tableIndexClustered;
                case 2 -> DatabaseMetaData.tableIndexHashed;
                default -> DatabaseMetaData.tableIndexOther;
            };
        }
        if (rawType != null) {
            String normalized = rawType.toString().trim().toUpperCase(Locale.ROOT);
            if (normalized.contains("CLUSTER")) {
                return DatabaseMetaData.tableIndexClustered;
            }
            if (normalized.contains("HASH")) {
                return DatabaseMetaData.tableIndexHashed;
            }
        }
        return DatabaseMetaData.tableIndexOther;
    }

    private short mapRule(String rule) {
        if (rule == null) {
            return DatabaseMetaData.importedKeyNoAction;
        }
        String normalized = rule.trim().toUpperCase(Locale.ROOT);
        return switch (normalized) {
            case "CASCADE" -> DatabaseMetaData.importedKeyCascade;
            case "SET NULL" -> DatabaseMetaData.importedKeySetNull;
            case "SET DEFAULT" -> DatabaseMetaData.importedKeySetDefault;
            case "RESTRICT" -> DatabaseMetaData.importedKeyRestrict;
            case "NO ACTION" -> DatabaseMetaData.importedKeyNoAction;
            default -> DatabaseMetaData.importedKeyNoAction;
        };
    }

    private short mapDeferrable(String deferrable) {
        if (deferrable == null) {
            return DatabaseMetaData.importedKeyNotDeferrable;
        }
        String normalized = deferrable.trim().toUpperCase(Locale.ROOT);
        if (normalized.contains("DEFERRED")) {
            return DatabaseMetaData.importedKeyInitiallyDeferred;
        }
        if (normalized.contains("NOT")) {
            return DatabaseMetaData.importedKeyNotDeferrable;
        }
        return DatabaseMetaData.importedKeyInitiallyImmediate;
    }

    private String metadataKey(String schema, String name) {
        String safeSchema = schema == null ? "" : schema.toLowerCase(Locale.ROOT);
        String safeName = name == null ? "" : name.toLowerCase(Locale.ROOT);
        return safeSchema + "\u0000" + safeName;
    }

    private String normalizeTypeName(String typeName) {
        if (typeName == null) {
            return "text";
        }
        String normalized = typeName.trim();
        if (normalized.isEmpty()) {
            return "text";
        }
        int dot = normalized.indexOf('(');
        if (dot >= 0) {
            normalized = normalized.substring(0, dot).trim();
        }
        return normalized.toLowerCase(Locale.ROOT);
    }

    private short mapProcedureTypeFromDataType(String dataType) {
        if (dataType == null) {
            return DatabaseMetaData.procedureResultUnknown;
        }
        if ("void".equalsIgnoreCase(dataType.trim())) {
            return DatabaseMetaData.procedureNoResult;
        }
        return DatabaseMetaData.procedureReturnsResult;
    }

    private short mapProcedureColumnType(String mode) {
        if (mode == null) {
            return DatabaseMetaData.procedureColumnIn;
        }
        return switch (mode.toUpperCase(Locale.ROOT)) {
            case "OUT" -> DatabaseMetaData.procedureColumnOut;
            case "IN OUT", "INOUT" -> DatabaseMetaData.procedureColumnInOut;
            default -> DatabaseMetaData.procedureColumnIn;
        };
    }

    private short mapFunctionType(String dataType) {
        String normalized = dataType == null ? "" : dataType.toLowerCase(Locale.ROOT).trim();
        if ("table".equals(normalized)) {
            return DatabaseMetaData.functionReturnsTable;
        }
        return DatabaseMetaData.functionNoTable;
    }

    private short mapFunctionColumnType(String mode) {
        if (mode == null || mode.isBlank()) {
            return DatabaseMetaData.functionColumnIn;
        }
        return switch (mode.toUpperCase(Locale.ROOT)) {
            case "OUT" -> DatabaseMetaData.functionColumnOut;
            case "INOUT", "IN OUT" -> DatabaseMetaData.functionColumnInOut;
            default -> DatabaseMetaData.functionColumnIn;
        };
    }

    private Object[] typeInfoRow(String typeName, int dataType, int precision, String createParams) {
        String literalPrefix = null;
        String literalSuffix = null;
        boolean caseSensitive = false;
        if (dataType == Types.CHAR || dataType == Types.VARCHAR || dataType == Types.LONGVARCHAR) {
            literalPrefix = "'";
            literalSuffix = "'";
            caseSensitive = true;
        }
        short searchable = DatabaseMetaData.typeSearchable;
        boolean unsigned = false;
        boolean fixedScale = false;
        boolean autoIncrement = false;
        String localTypeName = typeName;
        Short minScale = null;
        Short maxScale = null;
        if (dataType == Types.NUMERIC || dataType == Types.DECIMAL) {
            minScale = 0;
            maxScale = 38;
        }
        Integer numPrecRadix = 10;
        return new Object[]{
            typeName,
            dataType,
            precision,
            literalPrefix,
            literalSuffix,
            createParams,
            DatabaseMetaData.typeNullable,
            caseSensitive,
            searchable,
            unsigned,
            fixedScale,
            autoIncrement,
            localTypeName,
            minScale,
            maxScale,
            null,
            null,
            numPrecRadix
        };
    }

    private Integer parseOid(Object value) {
        if (value == null) {
            return null;
        }
        if (value instanceof Number) {
            return ((Number) value).intValue();
        }
        try {
            String text = value.toString();
            if (text.matches("^[0-9]+$")) {
                return Integer.parseInt(text);
            }
        } catch (NumberFormatException ex) {
            return null;
        }
        return null;
    }

    private String typeNameFromOid(int oid) {
        switch (oid) {
            case SBTypeCodec.OID_BOOL:
                return "boolean";
            case SBTypeCodec.OID_CHAR:
                return "char";
            case SBTypeCodec.OID_INT2:
                return "int2";
            case SBTypeCodec.OID_INT4:
                return "int4";
            case SBTypeCodec.OID_INT8:
                return "int8";
            case SBTypeCodec.OID_FLOAT4:
                return "float4";
            case SBTypeCodec.OID_FLOAT8:
                return "float8";
            case SBTypeCodec.OID_NUMERIC:
                return "numeric";
            case SBTypeCodec.OID_MONEY:
                return "money";
            case SBTypeCodec.OID_TEXT:
                return "text";
            case SBTypeCodec.OID_VARCHAR:
                return "varchar";
            case SBTypeCodec.OID_BPCHAR:
                return "char";
            case SBTypeCodec.OID_DATE:
                return "date";
            case SBTypeCodec.OID_TIME:
                return "time";
            case SBTypeCodec.OID_TIMESTAMP:
                return "timestamp";
            case SBTypeCodec.OID_TIMESTAMPTZ:
                return "timestamptz";
            case SBTypeCodec.OID_INTERVAL:
                return "interval";
            case SBTypeCodec.OID_UUID:
                return "uuid";
            case SBTypeCodec.OID_JSON:
                return "json";
            case SBTypeCodec.OID_JSONB:
                return "jsonb";
            case SBTypeCodec.OID_XML:
                return "xml";
            case SBTypeCodec.OID_BYTEA:
                return "bytea";
            case SBTypeCodec.OID_INET:
                return "inet";
            case SBTypeCodec.OID_CIDR:
                return "cidr";
            case SBTypeCodec.OID_MACADDR:
                return "macaddr";
            case SBTypeCodec.OID_MACADDR8:
                return "macaddr8";
            case SBTypeCodec.OID_RECORD:
                return "record";
            case 28:
                return "xid";
            default:
                return "unknown";
        }
    }

    private int jdbcTypeFromOid(int oid) {
        switch (oid) {
            case SBTypeCodec.OID_BOOL:
                return Types.BOOLEAN;
            case SBTypeCodec.OID_CHAR:
                return Types.CHAR;
            case SBTypeCodec.OID_INT2:
                return Types.SMALLINT;
            case SBTypeCodec.OID_INT4:
                return Types.INTEGER;
            case SBTypeCodec.OID_INT8:
                return Types.BIGINT;
            case SBTypeCodec.OID_FLOAT4:
                return Types.REAL;
            case SBTypeCodec.OID_FLOAT8:
                return Types.DOUBLE;
            case SBTypeCodec.OID_NUMERIC:
            case SBTypeCodec.OID_MONEY:
                return Types.NUMERIC;
            case SBTypeCodec.OID_TEXT:
            case SBTypeCodec.OID_VARCHAR:
                return Types.VARCHAR;
            case SBTypeCodec.OID_BPCHAR:
                return Types.CHAR;
            case SBTypeCodec.OID_BYTEA:
                return Types.BINARY;
            case SBTypeCodec.OID_DATE:
                return Types.DATE;
            case SBTypeCodec.OID_TIME:
                return Types.TIME;
            case SBTypeCodec.OID_TIMESTAMP:
            case SBTypeCodec.OID_TIMESTAMPTZ:
                return Types.TIMESTAMP;
            case 28:
                return Types.BIGINT;
            case SBTypeCodec.OID_UUID:
            case SBTypeCodec.OID_JSON:
            case SBTypeCodec.OID_JSONB:
            case SBTypeCodec.OID_XML:
            case SBTypeCodec.OID_INET:
            case SBTypeCodec.OID_CIDR:
            case SBTypeCodec.OID_MACADDR:
            case SBTypeCodec.OID_MACADDR8:
            case SBTypeCodec.OID_RECORD:
                return Types.OTHER;
            default:
                return Types.OTHER;
        }
    }

    private int jdbcTypeFromTypeName(String typeName) {
        if (typeName == null) {
            return Types.OTHER;
        }
        String normalized = typeName.toLowerCase(Locale.ROOT);
        if (normalized.contains("int2") || normalized.contains("smallint")) {
            return Types.SMALLINT;
        }
        if (normalized.contains("int4") || normalized.equals("int") || normalized.contains("integer")) {
            return Types.INTEGER;
        }
        if (normalized.contains("int8") || normalized.contains("bigint")) {
            return Types.BIGINT;
        }
        if (normalized.contains("float4") || normalized.contains("real")) {
            return Types.REAL;
        }
        if (normalized.contains("float8") || normalized.contains("double")) {
            return Types.DOUBLE;
        }
        if (normalized.contains("numeric") || normalized.contains("decimal") || normalized.contains("money")) {
            return Types.NUMERIC;
        }
        if (normalized.contains("char") && !normalized.contains("varchar")) {
            return Types.CHAR;
        }
        if (normalized.contains("varchar") || normalized.contains("text")) {
            return Types.VARCHAR;
        }
        if (normalized.contains("timestamp")) {
            return Types.TIMESTAMP;
        }
        if (normalized.contains("date")) {
            return Types.DATE;
        }
        if (normalized.contains("time")) {
            return Types.TIME;
        }
        if (normalized.contains("bytea") || normalized.contains("blob")) {
            return Types.BINARY;
        }
        return Types.OTHER;
    }

    private String mapTableType(Object rawType, String schemaName, boolean fromView) {
        String schema = schemaName != null ? schemaName.toLowerCase(Locale.ROOT) : "";
        String type;
        if (fromView) {
            boolean materialized = toBooleanValue(rawType);
            type = materialized ? "MATERIALIZED VIEW" : "VIEW";
        } else if (rawType instanceof Number) {
            int code = ((Number) rawType).intValue();
            switch (code) {
                case 1:
                    type = "TABLE";
                    break;
                case 2:
                    type = "TEMPORARY TABLE";
                    break;
                case 3:
                    type = "FOREIGN TABLE";
                    break;
                case 4:
                    type = "MATERIALIZED VIEW";
                    break;
                case 5:
                    type = "SYSTEM TABLE";
                    break;
                case 0:
                default:
                    type = "TABLE";
                    break;
            }
        } else {
            String normalized = rawType != null ? rawType.toString().trim().toUpperCase(Locale.ROOT) : "";
            if (normalized.isEmpty() || "BASE TABLE".equals(normalized) || "TABLE".equals(normalized)
                || "HEAP".equals(normalized) || normalized.endsWith("TABLE")) {
                type = "TABLE";
            } else if ("VIEW".equals(normalized) || "MATERIALIZED VIEW".equals(normalized)) {
                type = normalized;
            } else if ("TEMP".equals(normalized) || "TEMPORARY".equals(normalized)
                || "TEMP TABLE".equals(normalized) || "TEMPORARY TABLE".equals(normalized)) {
                type = "TEMPORARY TABLE";
            } else if ("FOREIGN".equals(normalized) || "FOREIGN TABLE".equals(normalized)) {
                type = "FOREIGN TABLE";
            } else if ("SYSTEM".equals(normalized) || "SYSTEM TABLE".equals(normalized)) {
                type = "SYSTEM TABLE";
            } else {
                type = normalized;
            }
        }

        if ("sys".equals(schema)) {
            if ("VIEW".equals(type) || "MATERIALIZED VIEW".equals(type)) {
                return "SYSTEM VIEW";
            }
            if ("TABLE".equals(type)) {
                return "SYSTEM TABLE";
            }
        }
        return type;
    }

    private String mapInfoSchemaTableType(Object rawType, String schemaName) {
        String normalized = rawType == null
            ? ""
            : rawType.toString().trim().toUpperCase(Locale.ROOT);
        String mapped;
        if (normalized.contains("VIEW")) {
            mapped = "VIEW";
        } else if (normalized.contains("SYSTEM")) {
            mapped = "SYSTEM TABLE";
        } else {
            mapped = "TABLE";
        }
        if ("sys".equalsIgnoreCase(schemaName) && "VIEW".equals(mapped)) {
            return "SYSTEM VIEW";
        }
        return mapped;
    }

    private int columnSizeForType(String typeName, int jdbcType) {
        String normalized = normalizeTypeName(typeName);
        if (normalized.contains("xid")) {
            return 10;
        }
        return switch (jdbcType) {
            case Types.BOOLEAN, Types.BIT -> 1;
            case Types.SMALLINT -> 5;
            case Types.INTEGER -> 10;
            case Types.BIGINT -> 19;
            case Types.REAL -> 24;
            case Types.FLOAT, Types.DOUBLE -> 53;
            case Types.NUMERIC, Types.DECIMAL -> 38;
            case Types.DATE -> 10;
            case Types.TIME -> 15;
            case Types.TIMESTAMP -> 29;
            case Types.CHAR -> 255;
            case Types.VARCHAR -> normalized.contains("text") ? Integer.MAX_VALUE : 65535;
            case Types.LONGVARCHAR -> Integer.MAX_VALUE;
            case Types.BINARY, Types.VARBINARY, Types.LONGVARBINARY -> Integer.MAX_VALUE;
            default -> {
                if (normalized.contains("uuid")) {
                    yield 36;
                }
                yield 0;
            }
        };
    }

    private Integer decimalDigitsForType(String typeName, int jdbcType) {
        String normalized = normalizeTypeName(typeName);
        return switch (jdbcType) {
            case Types.SMALLINT, Types.INTEGER, Types.BIGINT -> 0;
            case Types.NUMERIC, Types.DECIMAL -> 0;
            case Types.REAL -> 6;
            case Types.FLOAT, Types.DOUBLE -> 15;
            default -> normalized.contains("numeric") || normalized.contains("decimal") ? 0 : null;
        };
    }

    private Integer numPrecRadixForType(int jdbcType) {
        return switch (jdbcType) {
            case Types.SMALLINT, Types.INTEGER, Types.BIGINT,
                 Types.NUMERIC, Types.DECIMAL, Types.REAL,
                 Types.FLOAT, Types.DOUBLE -> 10;
            default -> null;
        };
    }

    private Integer charOctetLengthForType(String typeName, int jdbcType, int columnSize) {
        String normalized = normalizeTypeName(typeName);
        return switch (jdbcType) {
            case Types.CHAR, Types.VARCHAR, Types.LONGVARCHAR -> columnSize;
            default -> (normalized.contains("char") || normalized.contains("text")) ? columnSize : null;
        };
    }

    private boolean isAutoIncrementDefaultExpression(String defaultValue) {
        if (defaultValue == null || defaultValue.isBlank()) {
            return false;
        }
        String normalized = defaultValue.toLowerCase(Locale.ROOT);
        return normalized.contains("nextval(")
            || normalized.contains("generated always as identity")
            || normalized.contains("generated by default as identity");
    }

    private boolean isGeneratedColumnExpression(String defaultValue) {
        if (defaultValue == null || defaultValue.isBlank()) {
            return false;
        }
        String normalized = defaultValue.toLowerCase(Locale.ROOT);
        return normalized.contains("generated always as")
            || normalized.contains("generated by default as");
    }

    private Set<String> normalizeTypes(String[] types) {
        if (types == null || types.length == 0) {
            return Collections.emptySet();
        }
        Set<String> normalized = new HashSet<>();
        for (String type : types) {
            if (type != null) {
                normalized.add(type.toUpperCase(Locale.ROOT));
            }
        }
        return normalized;
    }

    private boolean matchesTypeFilter(String tableType, Set<String> filter) {
        if (filter == null || filter.isEmpty()) {
            return true;
        }
        if (tableType == null) {
            return false;
        }
        return filter.contains(tableType.toUpperCase(Locale.ROOT));
    }

    private void appendSyntheticSystemViews(
        List<Object[]> rows,
        Set<String> existing,
        String currentCatalog,
        String schemaPattern,
        String tableNamePattern
    ) {
        for (Map.Entry<String, List<String>> entry : SYNTHETIC_SYSTEM_VIEWS.entrySet()) {
            String schemaName = entry.getKey();
            if (!matchesPattern(schemaName, schemaPattern)) {
                continue;
            }
            for (String tableName : entry.getValue()) {
                if (!matchesPattern(tableName, tableNamePattern)) {
                    continue;
                }
                String key = schemaName.toLowerCase(Locale.ROOT) + "." + tableName.toLowerCase(Locale.ROOT);
                if (!existing.add(key)) {
                    continue;
                }
                rows.add(new Object[]{
                    currentCatalog,
                    schemaName,
                    tableName,
                    "SYSTEM VIEW",
                    null,
                    null,
                    null,
                    null,
                    null,
                    null
                });
            }
        }
    }

    private void appendLiveProbeColumns(
        List<Object[]> rows,
        String currentCatalog,
        String schemaPattern,
        String tableNamePattern,
        String columnNamePattern
    ) throws SQLException {
        String schemaName = exactPatternValue(schemaPattern);
        String tableName = exactPatternValue(tableNamePattern);
        if (schemaName == null || tableName == null) {
            return;
        }

        for (ProbeColumnMetadata column : loadProbeColumnMetadata(schemaName, tableName)) {
            if (!matchesPattern(column.columnName, columnNamePattern)) {
                continue;
            }

            int nullableFlag = column.nullable == ResultSetMetaData.columnNullableUnknown
                ? DatabaseMetaData.columnNullable
                : column.nullable;
            String nullableText = nullableFlag == DatabaseMetaData.columnNoNulls ? "NO" : "YES";
            int columnSize = column.precision > 0 ? column.precision : columnSizeForType(column.typeName, column.jdbcType);
            Integer decimalDigits = column.scale >= 0 ? column.scale : decimalDigitsForType(column.typeName, column.jdbcType);
            Integer numPrecRadix = numPrecRadixForType(column.jdbcType);
            Integer charOctetLength = charOctetLengthForType(column.typeName, column.jdbcType, columnSize);

            rows.add(new Object[]{
                currentCatalog,
                schemaName,
                tableName,
                column.columnName,
                column.jdbcType,
                column.typeName,
                columnSize,
                0,
                decimalDigits,
                numPrecRadix,
                nullableFlag,
                null,
                null,
                null,
                null,
                charOctetLength,
                column.ordinalPosition,
                nullableText,
                null,
                null,
                null,
                null,
                column.autoIncrement ? "YES" : "NO",
                "NO"
            });
        }
    }

    protected List<ProbeColumnMetadata> loadProbeColumnMetadata(String schemaName, String tableName) throws SQLException {
        if (connection == null) {
            return Collections.emptyList();
        }
        String qualifiedName = qualifyMetadataProbeTarget(schemaName, tableName);
        try (Statement statement = connection.createStatement();
             ResultSet rs = statement.executeQuery("SELECT * FROM " + qualifiedName + " LIMIT 0")) {
            ResultSetMetaData metadata = rs.getMetaData();
            List<ProbeColumnMetadata> columns = new ArrayList<>();
            for (int index = 1; index <= metadata.getColumnCount(); index++) {
                columns.add(new ProbeColumnMetadata(
                    metadata.getColumnName(index),
                    metadata.getColumnType(index),
                    metadata.getColumnTypeName(index),
                    metadata.getPrecision(index),
                    metadata.getScale(index),
                    metadata.isNullable(index),
                    metadata.isAutoIncrement(index),
                    index));
            }
            return columns;
        } catch (SQLException ignored) {
            return Collections.emptyList();
        }
    }

    private static String qualifyMetadataProbeTarget(String schemaName, String tableName) {
        StringBuilder out = new StringBuilder();
        if (schemaName != null && !schemaName.isBlank()) {
            for (String segment : schemaName.split("\\.")) {
                if (segment.isBlank()) {
                    continue;
                }
                if (out.length() > 0) {
                    out.append('.');
                }
                out.append(quoteIdentifier(segment));
            }
        }
        if (out.length() > 0) {
            out.append('.');
        }
        out.append(quoteIdentifier(tableName));
        return out.toString();
    }

    private static String quoteIdentifier(String identifier) {
        if (identifier == null) {
            return "\"\"";
        }
        return "\"" + identifier.replace("\"", "\"\"") + "\"";
    }

    private static String exactPatternValue(String pattern) {
        if (pattern == null || pattern.indexOf('%') >= 0) {
            return null;
        }
        return pattern;
    }

    private List<String> monitoringViews() {
        return Arrays.asList(
            "sessions",
            "context_variables",
            "transactions",
            "locks",
            "statements",
            "io_stats",
            "performance",
            "jobs",
            "job_runs",
            "job_dependencies"
        );
    }

    protected static final class ProbeColumnMetadata {
        final String columnName;
        final int jdbcType;
        final String typeName;
        final int precision;
        final int scale;
        final int nullable;
        final boolean autoIncrement;
        final int ordinalPosition;

        ProbeColumnMetadata(
            String columnName,
            int jdbcType,
            String typeName,
            int precision,
            int scale,
            int nullable,
            boolean autoIncrement,
            int ordinalPosition
        ) {
            this.columnName = columnName;
            this.jdbcType = jdbcType;
            this.typeName = typeName;
            this.precision = precision;
            this.scale = scale;
            this.nullable = nullable;
            this.autoIncrement = autoIncrement;
            this.ordinalPosition = ordinalPosition;
        }
    }

    @Override
    public boolean supportsResultSetType(int type) throws SQLException {
        return type == ResultSet.TYPE_FORWARD_ONLY ||
               type == ResultSet.TYPE_SCROLL_INSENSITIVE ||
               type == ResultSet.TYPE_SCROLL_SENSITIVE;
    }

    @Override
    public boolean supportsResultSetConcurrency(int type, int concurrency) throws SQLException {
        if (!supportsResultSetType(type)) {
            return false;
        }
        return concurrency == ResultSet.CONCUR_READ_ONLY
            || concurrency == ResultSet.CONCUR_UPDATABLE;
    }

    @Override
    public boolean ownUpdatesAreVisible(int type) throws SQLException {
        return supportsResultSetType(type);
    }

    @Override
    public boolean ownDeletesAreVisible(int type) throws SQLException {
        return supportsResultSetType(type);
    }

    @Override
    public boolean ownInsertsAreVisible(int type) throws SQLException {
        return supportsResultSetType(type);
    }

    @Override
    public boolean othersUpdatesAreVisible(int type) throws SQLException {
        return type == ResultSet.TYPE_SCROLL_SENSITIVE;
    }

    @Override
    public boolean othersDeletesAreVisible(int type) throws SQLException {
        return type == ResultSet.TYPE_SCROLL_SENSITIVE;
    }

    @Override
    public boolean othersInsertsAreVisible(int type) throws SQLException {
        return type == ResultSet.TYPE_SCROLL_SENSITIVE;
    }

    @Override
    public boolean updatesAreDetected(int type) throws SQLException {
        return supportsResultSetType(type);
    }

    @Override
    public boolean deletesAreDetected(int type) throws SQLException {
        return supportsResultSetType(type);
    }

    @Override
    public boolean insertsAreDetected(int type) throws SQLException {
        return supportsResultSetType(type);
    }

    @Override
    public boolean supportsBatchUpdates() throws SQLException {
        return true;
    }

    @Override
    public ResultSet getUDTs(String catalog, String schemaPattern, String typeNamePattern, int[] types)
            throws SQLException {
        String currentCatalog = connection.getConnectionProperties().getDatabase();
        if (catalog != null && currentCatalog != null && !catalog.equalsIgnoreCase(currentCatalog)) {
            return createEmptyResultSet(
                new String[]{"TYPE_CAT", "TYPE_SCHEM", "TYPE_NAME", "CLASS_NAME", "DATA_TYPE",
                             "REMARKS", "BASE_TYPE"},
                new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.INTEGER,
                          Types.VARCHAR, Types.SMALLINT}
            );
        }

        List<Object[]> rows = new ArrayList<>();
        try {
            for (Object[] row : queryRows(
                "SELECT USER_DEFINED_TYPE_SCHEMA, USER_DEFINED_TYPE_NAME, DATA_TYPE " +
                "FROM information_schema.user_defined_types"
            )) {
                String schemaName = toStringValue(row, 0);
                String typeName = toStringValue(row, 1);
                if (!matchesPattern(schemaName, schemaPattern)
                        || !matchesPattern(typeName, typeNamePattern)) {
                    continue;
                }

                String dataTypeName = toStringValue(row, 2);
                int dataType = jdbcTypeFromTypeName(dataTypeName);

                rows.add(new Object[]{
                    currentCatalog,
                    schemaName,
                    typeName,
                    typeName,
                    dataType,
                    null,
                    dataType
                });
            }
        } catch (SQLException ex) {
            return createEmptyResultSet(
                new String[]{"TYPE_CAT", "TYPE_SCHEM", "TYPE_NAME", "CLASS_NAME", "DATA_TYPE",
                             "REMARKS", "BASE_TYPE"},
                new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.INTEGER,
                          Types.VARCHAR, Types.SMALLINT}
            );
        }

        List<SBColumnInfo> cols = new ArrayList<>();
        cols.add(column("TYPE_CAT", 25));
        cols.add(column("TYPE_SCHEM", 25));
        cols.add(column("TYPE_NAME", 25));
        cols.add(column("CLASS_NAME", 25));
        cols.add(column("DATA_TYPE", 23));
        cols.add(column("REMARKS", 25));
        cols.add(column("BASE_TYPE", 21));
        return new SBResultSet(null, cols, rows);
    }

    @Override
    public Connection getConnection() throws SQLException {
        return connection;
    }

    @Override
    public boolean supportsSavepoints() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsNamedParameters() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsMultipleOpenResults() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsGetGeneratedKeys() throws SQLException {
        return true;
    }

    @Override
    public ResultSet getSuperTypes(String catalog, String schemaPattern, String typeNamePattern)
            throws SQLException {
        String currentCatalog = connection.getConnectionProperties().getDatabase();
        if (catalog != null && currentCatalog != null && !catalog.equalsIgnoreCase(currentCatalog)) {
            return createEmptyResultSet(
                new String[]{"TYPE_CAT", "TYPE_SCHEM", "TYPE_NAME", "SUPERTYPE_CAT", "SUPERTYPE_SCHEM",
                             "SUPERTYPE_NAME"},
                new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR,
                          Types.VARCHAR}
            );
        }

        List<Object[]> rows = new ArrayList<>();
        try {
            for (Object[] row : queryRows(
                "SELECT n.nspname, t.typname, sn.nspname, st.typname " +
                "FROM pg_catalog.pg_type t " +
                "JOIN pg_catalog.pg_namespace n ON n.oid = t.typnamespace " +
                "LEFT JOIN pg_catalog.pg_type st ON st.oid = t.typbasetype " +
                "LEFT JOIN pg_catalog.pg_namespace sn ON sn.oid = st.typnamespace " +
                "WHERE t.typbasetype IS NOT NULL " +
                "  AND t.typbasetype <> 0"
            )) {
                String schemaName = toStringValue(row, 0);
                String typeName = toStringValue(row, 1);
                String superSchemaName = toStringValue(row, 2);
                String superTypeName = toStringValue(row, 3);
                if (!matchesPattern(schemaName, schemaPattern)
                        || !matchesPattern(typeName, typeNamePattern)) {
                    continue;
                }

                rows.add(new Object[]{
                    currentCatalog,
                    schemaName,
                    typeName,
                    currentCatalog,
                    superSchemaName,
                    superTypeName
                });
            }
        } catch (SQLException ex) {
            return createEmptyResultSet(
                new String[]{"TYPE_CAT", "TYPE_SCHEM", "TYPE_NAME", "SUPERTYPE_CAT", "SUPERTYPE_SCHEM",
                             "SUPERTYPE_NAME"},
                new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR,
                          Types.VARCHAR}
            );
        }

        List<SBColumnInfo> cols = new ArrayList<>();
        cols.add(column("TYPE_CAT", 25));
        cols.add(column("TYPE_SCHEM", 25));
        cols.add(column("TYPE_NAME", 25));
        cols.add(column("SUPERTYPE_CAT", 25));
        cols.add(column("SUPERTYPE_SCHEM", 25));
        cols.add(column("SUPERTYPE_NAME", 25));
        return new SBResultSet(null, cols, rows);
    }

    @Override
    public ResultSet getSuperTables(String catalog, String schemaPattern, String tableNamePattern)
            throws SQLException {
        String currentCatalog = connection.getConnectionProperties().getDatabase();
        if (catalog != null && currentCatalog != null && !catalog.equalsIgnoreCase(currentCatalog)) {
            return createEmptyResultSet(
                new String[]{"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "SUPERTABLE_NAME"},
                new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR}
            );
        }

        List<Object[]> rows = new ArrayList<>();
        try {
            for (Object[] row : queryRows(
                "SELECT cns.nspname, c.relname, pns.nspname, prs.relname " +
                "FROM pg_catalog.pg_inherits i " +
                "JOIN pg_catalog.pg_class c ON c.oid = i.inhrelid " +
                "JOIN pg_catalog.pg_class prs ON prs.oid = i.inhparent " +
                "JOIN pg_catalog.pg_namespace cns ON cns.oid = c.relnamespace " +
                "JOIN pg_catalog.pg_namespace pns ON pns.oid = prs.relnamespace " +
                "WHERE c.relkind IN ('r', 'v', 'm', 'f') " +
                "ORDER BY cns.nspname, c.relname, pns.nspname, prs.relname"
            )) {
                String childSchemaName = toStringValue(row, 0);
                String childTableName = toStringValue(row, 1);
                String superTableName = toStringValue(row, 3);
                if (!matchesPattern(childSchemaName, schemaPattern)
                        || !matchesPattern(childTableName, tableNamePattern)) {
                    continue;
                }

                rows.add(new Object[]{
                    currentCatalog,
                    childSchemaName,
                    childTableName,
                    superTableName
                });
            }
        } catch (SQLException ex) {
            return createEmptyResultSet(
                new String[]{"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "SUPERTABLE_NAME"},
                new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR}
            );
        }

        List<SBColumnInfo> cols = new ArrayList<>();
        cols.add(column("TABLE_CAT", 25));
        cols.add(column("TABLE_SCHEM", 25));
        cols.add(column("TABLE_NAME", 25));
        cols.add(column("SUPERTABLE_NAME", 25));
        return new SBResultSet(null, cols, rows);
    }

    @Override
    public ResultSet getAttributes(String catalog, String schemaPattern, String typeNamePattern,
            String attributeNamePattern) throws SQLException {
        String currentCatalog = connection.getConnectionProperties().getDatabase();
        if (catalog != null && currentCatalog != null && !catalog.equalsIgnoreCase(currentCatalog)) {
            return createEmptyResultSet(
                new String[]{"TYPE_CAT", "TYPE_SCHEM", "TYPE_NAME", "ATTR_NAME", "DATA_TYPE",
                             "ATTR_TYPE_NAME", "ATTR_SIZE", "DECIMAL_DIGITS", "NUM_PREC_RADIX", "NULLABLE",
                             "REMARKS", "ATTR_DEF", "SQL_DATA_TYPE", "SQL_DATETIME_SUB", "CHAR_OCTET_LENGTH",
                             "ORDINAL_POSITION", "IS_NULLABLE", "SCOPE_CATALOG", "SCOPE_SCHEMA", "SCOPE_TABLE",
                             "SOURCE_DATA_TYPE"},
                new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.INTEGER,
                          Types.VARCHAR, Types.INTEGER, Types.INTEGER, Types.INTEGER, Types.INTEGER,
                          Types.VARCHAR, Types.VARCHAR, Types.INTEGER, Types.INTEGER, Types.INTEGER,
                          Types.INTEGER, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR,
                          Types.SMALLINT}
            );
        }

        List<Object[]> rows = new ArrayList<>();
        try {
            for (Object[] row : queryRows(
                "SELECT ns.nspname, t.typname, a.attname, a.atttypid, a.attnotnull, a.attnum " +
                "FROM pg_catalog.pg_attribute a " +
                "JOIN pg_catalog.pg_type t ON t.oid = a.attrelid " +
                "JOIN pg_catalog.pg_namespace ns ON ns.oid = t.typnamespace " +
                "WHERE t.typtype = 'c' " +
                "  AND a.attnum > 0 " +
                "  AND NOT a.attisdropped " +
                "ORDER BY ns.nspname, t.typname, a.attnum"
            )) {
                String schemaName = toStringValue(row, 0);
                String typeName = toStringValue(row, 1);
                String attributeName = toStringValue(row, 2);
                if (!matchesPattern(schemaName, schemaPattern)
                        || !matchesPattern(typeName, typeNamePattern)
                        || !matchesPattern(attributeName, attributeNamePattern)) {
                    continue;
                }

                Integer typeOid = parseOid(row[3]);
                int dataType = typeOid != null ? jdbcTypeFromOid(typeOid) : Types.OTHER;
                String attrTypeName = typeOid != null ? typeNameFromOid(typeOid) : null;
                boolean nullable = toBooleanValue(row[4]);
                int ordinalPosition = toIntValue(row[5], 0);
                int nullableValue = nullable ? DatabaseMetaData.attributeNullable : DatabaseMetaData.attributeNoNulls;

                rows.add(new Object[]{
                    currentCatalog,
                    schemaName,
                    typeName,
                    attributeName,
                    dataType,
                    attrTypeName,
                    0,
                    0,
                    10,
                    nullableValue,
                    null,
                    null,
                    null,
                    null,
                    0,
                    ordinalPosition,
                    nullable ? "YES" : "NO",
                    null,
                    null,
                    null,
                    null
                });
            }
        } catch (SQLException ex) {
            return createEmptyResultSet(
                new String[]{"TYPE_CAT", "TYPE_SCHEM", "TYPE_NAME", "ATTR_NAME", "DATA_TYPE",
                             "ATTR_TYPE_NAME", "ATTR_SIZE", "DECIMAL_DIGITS", "NUM_PREC_RADIX", "NULLABLE",
                             "REMARKS", "ATTR_DEF", "SQL_DATA_TYPE", "SQL_DATETIME_SUB", "CHAR_OCTET_LENGTH",
                             "ORDINAL_POSITION", "IS_NULLABLE", "SCOPE_CATALOG", "SCOPE_SCHEMA", "SCOPE_TABLE",
                             "SOURCE_DATA_TYPE"},
                new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.INTEGER,
                          Types.VARCHAR, Types.INTEGER, Types.INTEGER, Types.INTEGER, Types.INTEGER,
                          Types.VARCHAR, Types.VARCHAR, Types.INTEGER, Types.INTEGER, Types.INTEGER,
                          Types.INTEGER, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR,
                          Types.SMALLINT}
            );
        }

        List<SBColumnInfo> cols = new ArrayList<>();
        cols.add(column("TYPE_CAT", 25));
        cols.add(column("TYPE_SCHEM", 25));
        cols.add(column("TYPE_NAME", 25));
        cols.add(column("ATTR_NAME", 25));
        cols.add(column("DATA_TYPE", 23));
        cols.add(column("ATTR_TYPE_NAME", 25));
        cols.add(column("ATTR_SIZE", 23));
        cols.add(column("DECIMAL_DIGITS", 21));
        cols.add(column("NUM_PREC_RADIX", 23));
        cols.add(column("NULLABLE", 23));
        cols.add(column("REMARKS", 25));
        cols.add(column("ATTR_DEF", 25));
        cols.add(column("SQL_DATA_TYPE", 23));
        cols.add(column("SQL_DATETIME_SUB", 23));
        cols.add(column("CHAR_OCTET_LENGTH", 23));
        cols.add(column("ORDINAL_POSITION", 23));
        cols.add(column("IS_NULLABLE", 25));
        cols.add(column("SCOPE_CATALOG", 25));
        cols.add(column("SCOPE_SCHEMA", 25));
        cols.add(column("SCOPE_TABLE", 25));
        cols.add(column("SOURCE_DATA_TYPE", 21));
        return new SBResultSet(null, cols, rows);
    }

    @Override
    public boolean supportsResultSetHoldability(int holdability) throws SQLException {
        return holdability == ResultSet.HOLD_CURSORS_OVER_COMMIT
            || holdability == ResultSet.CLOSE_CURSORS_AT_COMMIT;
    }

    @Override
    public int getResultSetHoldability() throws SQLException {
        return ResultSet.HOLD_CURSORS_OVER_COMMIT;
    }

    @Override
    public int getDatabaseMajorVersion() throws SQLException {
        return parseVersionComponent(getDatabaseProductVersion(), 0, 1);
    }

    @Override
    public int getDatabaseMinorVersion() throws SQLException {
        return parseVersionComponent(getDatabaseProductVersion(), 1, 0);
    }

    @Override
    public int getJDBCMajorVersion() throws SQLException {
        return 4;
    }

    @Override
    public int getJDBCMinorVersion() throws SQLException {
        return 3;
    }

    @Override
    public int getSQLStateType() throws SQLException {
        return sqlStateSQL;
    }

    @Override
    public boolean locatorsUpdateCopy() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsStatementPooling() throws SQLException {
        return true;
    }

    @Override
    public RowIdLifetime getRowIdLifetime() throws SQLException {
        return RowIdLifetime.ROWID_VALID_OTHER;
    }

    @Override
    public boolean supportsStoredFunctionsUsingCallSyntax() throws SQLException {
        return true;
    }

    @Override
    public boolean autoCommitFailureClosesAllResultSets() throws SQLException {
        return false;
    }

    @Override
    public ResultSet getClientInfoProperties() throws SQLException {
        String defaultApplicationName = null;
        String defaultClientUser = null;
        String defaultClientHostname = null;
        if (connection != null) {
            SBConnectionProperties props = connection.getConnectionProperties();
            if (props != null) {
                defaultApplicationName = props.getApplicationName();
                defaultClientUser = props.getUser();
                defaultClientHostname = props.getHost();
            }
        }
        List<Object[]> rows = new ArrayList<>();
        rows.add(new Object[]{
            "ApplicationName",
            1024,
            defaultApplicationName,
            "Client application name propagated to server session metadata"
        });
        rows.add(new Object[]{
            "ClientUser",
            256,
            defaultClientUser,
            "Client user hint for auditing and observability tooling"
        });
        rows.add(new Object[]{
            "ClientHostname",
            256,
            defaultClientHostname,
            "Client host hint for telemetry and audit correlation"
        });
        rows.add(new Object[]{
            "ClientPid",
            32,
            null,
            "Client process identifier used in diagnostics"
        });
        rows.add(new Object[]{
            "TraceTag",
            256,
            null,
            "Opaque trace tag for distributed request correlation"
        });

        List<SBColumnInfo> cols = new ArrayList<>();
        cols.add(column("NAME", 25));
        cols.add(column("MAX_LEN", 23));
        cols.add(column("DEFAULT_VALUE", 25));
        cols.add(column("DESCRIPTION", 25));
        return new SBResultSet(null, cols, rows);
    }

    @Override
    public ResultSet getFunctions(String catalog, String schemaPattern, String functionNamePattern)
            throws SQLException {
        String currentCatalog = connection.getConnectionProperties().getDatabase();
        if (catalog != null && currentCatalog != null && !catalog.equalsIgnoreCase(currentCatalog)) {
            return createEmptyResultSet(
                new String[]{"FUNCTION_CAT", "FUNCTION_SCHEM", "FUNCTION_NAME", "REMARKS",
                             "FUNCTION_TYPE", "SPECIFIC_NAME"},
                new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR,
                          Types.SMALLINT, Types.VARCHAR}
            );
        }

        List<Object[]> rows = new ArrayList<>();
        try {
            for (Object[] row : queryRows(
                "SELECT routine_schema, routine_name, data_type, routine_name " +
                "FROM information_schema.routines " +
                "WHERE routine_type = 'FUNCTION'"
            )) {
                String schemaName = toStringValue(row, 0);
                String functionName = toStringValue(row, 1);
                if (!matchesPattern(schemaName, schemaPattern)
                        || !matchesPattern(functionName, functionNamePattern)) {
                    continue;
                }

                String dataType = normalizeTypeName(toStringValue(row, 2));
                rows.add(new Object[]{
                    currentCatalog,
                    schemaName,
                    functionName,
                    null,
                    mapFunctionType(dataType),
                    toStringValue(row, 3)
                });
            }
        } catch (SQLException ex) {
            return createEmptyResultSet(
                new String[]{"FUNCTION_CAT", "FUNCTION_SCHEM", "FUNCTION_NAME", "REMARKS",
                             "FUNCTION_TYPE", "SPECIFIC_NAME"},
                new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR,
                          Types.SMALLINT, Types.VARCHAR}
            );
        }

        List<SBColumnInfo> cols = new ArrayList<>();
        cols.add(column("FUNCTION_CAT", 25));
        cols.add(column("FUNCTION_SCHEM", 25));
        cols.add(column("FUNCTION_NAME", 25));
        cols.add(column("REMARKS", 25));
        cols.add(column("FUNCTION_TYPE", 21));
        cols.add(column("SPECIFIC_NAME", 25));
        return new SBResultSet(null, cols, rows);
    }

    @Override
    public ResultSet getFunctionColumns(String catalog, String schemaPattern, String functionNamePattern,
            String columnNamePattern) throws SQLException {
        String currentCatalog = connection.getConnectionProperties().getDatabase();
        if (catalog != null && currentCatalog != null && !catalog.equalsIgnoreCase(currentCatalog)) {
            return createEmptyResultSet(
                new String[]{"FUNCTION_CAT", "FUNCTION_SCHEM", "FUNCTION_NAME", "COLUMN_NAME",
                             "COLUMN_TYPE", "DATA_TYPE", "TYPE_NAME", "PRECISION", "LENGTH", "SCALE",
                             "RADIX", "NULLABLE", "REMARKS", "CHAR_OCTET_LENGTH", "ORDINAL_POSITION",
                             "IS_NULLABLE", "SPECIFIC_NAME"},
                new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.SMALLINT,
                          Types.INTEGER, Types.VARCHAR, Types.INTEGER, Types.INTEGER, Types.SMALLINT,
                          Types.SMALLINT, Types.SMALLINT, Types.VARCHAR, Types.INTEGER, Types.INTEGER,
                          Types.VARCHAR, Types.VARCHAR}
            );
        }

        List<String[]> functionReturnTypes = new ArrayList<>();
        Map<String, String> functionTypeByKey = new HashMap<>();
        try {
            for (Object[] row : queryRows(
                "SELECT routine_schema, routine_name, data_type, routine_name " +
                "FROM information_schema.routines " +
                "WHERE routine_type = 'FUNCTION'"
            )) {
                String schemaName = toStringValue(row, 0);
                String functionName = toStringValue(row, 1);
                if (!matchesPattern(schemaName, schemaPattern)
                        || !matchesPattern(functionName, functionNamePattern)) {
                    continue;
                }
                String returnType = normalizeTypeName(toStringValue(row, 2));
                String specificName = toStringValue(row, 3);
                String functionKey = metadataKey(schemaName, functionName);
                functionTypeByKey.put(functionKey, returnType);
                functionReturnTypes.add(new String[]{schemaName, functionName, specificName});
            }

            if (functionTypeByKey.isEmpty()) {
                return createEmptyResultSet(
                    new String[]{"FUNCTION_CAT", "FUNCTION_SCHEM", "FUNCTION_NAME", "COLUMN_NAME",
                                 "COLUMN_TYPE", "DATA_TYPE", "TYPE_NAME", "PRECISION", "LENGTH", "SCALE",
                                 "RADIX", "NULLABLE", "REMARKS", "CHAR_OCTET_LENGTH", "ORDINAL_POSITION",
                                 "IS_NULLABLE", "SPECIFIC_NAME"},
                    new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.SMALLINT,
                              Types.INTEGER, Types.VARCHAR, Types.INTEGER, Types.INTEGER, Types.SMALLINT,
                              Types.SMALLINT, Types.SMALLINT, Types.VARCHAR, Types.INTEGER, Types.INTEGER,
                              Types.VARCHAR, Types.VARCHAR}
                );
            }

            Map<String, List<Object[]>> parameterRowsByFunction = new HashMap<>();
            for (Object[] row : queryRows(
                "SELECT p.specific_schema, p.specific_name, p.parameter_mode, p.parameter_name, " +
                "       p.data_type, p.character_maximum_length, p.numeric_precision, p.numeric_scale, " +
                "       p.ordinal_position " +
                "FROM information_schema.parameters p " +
                "JOIN information_schema.routines r " +
                "  ON r.routine_schema = p.specific_schema " +
                " AND r.routine_name = p.specific_name " +
                "WHERE r.routine_type = 'FUNCTION' " +
                "ORDER BY p.specific_schema, p.specific_name, p.ordinal_position"
            )) {
                String schemaName = toStringValue(row, 0);
                String functionName = toStringValue(row, 1);
                String functionKey = metadataKey(schemaName, functionName);
                if (!functionTypeByKey.containsKey(functionKey)) {
                    continue;
                }
                String parameterName = toStringValue(row, 3);
                if (!matchesPattern(parameterName, columnNamePattern)) {
                    continue;
                }
                String typeName = normalizeTypeName(toStringValue(row, 4));
                int jdbcType = jdbcTypeFromTypeName(typeName);
                int precision = toIntValue(row[6], 0);
                int length = toIntValue(row[5], 0);
                int scale = toIntValue(row[7], 0);
                short nullable = DatabaseMetaData.columnNullable;
                Object[] parameterRow = {
                    currentCatalog,
                    schemaName,
                    functionName,
                    parameterName,
                    mapFunctionColumnType(toStringValue(row, 2)),
                    jdbcType,
                    typeName,
                    precision,
                    length,
                    scale,
                    10,
                    nullable,
                    null,
                    length,
                    toIntValue(row[8], 0),
                    nullable == DatabaseMetaData.columnNullable ? "YES" : "NO",
                    functionName
                };
                parameterRowsByFunction.computeIfAbsent(functionKey, k -> new ArrayList<>()).add(parameterRow);
            }

            List<Object[]> rows = new ArrayList<>();
            for (String[] functionInfo : functionReturnTypes) {
                String schemaName = functionInfo[0];
                String functionName = functionInfo[1];
                String specificName = functionInfo[2];
                String functionKey = metadataKey(schemaName, functionName);
                String returnType = functionTypeByKey.get(functionKey);
                String normalizedReturnType = normalizeTypeName(returnType);
                int returnSqlType = jdbcTypeFromTypeName(normalizedReturnType);
                rows.add(new Object[]{
                    currentCatalog,
                    schemaName,
                    functionName,
                    null,
                    DatabaseMetaData.functionColumnResult,
                    returnSqlType,
                    normalizedReturnType,
                    0,
                    0,
                    0,
                    10,
                    DatabaseMetaData.columnNullable,
                    null,
                    0,
                    0,
                    "YES",
                    specificName
                });
                List<Object[]> parameters = parameterRowsByFunction.get(functionKey);
                if (parameters != null) {
                    rows.addAll(parameters);
                }
            }

            List<SBColumnInfo> cols = new ArrayList<>();
            cols.add(column("FUNCTION_CAT", 25));
            cols.add(column("FUNCTION_SCHEM", 25));
            cols.add(column("FUNCTION_NAME", 25));
            cols.add(column("COLUMN_NAME", 25));
            cols.add(column("COLUMN_TYPE", 21));
            cols.add(column("DATA_TYPE", 23));
            cols.add(column("TYPE_NAME", 25));
            cols.add(column("PRECISION", 23));
            cols.add(column("LENGTH", 23));
            cols.add(column("SCALE", 21));
            cols.add(column("RADIX", 21));
            cols.add(column("NULLABLE", 21));
            cols.add(column("REMARKS", 25));
            cols.add(column("CHAR_OCTET_LENGTH", 23));
            cols.add(column("ORDINAL_POSITION", 23));
            cols.add(column("IS_NULLABLE", 25));
            cols.add(column("SPECIFIC_NAME", 25));
            return new SBResultSet(null, cols, rows);
        } catch (SQLException ex) {
            return createEmptyResultSet(
                new String[]{"FUNCTION_CAT", "FUNCTION_SCHEM", "FUNCTION_NAME", "COLUMN_NAME",
                             "COLUMN_TYPE", "DATA_TYPE", "TYPE_NAME", "PRECISION", "LENGTH", "SCALE",
                             "RADIX", "NULLABLE", "REMARKS", "CHAR_OCTET_LENGTH", "ORDINAL_POSITION",
                             "IS_NULLABLE", "SPECIFIC_NAME"},
                new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.SMALLINT,
                          Types.INTEGER, Types.VARCHAR, Types.INTEGER, Types.INTEGER, Types.SMALLINT,
                          Types.SMALLINT, Types.SMALLINT, Types.VARCHAR, Types.INTEGER, Types.INTEGER,
                          Types.VARCHAR, Types.VARCHAR}
            );
        }
    }

    @Override
    public ResultSet getPseudoColumns(String catalog, String schemaPattern, String tableNamePattern,
            String columnNamePattern) throws SQLException {
        String currentCatalog = connection.getConnectionProperties().getDatabase();
        if (catalog != null && currentCatalog != null && !catalog.equalsIgnoreCase(currentCatalog)) {
            return createEmptyResultSet(
                new String[]{"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "COLUMN_NAME", "DATA_TYPE",
                             "COLUMN_SIZE", "DECIMAL_DIGITS", "NUM_PREC_RADIX", "COLUMN_USAGE",
                             "REMARKS", "CHAR_OCTET_LENGTH", "IS_NULLABLE"},
                new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.INTEGER,
                          Types.INTEGER, Types.INTEGER, Types.INTEGER, Types.VARCHAR, Types.VARCHAR,
                          Types.INTEGER, Types.VARCHAR}
            );
        }

        List<Object[]> rows = new ArrayList<>();
        try {
            for (Object[] row : queryRows(
                "SELECT ns.nspname, c.relname, a.attname, a.atttypid " +
                "FROM pg_catalog.pg_class c " +
                "JOIN pg_catalog.pg_namespace ns ON ns.oid = c.relnamespace " +
                "JOIN pg_catalog.pg_attribute a ON a.attrelid = c.oid " +
                "WHERE c.relkind IN ('r','v','m','f') " +
                "  AND ns.nspname NOT IN ('pg_catalog','pg_toast','information_schema') " +
                "  AND a.attnum < 0 " +
                "  AND a.attname IN ('ctid','xmin','xmax','cmin','cmax','tableoid') " +
                "ORDER BY ns.nspname, c.relname, a.attname"
            )) {
                String schemaName = toStringValue(row, 0);
                String tableName = toStringValue(row, 1);
                String columnName = toStringValue(row, 2);
                if (!matchesPattern(schemaName, schemaPattern)
                        || !matchesPattern(tableName, tableNamePattern)
                        || !matchesPattern(columnName, columnNamePattern)) {
                    continue;
                }

                Integer typeOid = parseOid(row[3]);
                int dataType = typeOid != null ? jdbcTypeFromOid(typeOid) : Types.OTHER;
                rows.add(new Object[]{
                    currentCatalog,
                    schemaName,
                    tableName,
                    columnName,
                    dataType,
                    0,
                    0,
                    10,
                    "SYSTEM",
                    null,
                    0,
                    "NO"
                });
            }
        } catch (SQLException ex) {
            return createEmptyResultSet(
                new String[]{"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "COLUMN_NAME", "DATA_TYPE",
                             "COLUMN_SIZE", "DECIMAL_DIGITS", "NUM_PREC_RADIX", "COLUMN_USAGE",
                             "REMARKS", "CHAR_OCTET_LENGTH", "IS_NULLABLE"},
                new int[]{Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.INTEGER,
                          Types.INTEGER, Types.INTEGER, Types.INTEGER, Types.VARCHAR, Types.VARCHAR,
                          Types.INTEGER, Types.VARCHAR}
            );
        }

        List<SBColumnInfo> cols = new ArrayList<>();
        cols.add(column("TABLE_CAT", 25));
        cols.add(column("TABLE_SCHEM", 25));
        cols.add(column("TABLE_NAME", 25));
        cols.add(column("COLUMN_NAME", 25));
        cols.add(column("DATA_TYPE", 23));
        cols.add(column("COLUMN_SIZE", 23));
        cols.add(column("DECIMAL_DIGITS", 21));
        cols.add(column("NUM_PREC_RADIX", 23));
        cols.add(column("COLUMN_USAGE", 25));
        cols.add(column("REMARKS", 25));
        cols.add(column("CHAR_OCTET_LENGTH", 23));
        cols.add(column("IS_NULLABLE", 25));
        return new SBResultSet(null, cols, rows);
    }

    @Override
    public boolean generatedKeyAlwaysReturned() throws SQLException {
        return true;
    }

    @Override
    public long getMaxLogicalLobSize() throws SQLException {
        return 1073741823L;
    }

    @Override
    public boolean supportsRefCursors() throws SQLException {
        return true;
    }

    @Override
    public boolean supportsSharding() throws SQLException {
        return true;
    }

    private static boolean isNumericType(int sqlType) {
        return switch (sqlType) {
            case Types.BIT, Types.TINYINT, Types.SMALLINT, Types.INTEGER, Types.BIGINT,
                Types.FLOAT, Types.REAL, Types.DOUBLE, Types.NUMERIC, Types.DECIMAL -> true;
            default -> false;
        };
    }

    private static boolean isCharacterType(int sqlType) {
        return switch (sqlType) {
            case Types.CHAR, Types.VARCHAR, Types.LONGVARCHAR,
                Types.NCHAR, Types.NVARCHAR, Types.LONGNVARCHAR, Types.CLOB, Types.NCLOB -> true;
            default -> false;
        };
    }

    private static boolean isTemporalType(int sqlType) {
        return switch (sqlType) {
            case Types.DATE, Types.TIME, Types.TIME_WITH_TIMEZONE,
                Types.TIMESTAMP, Types.TIMESTAMP_WITH_TIMEZONE -> true;
            default -> false;
        };
    }

    private static boolean isBinaryType(int sqlType) {
        return switch (sqlType) {
            case Types.BINARY, Types.VARBINARY, Types.LONGVARBINARY, Types.BLOB -> true;
            default -> false;
        };
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

    // Helper method to create empty result sets
    private ResultSet createEmptyResultSet(String[] columnNames, int[] columnTypes) {
        List<SBColumnInfo> cols = new ArrayList<>();
        for (int i = 0; i < columnNames.length; i++) {
            SBColumnInfo col = new SBColumnInfo();
            col.setName(columnNames[i]);
            // Map SQL type to OID
            col.setTypeOid(sqlTypeToOid(columnTypes[i]));
            cols.add(col);
        }
        return new SBResultSet(null, cols, Collections.emptyList());
    }

    private int sqlTypeToOid(int sqlType) {
        switch (sqlType) {
            case Types.VARCHAR: return 25;
            case Types.INTEGER: return 23;
            case Types.SMALLINT: return 21;
            case Types.BIGINT: return 20;
            case Types.BOOLEAN: return 16;
            default: return 25;  // Default to text
        }
    }

    private String normalizedServerParameter(String name) {
        if (connection == null || name == null || name.isBlank()) {
            return "";
        }
        try {
            SBProtocolHandler protocol = connection.getProtocol();
            if (protocol == null) {
                return "";
            }
            String value = protocol.getServerParameter(name);
            if (value == null) {
                return "";
            }
            return value.trim().toLowerCase(Locale.ROOT);
        } catch (Exception ignored) {
            return "";
        }
    }

    private String serverVersionFromConnection() {
        if (connection == null) {
            return null;
        }
        try {
            SBProtocolHandler protocol = connection.getProtocol();
            if (protocol == null) {
                return null;
            }
            String serverVersion = protocol.getServerParameter("server_version");
            if (serverVersion != null && !serverVersion.isBlank()) {
                return serverVersion;
            }
            String serverVersionNum = protocol.getServerParameter("server_version_num");
            if (serverVersionNum == null || serverVersionNum.isBlank()) {
                return null;
            }
            if (!serverVersionNum.chars().allMatch(Character::isDigit)) {
                return serverVersionNum;
            }
            int numeric = Integer.parseInt(serverVersionNum);
            int major = numeric / 10000;
            int minor = (numeric / 100) % 100;
            int patch = numeric % 100;
            if (patch > 0) {
                return major + "." + minor + "." + patch;
            }
            return major + "." + minor;
        } catch (Exception ignored) {
            return null;
        }
    }

    private int parseVersionComponent(String version, int index, int fallback) {
        if (version == null || version.isBlank()) {
            return fallback;
        }
        int partIndex = 0;
        int i = 0;
        while (i < version.length()) {
            while (i < version.length() && !Character.isDigit(version.charAt(i))) {
                i++;
            }
            if (i >= version.length()) {
                break;
            }
            int start = i;
            while (i < version.length() && Character.isDigit(version.charAt(i))) {
                i++;
            }
            if (partIndex == index) {
                try {
                    return Integer.parseInt(version.substring(start, i));
                } catch (NumberFormatException ignored) {
                    return fallback;
                }
            }
            partIndex++;
        }
        return fallback;
    }
}
