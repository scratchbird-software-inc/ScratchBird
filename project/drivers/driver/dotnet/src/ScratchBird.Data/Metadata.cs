// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

namespace ScratchBird.Data;

public static class ScratchBirdMetadata
{
    public const string CatalogsQuery = "SELECT schema_id AS catalog_id, schema_name AS catalog_name FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name";
    public const string SchemasQuery = "SELECT schema_id, schema_name, owner_id, default_tablespace_id FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name";
    public const string TablesQuery = "SELECT t.table_id, t.schema_id, s.schema_name, s.schema_name AS table_schema, t.table_name, t.table_type, t.owner_id FROM sys.tables t LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE t.is_valid = 1 ORDER BY s.schema_name, t.table_name";
    public const string ColumnsQuery = "SELECT c.column_id, c.table_id, t.table_name, t.schema_id, s.schema_name, s.schema_name AS table_schema, c.column_name, c.data_type_id, c.data_type_name AS data_type, c.data_type_name, c.ordinal_position, c.is_nullable, c.default_value, c.domain_id, c.collation_id, c.charset_id, c.is_identity, c.is_generated, c.generation_expression FROM sys.columns c LEFT JOIN sys.tables t ON t.table_id = c.table_id LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE c.is_valid = 1 ORDER BY s.schema_name, t.table_name, c.ordinal_position";
    public const string IndexesQuery = "SELECT i.index_id, i.table_id, t.table_name, t.schema_id, s.schema_name, s.schema_name AS table_schema, i.index_name, i.index_type, i.is_unique FROM sys.indexes i LEFT JOIN sys.tables t ON t.table_id = i.table_id LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE i.is_valid = 1 ORDER BY s.schema_name, t.table_name, i.index_name";
    public const string IndexColumnsQuery = "SELECT ic.index_id, i.index_name, ic.column_id, ic.column_name, ic.ordinal_position, ic.is_included, i.table_id, t.table_name, t.schema_id, s.schema_name, s.schema_name AS table_schema FROM sys.index_columns ic LEFT JOIN sys.indexes i ON i.index_id = ic.index_id LEFT JOIN sys.tables t ON t.table_id = i.table_id LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id ORDER BY s.schema_name, t.table_name, i.index_name, ic.ordinal_position";
    public const string ConstraintsQuery = "SELECT * FROM information_schema.table_constraints";
    public const string PrimaryKeysQuery = ConstraintsQuery;
    public const string ForeignKeysQuery = ConstraintsQuery;
    public const string TablePrivilegesQuery = "SELECT t.table_id, t.table_name, t.schema_id, s.schema_name, s.schema_name AS table_schema, t.owner_id AS grantor_id, t.owner_id AS grantee_id, 'ALL' AS privilege_type FROM sys.tables t LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE t.is_valid = 1 ORDER BY s.schema_name, t.table_name";
    public const string ColumnPrivilegesQuery = "SELECT c.table_id, t.table_name, t.schema_id, s.schema_name, s.schema_name AS table_schema, c.column_id, c.column_name, 'ALL' AS privilege_type FROM sys.columns c LEFT JOIN sys.tables t ON t.table_id = c.table_id LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE c.is_valid = 1 ORDER BY s.schema_name, t.table_name, c.ordinal_position";
    public const string ProceduresQuery = "SELECT * FROM information_schema.routines";
    public const string FunctionsQuery = ProceduresQuery;
    public const string RoutinesQuery = ProceduresQuery;
    public const string TypeInfoQuery = "SELECT DISTINCT data_type_id, data_type_name, data_type_name AS type_name FROM sys.columns WHERE is_valid = 1 ORDER BY data_type_name";
}
