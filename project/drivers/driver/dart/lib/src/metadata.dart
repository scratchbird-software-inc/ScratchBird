// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

const String metadataSchemasQuery =
    'SELECT schema_id, schema_name, owner_id, default_tablespace_id FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name';

const String metadataTablesQuery =
    'SELECT table_id, schema_id, table_name, table_type, owner_id FROM sys.tables WHERE is_valid = 1 ORDER BY table_name';

const String metadataColumnsQuery =
    'SELECT column_id, table_id, column_name, data_type_id, data_type_name, ordinal_position, is_nullable, default_value, domain_id, collation_id, charset_id, is_identity, is_generated, generation_expression FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position';

const String metadataIndexesQuery =
    'SELECT index_id, table_id, index_name, index_type, is_unique FROM sys.indexes WHERE is_valid = 1 ORDER BY table_id, index_name';

const String metadataIndexColumnsQuery =
    'SELECT index_id, column_id, column_name, ordinal_position, is_included FROM sys.index_columns ORDER BY index_id, ordinal_position';

const String metadataConstraintsQuery =
    'SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 ORDER BY table_id, constraint_name';

const String metadataProceduresQuery =
    "SELECT routine_schema AS schema_id, routine_schema AS schema_name, routine_schema AS table_schema, routine_schema AS table_schem, routine_name AS procedure_name, routine_name AS routine_name, routine_name AS specific_name, routine_type FROM information_schema.routines WHERE lower(routine_type) = 'procedure' ORDER BY schema_name, procedure_name";

const String metadataFunctionsQuery =
    "SELECT routine_schema AS schema_id, routine_schema AS schema_name, routine_schema AS table_schema, routine_schema AS table_schem, routine_name AS function_name, routine_name AS routine_name, routine_name AS specific_name, routine_type FROM information_schema.routines WHERE lower(routine_type) = 'function' ORDER BY schema_name, function_name";

const String metadataRoutinesQuery =
    "SELECT routine_schema AS schema_id, routine_schema AS schema_name, routine_schema AS table_schema, routine_schema AS table_schem, routine_name AS routine_name, routine_name AS specific_name, routine_type FROM information_schema.routines ORDER BY schema_name, routine_name";

const String metadataCatalogsQuery =
    'SELECT schema_id AS catalog_id, schema_name AS catalog_name FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name';

const String metadataPrimaryKeysQuery =
    "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 AND lower(constraint_type) IN ('primary key', 'primary') ORDER BY table_id, constraint_name";

const String metadataForeignKeysQuery =
    "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 AND lower(constraint_type) IN ('foreign key', 'foreign') ORDER BY table_id, constraint_name";

const String metadataTablePrivilegesQuery =
    "SELECT table_id, table_name, owner_id AS grantor_id, owner_id AS grantee_id, 'ALL' AS privilege_type FROM sys.tables WHERE is_valid = 1 ORDER BY table_id, table_name";

const String metadataColumnPrivilegesQuery =
    "SELECT table_id, column_id, column_name, 'ALL' AS privilege_type FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position";

const String metadataTypeInfoQuery =
    'SELECT DISTINCT data_type_id, data_type_name, data_type_name AS type_name FROM sys.columns WHERE is_valid = 1 ORDER BY data_type_name';

enum MetadataCollectionName {
  schemas,
  tables,
  columns,
  indexes,
  indexColumns,
  constraints,
  procedures,
  functions,
  routines,
  catalogs,
  primaryKeys,
  foreignKeys,
  tablePrivileges,
  columnPrivileges,
  typeInfo,
}

const Map<MetadataCollectionName, String> _metadataCollectionQueries = {
  MetadataCollectionName.schemas: metadataSchemasQuery,
  MetadataCollectionName.tables: metadataTablesQuery,
  MetadataCollectionName.columns: metadataColumnsQuery,
  MetadataCollectionName.indexes: metadataIndexesQuery,
  MetadataCollectionName.indexColumns: metadataIndexColumnsQuery,
  MetadataCollectionName.constraints: metadataConstraintsQuery,
  MetadataCollectionName.procedures: metadataProceduresQuery,
  MetadataCollectionName.functions: metadataFunctionsQuery,
  MetadataCollectionName.routines: metadataRoutinesQuery,
  MetadataCollectionName.catalogs: metadataCatalogsQuery,
  MetadataCollectionName.primaryKeys: metadataPrimaryKeysQuery,
  MetadataCollectionName.foreignKeys: metadataForeignKeysQuery,
  MetadataCollectionName.tablePrivileges: metadataTablePrivilegesQuery,
  MetadataCollectionName.columnPrivileges: metadataColumnPrivilegesQuery,
  MetadataCollectionName.typeInfo: metadataTypeInfoQuery,
};

const Map<String, MetadataCollectionName> _metadataCollectionAliases = {
  'schemas': MetadataCollectionName.schemas,
  'schema': MetadataCollectionName.schemas,
  'tables': MetadataCollectionName.tables,
  'table': MetadataCollectionName.tables,
  'columns': MetadataCollectionName.columns,
  'column': MetadataCollectionName.columns,
  'indexes': MetadataCollectionName.indexes,
  'index': MetadataCollectionName.indexes,
  'indexcolumns': MetadataCollectionName.indexColumns,
  'index_columns': MetadataCollectionName.indexColumns,
  'constraints': MetadataCollectionName.constraints,
  'constraint': MetadataCollectionName.constraints,
  'procedures': MetadataCollectionName.procedures,
  'procedure': MetadataCollectionName.procedures,
  'functions': MetadataCollectionName.functions,
  'function': MetadataCollectionName.functions,
  'routines': MetadataCollectionName.routines,
  'routine': MetadataCollectionName.routines,
  'catalogs': MetadataCollectionName.catalogs,
  'catalog': MetadataCollectionName.catalogs,
  'primary_keys': MetadataCollectionName.primaryKeys,
  'primary_key': MetadataCollectionName.primaryKeys,
  'primarykeys': MetadataCollectionName.primaryKeys,
  'primarykey': MetadataCollectionName.primaryKeys,
  'pk': MetadataCollectionName.primaryKeys,
  'foreign_keys': MetadataCollectionName.foreignKeys,
  'foreign_key': MetadataCollectionName.foreignKeys,
  'foreignkeys': MetadataCollectionName.foreignKeys,
  'foreignkey': MetadataCollectionName.foreignKeys,
  'fk': MetadataCollectionName.foreignKeys,
  'table_privileges': MetadataCollectionName.tablePrivileges,
  'table_privilege': MetadataCollectionName.tablePrivileges,
  'tableprivileges': MetadataCollectionName.tablePrivileges,
  'tableprivilege': MetadataCollectionName.tablePrivileges,
  'column_privileges': MetadataCollectionName.columnPrivileges,
  'column_privilege': MetadataCollectionName.columnPrivileges,
  'columnprivileges': MetadataCollectionName.columnPrivileges,
  'columnprivilege': MetadataCollectionName.columnPrivileges,
  'type_info': MetadataCollectionName.typeInfo,
  'typeinfo': MetadataCollectionName.typeInfo,
  'types': MetadataCollectionName.typeInfo,
};

const List<String> _schemaFieldCandidates = [
  'schema_name',
  'TABLE_SCHEM',
  'table_schem',
  'table_schema',
  'TABLE_SCHEMA',
  'schema',
];

const Set<String> _schemaFieldCandidatesLower = {
  'schema_name',
  'table_schem',
  'table_schema',
  'schema',
};

const Set<String> _catalogFieldCandidatesLower = {
  'table_catalog',
  'table_cat',
  'catalog',
  'database',
};

class MetadataSchemaTreeNode {
  final String name;
  final String path;
  bool terminal;
  final List<MetadataSchemaTreeNode> children;

  MetadataSchemaTreeNode({
    required this.name,
    required this.path,
    this.terminal = false,
    List<MetadataSchemaTreeNode>? children,
  }) : children = children ?? <MetadataSchemaTreeNode>[];
}

class MetadataSchemaTree {
  final String? database;
  final List<MetadataSchemaTreeNode> schemas;

  const MetadataSchemaTree({
    required this.database,
    required this.schemas,
  });
}

MetadataCollectionName normalizeMetadataCollectionName(
    [String? collectionName]) {
  final normalized = (collectionName ?? 'tables').trim().toLowerCase();
  final resolved = _metadataCollectionAliases[normalized];
  if (resolved != null) {
    return resolved;
  }
  throw ArgumentError(
      "Metadata collection '${collectionName ?? ''}' is not supported");
}

String resolveMetadataCollectionQuery([String? collectionName]) {
  return _metadataCollectionQueries[
      normalizeMetadataCollectionName(collectionName)]!;
}

List<String> expandSchemaPaths(Iterable<String> schemaPaths) {
  final out = <String>[];
  final seen = <String>{};
  for (final rawPath in schemaPaths) {
    final normalized = normalizeSchemaPath(rawPath);
    if (normalized == null) {
      continue;
    }
    var current = '';
    for (final segment in splitSchemaPath(normalized)) {
      current = current.isEmpty ? segment : '$current.$segment';
      if (seen.add(current)) {
        out.add(current);
      }
    }
  }
  return out;
}

List<String> listMetadataSchemaPaths(
  Iterable<Object?> rows, {
  bool expandParents = false,
}) {
  final deduped = <String>[];
  final seen = <String>{};
  for (final row in rows) {
    final schemaPath = readMetadataSchemaPath(row);
    if (schemaPath == null) {
      continue;
    }
    if (seen.add(schemaPath)) {
      deduped.add(schemaPath);
    }
  }
  return expandParents ? expandSchemaPaths(deduped) : deduped;
}

MetadataSchemaTree buildMetadataSchemaTree(
  Iterable<Object?> rows, {
  bool expandParents = false,
  String? database,
}) {
  final basePaths = listMetadataSchemaPaths(rows);
  final expandedPaths =
      expandParents ? expandSchemaPaths(basePaths) : basePaths;
  final terminalPaths = (expandParents ? expandedPaths : basePaths).toSet();
  final nodesByPath = <String, MetadataSchemaTreeNode>{};
  final roots = <MetadataSchemaTreeNode>[];

  for (final schemaPath in expandedPaths) {
    MetadataSchemaTreeNode? parent;
    var currentPath = '';
    for (final segment in splitSchemaPath(schemaPath)) {
      currentPath = currentPath.isEmpty ? segment : '$currentPath.$segment';
      var node = nodesByPath[currentPath];
      if (node == null) {
        node = MetadataSchemaTreeNode(name: segment, path: currentPath);
        nodesByPath[currentPath] = node;
        if (parent == null) {
          roots.add(node);
        } else {
          parent.children.add(node);
        }
      }
      if (terminalPaths.contains(currentPath)) {
        node.terminal = true;
      }
      parent = node;
    }
  }

  final normalizedDatabase = database?.trim();
  return MetadataSchemaTree(
    database: normalizedDatabase != null && normalizedDatabase.isNotEmpty
        ? normalizedDatabase
        : null,
    schemas: roots,
  );
}

List<Map<String, dynamic>> expandSchemaMetadataRows(
  Iterable<Map<String, dynamic>> rows, {
  bool expandParents = true,
}) {
  final out = <Map<String, dynamic>>[];
  if (!expandParents) {
    for (final row in rows) {
      out.add(Map<String, dynamic>.from(row));
    }
    return out;
  }

  final seen = <String>{};
  for (final row in rows) {
    final schemaPath = readMetadataSchemaPath(row);
    if (schemaPath == null) {
      out.add(Map<String, dynamic>.from(row));
      continue;
    }

    final segments = splitSchemaPath(schemaPath);
    var current = '';
    final catalog = readMetadataCatalog(row);
    for (var i = 0; i < segments.length; i++) {
      current = current.isEmpty ? segments[i] : '$current.${segments[i]}';
      final dedupKey = catalog == null ? current : '$catalog::$current';
      if (!seen.add(dedupKey)) {
        continue;
      }
      if (i == segments.length - 1) {
        final leaf = Map<String, dynamic>.from(row);
        _assignSchemaPath(leaf, current);
        out.add(leaf);
      } else {
        out.add(_createSyntheticSchemaRow(row, current));
      }
    }
  }

  return out;
}

String? readMetadataSchemaPath(Object? row) {
  if (row is String) {
    return normalizeSchemaPath(row);
  }
  if (row is! Map) {
    return null;
  }

  final key = _findSchemaKeyInMap(row);
  if (key == null) {
    return null;
  }
  return normalizeSchemaPath(row[key]);
}

String? readMetadataCatalog(Map row) {
  final key = _findCatalogKeyInMap(row);
  if (key == null) {
    return null;
  }
  final value = row[key];
  if (value is! String) {
    return null;
  }
  final trimmed = value.trim();
  return trimmed.isEmpty ? null : trimmed;
}

List<String> splitSchemaPath(String value) {
  return value
      .split('.')
      .map((segment) => segment.trim())
      .where((segment) => segment.isNotEmpty)
      .toList(growable: false);
}

String? normalizeSchemaPath(Object? value) {
  if (value is! String) {
    return null;
  }
  final normalized = splitSchemaPath(value).join('.');
  return normalized.isEmpty ? null : normalized;
}

Map<String, dynamic> _createSyntheticSchemaRow(
  Map<String, dynamic> sample,
  String schemaPath,
) {
  final synthetic = <String, dynamic>{};
  for (final key in sample.keys) {
    synthetic[key] = null;
  }

  _assignSchemaPath(synthetic, schemaPath);
  for (final key in _catalogKeysInMap(sample)) {
    synthetic[key] = sample[key];
  }
  return synthetic;
}

void _assignSchemaPath(Map<String, dynamic> row, String schemaPath) {
  final schemaKeys = _schemaKeysInMap(row);
  if (schemaKeys.isEmpty) {
    row['schema_name'] = schemaPath;
    return;
  }
  for (final key in schemaKeys) {
    row[key] = schemaPath;
  }
}

Object? _findSchemaKeyInMap(Map row) {
  for (final candidate in _schemaFieldCandidates) {
    for (final key in row.keys) {
      if (key is String && key.toLowerCase() == candidate.toLowerCase()) {
        return key;
      }
    }
  }
  return null;
}

Object? _findCatalogKeyInMap(Map row) {
  for (final key in row.keys) {
    if (key is String &&
        _catalogFieldCandidatesLower.contains(key.toLowerCase())) {
      return key;
    }
  }
  return null;
}

List<String> _schemaKeysInMap(Map<String, dynamic> row) {
  final keys = <String>[];
  for (final key in row.keys) {
    if (_schemaFieldCandidatesLower.contains(key.toLowerCase())) {
      keys.add(key);
    }
  }
  return keys;
}

List<String> _catalogKeysInMap(Map<String, dynamic> row) {
  final keys = <String>[];
  for (final key in row.keys) {
    if (_catalogFieldCandidatesLower.contains(key.toLowerCase())) {
      keys.add(key);
    }
  }
  return keys;
}
