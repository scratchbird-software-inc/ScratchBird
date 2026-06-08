// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

export const METADATA_SCHEMAS_QUERY =
  "SELECT schema_id, schema_name, owner_id, default_tablespace_id FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name";

export const METADATA_CATALOGS_QUERY =
  "SELECT schema_id AS catalog_id, schema_name AS catalog_name FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name";

export const METADATA_TABLES_QUERY =
  "SELECT t.table_id, t.schema_id, s.schema_name, t.table_name, t.table_type, t.owner_id FROM sys.tables t LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE t.is_valid = 1 ORDER BY s.schema_name, t.table_name";

export const METADATA_COLUMNS_QUERY =
  "SELECT c.column_id, c.table_id, t.table_name, t.schema_id, s.schema_name, c.column_name, c.data_type_id, c.data_type_name, c.ordinal_position, c.is_nullable, c.default_value, c.domain_id, c.collation_id, c.charset_id, c.is_identity, c.is_generated, c.generation_expression FROM sys.columns c LEFT JOIN sys.tables t ON t.table_id = c.table_id LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE c.is_valid = 1 ORDER BY s.schema_name, t.table_name, c.ordinal_position";

export const METADATA_INDEXES_QUERY =
  "SELECT i.index_id, i.table_id, t.table_name, t.schema_id, s.schema_name, i.index_name, i.index_type, i.is_unique FROM sys.indexes i LEFT JOIN sys.tables t ON t.table_id = i.table_id LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE i.is_valid = 1 ORDER BY s.schema_name, t.table_name, i.index_name";

export const METADATA_INDEX_COLUMNS_QUERY =
  "SELECT ic.index_id, i.index_name, ic.column_id, ic.column_name, ic.ordinal_position, ic.is_included, i.table_id, t.table_name, t.schema_id, s.schema_name FROM sys.index_columns ic LEFT JOIN sys.indexes i ON i.index_id = ic.index_id LEFT JOIN sys.tables t ON t.table_id = i.table_id LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id ORDER BY s.schema_name, t.table_name, i.index_name, ic.ordinal_position";

export const METADATA_CONSTRAINTS_QUERY =
  "SELECT * FROM information_schema.table_constraints";

export const METADATA_PRIMARY_KEYS_QUERY =
  METADATA_CONSTRAINTS_QUERY;

export const METADATA_FOREIGN_KEYS_QUERY =
  METADATA_CONSTRAINTS_QUERY;

export const METADATA_TABLE_PRIVILEGES_QUERY =
  "SELECT t.table_id, t.table_name, t.schema_id, s.schema_name, t.owner_id AS grantor_id, t.owner_id AS grantee_id, 'ALL' AS privilege_type, 'YES' AS is_grantable FROM sys.tables t LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE t.is_valid = 1 ORDER BY s.schema_name, t.table_name";

export const METADATA_COLUMN_PRIVILEGES_QUERY =
  "SELECT c.table_id, t.table_name, t.schema_id, s.schema_name, c.column_id, c.column_name, 'ALL' AS privilege_type, 'YES' AS is_grantable FROM sys.columns c LEFT JOIN sys.tables t ON t.table_id = c.table_id LEFT JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE c.is_valid = 1 ORDER BY s.schema_name, t.table_name, c.ordinal_position";

export const METADATA_PROCEDURES_QUERY =
  "SELECT * FROM information_schema.routines";

export const METADATA_FUNCTIONS_QUERY =
  METADATA_PROCEDURES_QUERY;

export const METADATA_ROUTINES_QUERY =
  METADATA_PROCEDURES_QUERY;

export const METADATA_TYPE_INFO_QUERY =
  "SELECT DISTINCT data_type_id, data_type_name, data_type_name AS type_name FROM sys.columns WHERE is_valid = 1 ORDER BY data_type_name";

export type MetadataCollectionName =
  | "catalogs"
  | "schemas"
  | "tables"
  | "columns"
  | "indexes"
  | "index_columns"
  | "constraints"
  | "primary_keys"
  | "foreign_keys"
  | "table_privileges"
  | "column_privileges"
  | "procedures"
  | "functions"
  | "routines"
  | "type_info";

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

const METADATA_COLLECTION_QUERIES: Record<MetadataCollectionName, string> = {
  catalogs: METADATA_CATALOGS_QUERY,
  schemas: METADATA_SCHEMAS_QUERY,
  tables: METADATA_TABLES_QUERY,
  columns: METADATA_COLUMNS_QUERY,
  indexes: METADATA_INDEXES_QUERY,
  index_columns: METADATA_INDEX_COLUMNS_QUERY,
  constraints: METADATA_CONSTRAINTS_QUERY,
  primary_keys: METADATA_PRIMARY_KEYS_QUERY,
  foreign_keys: METADATA_FOREIGN_KEYS_QUERY,
  table_privileges: METADATA_TABLE_PRIVILEGES_QUERY,
  column_privileges: METADATA_COLUMN_PRIVILEGES_QUERY,
  procedures: METADATA_PROCEDURES_QUERY,
  functions: METADATA_FUNCTIONS_QUERY,
  routines: METADATA_ROUTINES_QUERY,
  type_info: METADATA_TYPE_INFO_QUERY,
};

const METADATA_COLLECTION_ALIASES: Record<string, MetadataCollectionName> = {
  catalogs: "catalogs",
  catalog: "catalogs",
  schemas: "schemas",
  schema: "schemas",
  tables: "tables",
  table: "tables",
  columns: "columns",
  column: "columns",
  indexes: "indexes",
  index: "indexes",
  indexcolumns: "index_columns",
  index_columns: "index_columns",
  constraints: "constraints",
  constraint: "constraints",
  primarykeys: "primary_keys",
  primary_keys: "primary_keys",
  primarykey: "primary_keys",
  pk: "primary_keys",
  foreignkeys: "foreign_keys",
  foreign_keys: "foreign_keys",
  foreignkey: "foreign_keys",
  fk: "foreign_keys",
  tableprivileges: "table_privileges",
  table_privileges: "table_privileges",
  columnprivileges: "column_privileges",
  column_privileges: "column_privileges",
  procedures: "procedures",
  procedure: "procedures",
  functions: "functions",
  function: "functions",
  routines: "routines",
  routine: "routines",
  typeinfo: "type_info",
  type_info: "type_info",
  types: "type_info",
};

const SCHEMA_FIELD_CANDIDATES = [
  "schema_name",
  "TABLE_SCHEM",
  "table_schem",
  "table_schema",
  "TABLE_SCHEMA",
  "schema",
] as const;

const METADATA_RESTRICTION_KEY_ALIASES: Record<string, readonly string[]> = {
  catalog: ["catalog_name", "table_catalog", "table_cat", "catalog"],
  schema: ["schema_name", "table_schema", "table_schem", "schema"],
  table: ["table_name", "table", "relname"],
  column: ["column_name", "column"],
  index: ["index_name", "index"],
  constraint: ["constraint_name", "constraint"],
  procedure: ["procedure_name", "routine_name", "procedure"],
  function: ["function_name", "routine_name", "function"],
  routine: ["routine_name", "procedure_name", "function_name", "routine"],
  type: ["type_name", "data_type_name", "data_type", "udt_name"],
};

const METADATA_COLLECTION_RESTRICTION_KEYS: Record<MetadataCollectionName, readonly string[]> = {
  catalogs: ["catalog"],
  schemas: ["catalog", "schema"],
  tables: ["catalog", "schema", "table", "type"],
  columns: ["catalog", "schema", "table", "column", "type"],
  indexes: ["catalog", "schema", "table", "index"],
  index_columns: ["catalog", "schema", "table", "index", "column"],
  constraints: ["catalog", "schema", "table", "constraint"],
  primary_keys: ["catalog", "schema", "table", "constraint"],
  foreign_keys: ["catalog", "schema", "table", "constraint"],
  table_privileges: ["catalog", "schema", "table"],
  column_privileges: ["catalog", "schema", "table", "column"],
  procedures: ["catalog", "schema", "procedure"],
  functions: ["catalog", "schema", "function"],
  routines: ["catalog", "schema", "routine"],
  type_info: ["type"],
};

export function normalizeMetadataCollectionName(collectionName?: string): MetadataCollectionName {
  const normalized = (collectionName ?? "tables").trim().toLowerCase();
  const resolved = METADATA_COLLECTION_ALIASES[normalized];
  if (resolved) {
    return resolved;
  }
  throw new Error(`Metadata collection '${collectionName ?? ""}' is not supported`);
}

export function resolveMetadataCollectionQuery(collectionName?: string): string {
  return METADATA_COLLECTION_QUERIES[normalizeMetadataCollectionName(collectionName)];
}

export function normalizeMetadataRestrictions(restrictions?: MetadataRestrictions): MetadataRestrictions {
  if (!restrictions) {
    return {};
  }
  const out: MetadataRestrictions = {};
  for (const [key, value] of Object.entries(restrictions)) {
    const normalizedKey = normalizeMetadataIdentifier(key);
    if (!normalizedKey) {
      continue;
    }
    out[normalizedKey] = value;
  }
  return out;
}

export function filterMetadataRowsByRestrictions<T extends Record<string, unknown>>(
  rows: readonly T[],
  restrictions?: MetadataRestrictions,
  collectionName?: string,
): T[] {
  const normalizedRestrictions = normalizeMetadataRestrictions(restrictions);
  if (Object.keys(normalizedRestrictions).length === 0) {
    return [...rows];
  }
  const bindings = buildMetadataRestrictionBindings(rows, normalizedRestrictions, collectionName);
  if (!bindings.length) {
    return [...rows];
  }
  return rows.filter((row) => metadataRowMatchesBindings(row, bindings));
}

export function expandSchemaPaths(schemaPaths: readonly string[]): string[] {
  const out: string[] = [];
  const seen = new Set<string>();
  for (const schemaPath of schemaPaths) {
    const segments = splitSchemaPath(schemaPath);
    if (!segments.length) {
      continue;
    }
    let current = "";
    for (const segment of segments) {
      current = current ? `${current}.${segment}` : segment;
      if (!seen.has(current)) {
        seen.add(current);
        out.push(current);
      }
    }
  }
  return out;
}

export function listMetadataSchemaPaths(rows: readonly MetadataSchemaInput[], options?: { expandParents?: boolean }): string[] {
  const deduped: string[] = [];
  const seen = new Set<string>();
  for (const row of rows) {
    const schemaPath = readSchemaPath(row);
    if (!schemaPath || seen.has(schemaPath)) {
      continue;
    }
    seen.add(schemaPath);
    deduped.push(schemaPath);
  }
  return options?.expandParents ? expandSchemaPaths(deduped) : deduped;
}

export function buildMetadataSchemaTree(rows: readonly MetadataSchemaInput[], options?: MetadataSchemaTreeOptions): MetadataSchemaTree {
  const basePaths = listMetadataSchemaPaths(rows);
  const expandedPaths = options?.expandParents ? expandSchemaPaths(basePaths) : basePaths;
  const terminalPaths = new Set(options?.expandParents ? expandedPaths : basePaths);
  const nodesByPath = new Map<string, MetadataSchemaTreeNode>();
  const roots: MetadataSchemaTreeNode[] = [];

  for (const schemaPath of expandedPaths) {
    let parent: MetadataSchemaTreeNode | null = null;
    let currentPath = "";
    for (const segment of splitSchemaPath(schemaPath)) {
      currentPath = currentPath ? `${currentPath}.${segment}` : segment;
      let node = nodesByPath.get(currentPath);
      if (!node) {
        node = { name: segment, path: currentPath, terminal: false, children: [] };
        nodesByPath.set(currentPath, node);
        if (parent) {
          parent.children.push(node);
        } else {
          roots.push(node);
        }
      }
      if (terminalPaths.has(currentPath)) {
        node.terminal = true;
      }
      parent = node;
    }
  }

  const database = options?.database?.trim();
  return {
    database: database ? database : null,
    schemas: roots,
  };
}

export function expandSchemaMetadataRows<T extends Record<string, unknown>>(rows: readonly T[]): T[] {
  const out: T[] = [];
  const seen = new Set<string>();
  for (const row of rows) {
    const schemaPath = readSchemaPath(row);
    if (!schemaPath) {
      out.push(row);
      continue;
    }
    let current = "";
    const segments = splitSchemaPath(schemaPath);
    for (let i = 0; i < segments.length; i++) {
      current = current ? `${current}.${segments[i]}` : segments[i];
      if (seen.has(current)) {
        continue;
      }
      seen.add(current);
      if (i === segments.length - 1) {
        out.push(row);
      } else {
        out.push(createSyntheticSchemaRow(row, current));
      }
    }
  }
  return out;
}

export function shapeMetadataRowsForCollection<T extends Record<string, unknown>>(
  rows: readonly T[],
  collectionName: MetadataCollectionName,
  options?: MetadataShapeOptions,
): T[] {
  const catalogName = normalizeCatalogName(options?.database);
  return rows.map((row) => shapeMetadataRow(row, collectionName, catalogName));
}

export function filterMetadataRowsForCollectionFamily<T extends Record<string, unknown>>(
  rows: readonly T[],
  collectionName: MetadataCollectionName,
): T[] {
  switch (collectionName) {
    case "primary_keys":
      return rows.filter((row) => metadataTypeMatches(row, ["constraint_type", "CONSTRAINT_TYPE"], ["primary key", "primary"]));
    case "foreign_keys":
      return rows.filter((row) => metadataTypeMatches(row, ["constraint_type", "CONSTRAINT_TYPE"], ["foreign key", "foreign"]));
    case "procedures":
      return rows.filter((row) => metadataTypeMatches(row, ["routine_type", "ROUTINE_TYPE"], ["procedure"]));
    case "functions":
      return rows.filter((row) => metadataTypeMatches(row, ["routine_type", "ROUTINE_TYPE"], ["function"]));
    default:
      return [...rows];
  }
}

interface MetadataRestrictionBinding {
  aliases: string[];
  expectNull: boolean;
  expectedText: string;
}

function buildMetadataRestrictionBindings(
  rows: readonly Record<string, unknown>[],
  restrictions: MetadataRestrictions,
  collectionName?: string,
): MetadataRestrictionBinding[] {
  const allowedAliases = new Set<string>();
  if (collectionName) {
    const resolvedCollection = normalizeMetadataCollectionName(collectionName);
    for (const restrictionKey of METADATA_COLLECTION_RESTRICTION_KEYS[resolvedCollection]) {
      for (const alias of metadataRestrictionAliases(restrictionKey)) {
        allowedAliases.add(alias);
      }
    }
  }

  const bindings: MetadataRestrictionBinding[] = [];
  for (const [restrictionKey, restrictionValue] of Object.entries(restrictions)) {
    const aliases = new Set<string>();
    for (const alias of metadataRestrictionAliases(restrictionKey)) {
      if (allowedAliases.size > 0 && !allowedAliases.has(alias) && alias !== restrictionKey) {
        continue;
      }
      aliases.add(alias);
    }
    if (!aliases.size) {
      continue;
    }
    if (!rowsHaveAnyAlias(rows, aliases)) {
      continue;
    }

    const expectedText = normalizeMetadataMatchText(restrictionValue);
    bindings.push({
      aliases: [...aliases],
      expectNull: expectedText === "null",
      expectedText,
    });
  }
  return bindings;
}

function rowsHaveAnyAlias(rows: readonly Record<string, unknown>[], aliases: Set<string>): boolean {
  for (const row of rows) {
    for (const alias of aliases) {
      if (metadataRowValueByAlias(row, alias).present) {
        return true;
      }
    }
  }
  return false;
}

function metadataRowMatchesBindings(row: Record<string, unknown>, bindings: readonly MetadataRestrictionBinding[]): boolean {
  for (const binding of bindings) {
    let matched = false;
    for (const alias of binding.aliases) {
      const candidate = metadataRowValueByAlias(row, alias);
      if (!candidate.present) {
        continue;
      }
      if (binding.expectNull) {
        if (candidate.value === null) {
          matched = true;
          break;
        }
        continue;
      }
      if (candidate.value !== null && normalizeMetadataMatchText(candidate.value) === binding.expectedText) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      return false;
    }
  }
  return true;
}

function metadataRowValueByAlias(
  row: Record<string, unknown>,
  alias: string,
): { present: boolean; value: unknown } {
  if (Object.prototype.hasOwnProperty.call(row, alias)) {
    return { present: true, value: row[alias] };
  }

  const target = normalizeMetadataIdentifier(alias);
  for (const [key, value] of Object.entries(row)) {
    if (normalizeMetadataIdentifier(key) === target) {
      return { present: true, value };
    }
  }
  return { present: false, value: null };
}

function metadataRestrictionAliases(key: string): string[] {
  const canonical = normalizeMetadataIdentifier(key);
  const aliases = METADATA_RESTRICTION_KEY_ALIASES[canonical] ?? [];
  return [...new Set([...aliases, canonical].map((alias) => normalizeMetadataIdentifier(alias)).filter(Boolean))];
}

function normalizeMetadataIdentifier(value: string): string {
  return value.trim().toLowerCase().replace(/[^a-z0-9]/g, "");
}

function normalizeMetadataMatchText(value: unknown): string {
  return String(value).trim().toLowerCase();
}

function shapeMetadataRow<T extends Record<string, unknown>>(
  source: T,
  collectionName: MetadataCollectionName,
  catalogName: string | null,
): T {
  const row: Record<string, unknown> = { ...source };
  const schemaName = firstStringValue(row, [
    "schema_name",
    "table_schema",
    "table_schem",
    "TABLE_SCHEM",
    "TABLE_SCHEMA",
    "constraint_schema",
    "CONSTRAINT_SCHEMA",
    "routine_schema",
    "ROUTINE_SCHEMA",
  ]);
  const tableName = firstStringValue(row, ["table_name", "TABLE_NAME"]);
  const columnName = firstStringValue(row, ["column_name", "COLUMN_NAME"]);
  const constraintName = firstStringValue(row, ["constraint_name", "CONSTRAINT_NAME"]);
  const indexName = firstStringValue(row, ["index_name", "INDEX_NAME"]);
  const typeName = firstStringValue(row, ["type_name", "TYPE_NAME", "data_type_name"]);
  const tableType = firstStringValue(row, ["table_type", "TABLE_TYPE"]);
  const privilegeType = firstStringValue(row, ["privilege_type", "PRIVILEGE"]);
  const routineName = firstStringValue(row, ["routine_name", "ROUTINE_NAME", "procedure_name", "PROCEDURE_NAME", "function_name", "FUNCTION_NAME"]);
  const effectiveCatalog =
    firstStringValue(row, [
      "catalog_name",
      "table_catalog",
      "table_cat",
      "TABLE_CAT",
      "TABLE_CATALOG",
      "constraint_catalog",
      "CONSTRAINT_CATALOG",
      "routine_catalog",
      "ROUTINE_CATALOG",
    ]) ?? catalogName;
  const constraintType = firstStringValue(row, ["constraint_type", "CONSTRAINT_TYPE"]);
  const routineType = firstStringValue(row, ["routine_type", "ROUTINE_TYPE"]);

  if (effectiveCatalog) {
    assignIfMissing(row, "catalog_name", effectiveCatalog);
    assignIfMissing(row, "table_catalog", effectiveCatalog);
    assignIfMissing(row, "table_cat", effectiveCatalog);
    assignIfMissing(row, "TABLE_CAT", effectiveCatalog);
    assignIfMissing(row, "TABLE_CATALOG", effectiveCatalog);
  }

  if (schemaName) {
    assignIfMissing(row, "schema_name", schemaName);
    assignIfMissing(row, "table_schema", schemaName);
    assignIfMissing(row, "table_schem", schemaName);
    assignIfMissing(row, "TABLE_SCHEM", schemaName);
    assignIfMissing(row, "TABLE_SCHEMA", schemaName);
  }

  if (tableName) {
    assignIfMissing(row, "table_name", tableName);
    assignIfMissing(row, "TABLE_NAME", tableName);
  }

  if (columnName) {
    assignIfMissing(row, "column_name", columnName);
    assignIfMissing(row, "COLUMN_NAME", columnName);
  }

  if (typeName) {
    assignIfMissing(row, "type_name", typeName);
    assignIfMissing(row, "TYPE_NAME", typeName);
  }

  if (row.data_type_id !== undefined && row.data_type_id !== null) {
    assignIfMissing(row, "data_type", row.data_type_id);
    assignIfMissing(row, "DATA_TYPE", row.data_type_id);
  }

  if (collectionName === "tables") {
    if (tableType) {
      assignIfMissing(row, "TABLE_TYPE", tableType);
    }
  }

  if (collectionName === "columns") {
    if (row.ordinal_position !== undefined && row.ordinal_position !== null) {
      assignIfMissing(row, "ORDINAL_POSITION", row.ordinal_position);
    }
    if (row.is_nullable !== undefined && row.is_nullable !== null) {
      assignIfMissing(row, "IS_NULLABLE", normalizeNullableFlag(row.is_nullable));
    }
    if (row.default_value !== undefined) {
      assignIfMissing(row, "COLUMN_DEF", row.default_value);
    }
  }

  if (collectionName === "indexes") {
    if (indexName) {
      assignIfMissing(row, "INDEX_NAME", indexName);
    }
    if (row.is_unique !== undefined && row.is_unique !== null) {
      assignIfMissing(row, "NON_UNIQUE", row.is_unique ? 0 : 1);
    }
  }

  if (collectionName === "index_columns") {
    if (indexName) {
      assignIfMissing(row, "INDEX_NAME", indexName);
    }
    if (row.ordinal_position !== undefined && row.ordinal_position !== null) {
      assignIfMissing(row, "ORDINAL_POSITION", row.ordinal_position);
    }
  }

  if (collectionName === "constraints" || collectionName === "primary_keys" || collectionName === "foreign_keys") {
    if (constraintName) {
      assignIfMissing(row, "CONSTRAINT_NAME", constraintName);
    }
    if (constraintType) {
      assignIfMissing(row, "CONSTRAINT_TYPE", constraintType);
    }
    if (collectionName === "primary_keys") {
      assignIfMissing(row, "PK_NAME", constraintName);
    }
    if (collectionName === "foreign_keys") {
      assignIfMissing(row, "FK_NAME", constraintName);
    }
  }

  if (collectionName === "table_privileges" || collectionName === "column_privileges") {
    if (privilegeType) {
      assignIfMissing(row, "PRIVILEGE", privilegeType);
      assignIfMissing(row, "PRIVILEGE_TYPE", privilegeType);
    }
    if (row.is_grantable !== undefined && row.is_grantable !== null) {
      assignIfMissing(row, "IS_GRANTABLE", row.is_grantable);
    }
  }

  if (collectionName === "procedures" || collectionName === "functions" || collectionName === "routines") {
    if (routineName) {
      assignIfMissing(row, "ROUTINE_NAME", routineName);
    }
    if (routineType) {
      assignIfMissing(row, "ROUTINE_TYPE", routineType);
    }
  }

  if (collectionName === "type_info") {
    if (typeName) {
      assignIfMissing(row, "TYPE_NAME", typeName);
    }
    if (row.data_type_id !== undefined && row.data_type_id !== null) {
      assignIfMissing(row, "DATA_TYPE", row.data_type_id);
    }
  }

  return row as T;
}

function assignIfMissing(row: Record<string, unknown>, key: string, value: unknown): void {
  if (value === undefined || value === null || value === "") {
    return;
  }
  if (!Object.prototype.hasOwnProperty.call(row, key) || row[key] === undefined || row[key] === null) {
    row[key] = value;
  }
}

function firstStringValue(row: Record<string, unknown>, keys: readonly string[]): string | null {
  for (const key of keys) {
    const value = row[key];
    if (typeof value === "string") {
      const trimmed = value.trim();
      if (trimmed.length > 0) {
        return trimmed;
      }
    }
  }
  return null;
}

function metadataTypeMatches(
  row: Record<string, unknown>,
  keys: readonly string[],
  expectedValues: readonly string[],
): boolean {
  const value = firstStringValue(row, keys);
  if (!value) {
    return false;
  }
  const normalized = value.trim().toLowerCase();
  return expectedValues.some((candidate) => normalized === candidate);
}

function normalizeCatalogName(value: string | null | undefined): string | null {
  if (!value) {
    return null;
  }
  const trimmed = value.trim();
  return trimmed.length ? trimmed : null;
}

function normalizeNullableFlag(value: unknown): string {
  if (typeof value === "string") {
    const normalized = value.trim().toLowerCase();
    if (normalized === "yes" || normalized === "true" || normalized === "1") {
      return "YES";
    }
    if (normalized === "no" || normalized === "false" || normalized === "0") {
      return "NO";
    }
    return value;
  }
  if (typeof value === "number") {
    return value === 0 ? "NO" : "YES";
  }
  if (typeof value === "boolean") {
    return value ? "YES" : "NO";
  }
  return String(value);
}

function splitSchemaPath(value: string): string[] {
  return value
    .split(".")
    .map((segment) => segment.trim())
    .filter((segment) => segment.length > 0);
}

function readSchemaPath(row: MetadataSchemaInput): string | null {
  if (typeof row === "string") {
    return normalizeSchemaPath(row);
  }
  for (const candidate of SCHEMA_FIELD_CANDIDATES) {
    const value = row[candidate];
    if (typeof value === "string") {
      return normalizeSchemaPath(value);
    }
  }
  return null;
}

function normalizeSchemaPath(value: string): string | null {
  const normalized = splitSchemaPath(value).join(".");
  return normalized.length ? normalized : null;
}

function createSyntheticSchemaRow<T extends Record<string, unknown>>(sample: T, schemaPath: string): T {
  const synthetic: Record<string, unknown> = {};
  for (const key of Object.keys(sample)) {
    synthetic[key] = null;
  }
  let assigned = false;
  for (const key of SCHEMA_FIELD_CANDIDATES) {
    if (key in synthetic) {
      synthetic[key] = schemaPath;
      assigned = true;
    }
  }
  if (!assigned) {
    synthetic.schema_name = schemaPath;
  }
  return synthetic as T;
}
