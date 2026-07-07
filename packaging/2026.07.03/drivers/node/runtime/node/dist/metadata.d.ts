export declare const METADATA_SCHEMAS_QUERY = "SELECT schema_id, schema_name, owner_id, default_tablespace_id FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name";
export declare const METADATA_CATALOGS_QUERY = "SELECT schema_id AS catalog_id, schema_name AS catalog_name FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name";
export declare const METADATA_TABLES_QUERY = "SELECT t.table_id, t.schema_id, s.schema_name, t.table_name, t.table_type, t.owner_id FROM sys.tables t LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE t.is_valid = 1 ORDER BY s.schema_name, t.table_name";
export declare const METADATA_COLUMNS_QUERY = "SELECT c.column_id, c.table_id, t.table_name, t.schema_id, s.schema_name, c.column_name, c.data_type_id, c.data_type_name, c.ordinal_position, c.is_nullable, c.default_value, c.domain_id, c.collation_id, c.charset_id, c.is_identity, c.is_generated, c.generation_expression FROM sys.columns c LEFT JOIN sys.tables t ON t.table_id = c.table_id LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE c.is_valid = 1 ORDER BY s.schema_name, t.table_name, c.ordinal_position";
export declare const METADATA_INDEXES_QUERY = "SELECT i.index_id, i.table_id, t.table_name, t.schema_id, s.schema_name, i.index_name, i.index_type, i.is_unique FROM sys.indexes i LEFT JOIN sys.tables t ON t.table_id = i.table_id LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE i.is_valid = 1 ORDER BY s.schema_name, t.table_name, i.index_name";
export declare const METADATA_INDEX_COLUMNS_QUERY = "SELECT ic.index_id, i.index_name, ic.column_id, ic.column_name, ic.ordinal_position, ic.is_included, i.table_id, t.table_name, t.schema_id, s.schema_name FROM sys.index_columns ic LEFT JOIN sys.indexes i ON i.index_id = ic.index_id LEFT JOIN sys.tables t ON t.table_id = i.table_id LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id ORDER BY s.schema_name, t.table_name, i.index_name, ic.ordinal_position";
export declare const METADATA_CONSTRAINTS_QUERY = "SELECT * FROM information_schema.table_constraints";
export declare const METADATA_PRIMARY_KEYS_QUERY = "SELECT * FROM information_schema.table_constraints";
export declare const METADATA_FOREIGN_KEYS_QUERY = "SELECT * FROM information_schema.table_constraints";
export declare const METADATA_TABLE_PRIVILEGES_QUERY = "SELECT t.table_id, t.table_name, t.schema_id, s.schema_name, t.owner_id AS grantor_id, t.owner_id AS grantee_id, 'ALL' AS privilege_type, 'YES' AS is_grantable FROM sys.tables t LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE t.is_valid = 1 ORDER BY s.schema_name, t.table_name";
export declare const METADATA_COLUMN_PRIVILEGES_QUERY = "SELECT c.table_id, t.table_name, t.schema_id, s.schema_name, c.column_id, c.column_name, 'ALL' AS privilege_type, 'YES' AS is_grantable FROM sys.columns c LEFT JOIN sys.tables t ON t.table_id = c.table_id LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE c.is_valid = 1 ORDER BY s.schema_name, t.table_name, c.ordinal_position";
export declare const METADATA_PROCEDURES_QUERY = "SELECT * FROM information_schema.routines";
export declare const METADATA_FUNCTIONS_QUERY = "SELECT * FROM information_schema.routines";
export declare const METADATA_ROUTINES_QUERY = "SELECT * FROM information_schema.routines";
export declare const METADATA_TYPE_INFO_QUERY = "SELECT DISTINCT data_type_id, data_type_name, data_type_name AS type_name FROM sys.columns WHERE is_valid = 1 ORDER BY data_type_name";
export type MetadataCollectionName = "catalogs" | "schemas" | "tables" | "columns" | "indexes" | "index_columns" | "constraints" | "primary_keys" | "foreign_keys" | "table_privileges" | "column_privileges" | "procedures" | "functions" | "routines" | "type_info";
export interface MetadataSchemaTreeNode {
    name: string;
    path: string;
    terminal: boolean;
    children: MetadataSchemaTreeNode[];
}
export interface MetadataSchemaTree {
    database: string | null;
    schemas: MetadataSchemaTreeNode[];
}
export interface MetadataSchemaTreeOptions {
    expandParents?: boolean;
    database?: string;
    restrictions?: MetadataRestrictions;
}
export type MetadataSchemaInput = string | Record<string, unknown>;
export type MetadataRestrictions = Record<string, unknown>;
export interface MetadataShapeOptions {
    database?: string | null;
}
export declare function normalizeMetadataCollectionName(collectionName?: string): MetadataCollectionName;
export declare function resolveMetadataCollectionQuery(collectionName?: string): string;
export declare function normalizeMetadataRestrictions(restrictions?: MetadataRestrictions): MetadataRestrictions;
export declare function filterMetadataRowsByRestrictions<T extends Record<string, unknown>>(rows: readonly T[], restrictions?: MetadataRestrictions, collectionName?: string): T[];
export declare function expandSchemaPaths(schemaPaths: readonly string[]): string[];
export declare function listMetadataSchemaPaths(rows: readonly MetadataSchemaInput[], options?: {
    expandParents?: boolean;
}): string[];
export declare function buildMetadataSchemaTree(rows: readonly MetadataSchemaInput[], options?: MetadataSchemaTreeOptions): MetadataSchemaTree;
export declare function expandSchemaMetadataRows<T extends Record<string, unknown>>(rows: readonly T[]): T[];
export declare function shapeMetadataRowsForCollection<T extends Record<string, unknown>>(rows: readonly T[], collectionName: MetadataCollectionName, options?: MetadataShapeOptions): T[];
export declare function filterMetadataRowsForCollectionFamily<T extends Record<string, unknown>>(rows: readonly T[], collectionName: MetadataCollectionName): T[];
