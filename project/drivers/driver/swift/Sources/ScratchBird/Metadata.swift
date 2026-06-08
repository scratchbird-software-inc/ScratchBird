// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import Foundation

public enum ScratchBirdMetadata {
    public static let schemasQuery =
        "SELECT schema_id, schema_name, owner_id, default_tablespace_id FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name"

    public static let tablesQuery =
        "SELECT table_id, schema_id, table_name, table_type, owner_id FROM sys.tables WHERE is_valid = 1 ORDER BY table_name"

    public static let columnsQuery =
        "SELECT column_id, table_id, column_name, data_type_id, data_type_name, ordinal_position, is_nullable, default_value, domain_id, collation_id, charset_id, is_identity, is_generated, generation_expression FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position"

    public static let indexesQuery =
        "SELECT index_id, table_id, index_name, index_type, is_unique FROM sys.indexes WHERE is_valid = 1 ORDER BY table_id, index_name"

    public static let indexColumnsQuery =
        "SELECT index_id, column_id, column_name, ordinal_position, is_included FROM sys.index_columns ORDER BY index_id, ordinal_position"

    public static let constraintsQuery =
        "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 ORDER BY table_id, constraint_name"

    public static let proceduresQuery =
        "SELECT routine_schema AS schema_id, routine_schema AS schema_name, routine_schema AS table_schema, routine_schema AS table_schem, routine_name AS procedure_name, routine_name AS routine_name, routine_name AS specific_name, routine_type FROM information_schema.routines WHERE lower(routine_type) = 'procedure' ORDER BY schema_name, procedure_name"

    public static let functionsQuery =
        "SELECT routine_schema AS schema_id, routine_schema AS schema_name, routine_schema AS table_schema, routine_schema AS table_schem, routine_name AS function_name, routine_name AS routine_name, routine_name AS specific_name, routine_type FROM information_schema.routines WHERE lower(routine_type) = 'function' ORDER BY schema_name, function_name"

    public static let routinesQuery =
        "SELECT routine_schema AS schema_id, routine_schema AS schema_name, routine_schema AS table_schema, routine_schema AS table_schem, routine_name AS routine_name, routine_name AS specific_name, routine_type FROM information_schema.routines ORDER BY schema_name, routine_name"

    public static let catalogsQuery =
        "SELECT schema_id AS catalog_id, schema_name AS catalog_name FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name"

    public static let primaryKeysQuery =
        "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 AND lower(constraint_type) IN ('primary key', 'primary') ORDER BY table_id, constraint_name"

    public static let foreignKeysQuery =
        "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 AND lower(constraint_type) IN ('foreign key', 'foreign') ORDER BY table_id, constraint_name"

    public static let tablePrivilegesQuery =
        "SELECT table_id, table_name, owner_id AS grantor_id, owner_id AS grantee_id, 'ALL' AS privilege_type FROM sys.tables WHERE is_valid = 1 ORDER BY table_id, table_name"

    public static let columnPrivilegesQuery =
        "SELECT table_id, column_id, column_name, 'ALL' AS privilege_type FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position"

    public static let typeInfoQuery =
        "SELECT DISTINCT data_type_id, data_type_name, data_type_name AS type_name FROM sys.columns WHERE is_valid = 1 ORDER BY data_type_name"
}

public enum ScratchBirdMetadataCollectionName: String {
    case schemas
    case tables
    case columns
    case indexes
    case indexColumns = "index_columns"
    case constraints
    case procedures
    case functions
    case routines
    case catalogs
    case primaryKeys = "primary_keys"
    case foreignKeys = "foreign_keys"
    case tablePrivileges = "table_privileges"
    case columnPrivileges = "column_privileges"
    case typeInfo = "type_info"
}

private let metadataCollectionQueries: [ScratchBirdMetadataCollectionName: String] = [
    .schemas: ScratchBirdMetadata.schemasQuery,
    .tables: ScratchBirdMetadata.tablesQuery,
    .columns: ScratchBirdMetadata.columnsQuery,
    .indexes: ScratchBirdMetadata.indexesQuery,
    .indexColumns: ScratchBirdMetadata.indexColumnsQuery,
    .constraints: ScratchBirdMetadata.constraintsQuery,
    .procedures: ScratchBirdMetadata.proceduresQuery,
    .functions: ScratchBirdMetadata.functionsQuery,
    .routines: ScratchBirdMetadata.routinesQuery,
    .catalogs: ScratchBirdMetadata.catalogsQuery,
    .primaryKeys: ScratchBirdMetadata.primaryKeysQuery,
    .foreignKeys: ScratchBirdMetadata.foreignKeysQuery,
    .tablePrivileges: ScratchBirdMetadata.tablePrivilegesQuery,
    .columnPrivileges: ScratchBirdMetadata.columnPrivilegesQuery,
    .typeInfo: ScratchBirdMetadata.typeInfoQuery,
]

private let metadataCollectionAliases: [String: ScratchBirdMetadataCollectionName] = [
    "schema": .schemas,
    "schemas": .schemas,
    "table": .tables,
    "tables": .tables,
    "column": .columns,
    "columns": .columns,
    "index": .indexes,
    "indexes": .indexes,
    "indexcolumn": .indexColumns,
    "indexcolumns": .indexColumns,
    "index_column": .indexColumns,
    "index_columns": .indexColumns,
    "constraint": .constraints,
    "constraints": .constraints,
    "procedure": .procedures,
    "procedures": .procedures,
    "function": .functions,
    "functions": .functions,
    "routine": .routines,
    "routines": .routines,
    "catalog": .catalogs,
    "catalogs": .catalogs,
    "primary_key": .primaryKeys,
    "primary_keys": .primaryKeys,
    "primarykey": .primaryKeys,
    "primarykeys": .primaryKeys,
    "pk": .primaryKeys,
    "foreign_key": .foreignKeys,
    "foreign_keys": .foreignKeys,
    "foreignkey": .foreignKeys,
    "foreignkeys": .foreignKeys,
    "fk": .foreignKeys,
    "table_privilege": .tablePrivileges,
    "table_privileges": .tablePrivileges,
    "tableprivilege": .tablePrivileges,
    "tableprivileges": .tablePrivileges,
    "column_privilege": .columnPrivileges,
    "column_privileges": .columnPrivileges,
    "columnprivilege": .columnPrivileges,
    "columnprivileges": .columnPrivileges,
    "type_info": .typeInfo,
    "typeinfo": .typeInfo,
    "types": .typeInfo,
]

public func normalizeMetadataCollectionName(_ collectionName: String? = nil) throws -> ScratchBirdMetadataCollectionName {
    let raw = (collectionName ?? "tables").trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    if let resolved = metadataCollectionAliases[raw] {
        return resolved
    }
    throw ScratchBirdNotSupportedException(message: "metadata collection '\(collectionName ?? "")' is not supported", sqlState: "0A000")
}

public func resolveMetadataCollectionQuery(_ collectionName: String? = nil) throws -> String {
    let resolved = try normalizeMetadataCollectionName(collectionName)
    return metadataCollectionQueries[resolved]!
}

public func metadataSchemaNames(from result: ScratchBirdResult) -> [String] {
    let index = metadataSchemaNameColumnIndex(result.columns) ?? inferredSchemaNameColumnIndex(result.rows)
    guard let index else { return [] }

    var out: [String] = []
    var seen: Set<String> = []
    for row in result.rows {
        if index >= row.count {
            continue
        }
        guard let value = metadataStringValue(row[index]) else {
            continue
        }
        let normalized = value.trimmingCharacters(in: .whitespacesAndNewlines)
        if normalized.isEmpty {
            continue
        }
        if seen.insert(normalized).inserted {
            out.append(normalized)
        }
    }
    return out
}

public enum ScratchBirdMetadataTreeRowKind: Equatable {
    case database
    case schema
}

public struct ScratchBirdMetadataSchemaTreeRow {
    public let kind: ScratchBirdMetadataTreeRowKind
    public let database: String
    public let parentPath: String
    public let path: String
    public let name: String
    public let terminal: Bool
    public let topLevelBranch: Bool

    public init(
        kind: ScratchBirdMetadataTreeRowKind,
        database: String,
        parentPath: String,
        path: String,
        name: String,
        terminal: Bool,
        topLevelBranch: Bool
    ) {
        self.kind = kind
        self.database = database
        self.parentPath = parentPath
        self.path = path
        self.name = name
        self.terminal = terminal
        self.topLevelBranch = topLevelBranch
    }
}

public final class ScratchBirdMetadataSchemaTreeNode {
    public let name: String
    public let path: String
    public var terminal: Bool
    public var children: [ScratchBirdMetadataSchemaTreeNode]

    public init(
        name: String,
        path: String,
        terminal: Bool = false,
        children: [ScratchBirdMetadataSchemaTreeNode] = []
    ) {
        self.name = name
        self.path = path
        self.terminal = terminal
        self.children = children
    }
}

public struct ScratchBirdMetadataSchemaTree {
    public let database: String?
    public let schemas: [ScratchBirdMetadataSchemaTreeNode]

    public init(database: String?, schemas: [ScratchBirdMetadataSchemaTreeNode]) {
        self.database = database
        self.schemas = schemas
    }
}

public func metadataSchemaPathsForNavigation(
    _ schemaNames: [String],
    expandSchemaParents: Bool = false
) -> [String] {
    var out: [String] = []
    var seen: Set<String> = []

    for raw in schemaNames {
        guard let normalized = normalizeMetadataSchemaPath(raw) else {
            continue
        }

        if !expandSchemaParents {
            if seen.insert(normalized).inserted {
                out.append(normalized)
            }
            continue
        }

        var current = ""
        for part in splitMetadataSchemaPath(normalized) {
            current = current.isEmpty ? part : "\(current).\(part)"
            if seen.insert(current).inserted {
                out.append(current)
            }
        }
    }

    return out
}

public func buildMetadataSchemaTree(
    _ schemaNames: [String],
    database: String = "",
    expandSchemaParents: Bool = false
) -> ScratchBirdMetadataSchemaTree {
    let schemaPaths = metadataSchemaPathsForNavigation(schemaNames, expandSchemaParents: expandSchemaParents)
    let terminalPaths = Set(schemaPaths)
    var nodesByPath: [String: ScratchBirdMetadataSchemaTreeNode] = [:]
    var roots: [ScratchBirdMetadataSchemaTreeNode] = []

    for schemaPath in schemaPaths {
        var parent: ScratchBirdMetadataSchemaTreeNode?
        var currentPath = ""
        for part in splitMetadataSchemaPath(schemaPath) {
            currentPath = currentPath.isEmpty ? part : "\(currentPath).\(part)"
            let node: ScratchBirdMetadataSchemaTreeNode
            if let existing = nodesByPath[currentPath] {
                node = existing
            } else {
                node = ScratchBirdMetadataSchemaTreeNode(name: part, path: currentPath)
                nodesByPath[currentPath] = node
                if let parent {
                    parent.children.append(node)
                } else {
                    roots.append(node)
                }
            }
            if terminalPaths.contains(currentPath) {
                node.terminal = true
            }
            parent = node
        }
    }

    let trimmedDatabase = database.trimmingCharacters(in: .whitespacesAndNewlines)
    let normalizedDatabase = trimmedDatabase.isEmpty ? nil : trimmedDatabase
    return ScratchBirdMetadataSchemaTree(database: normalizedDatabase, schemas: roots)
}

public func buildMetadataSchemaTreeRows(
    _ schemaNames: [String],
    database: String = "",
    expandSchemaParents: Bool = false
) -> [ScratchBirdMetadataSchemaTreeRow] {
    let tree = buildMetadataSchemaTree(
        schemaNames,
        database: database,
        expandSchemaParents: expandSchemaParents
    )
    let databaseName = tree.database ?? "default"
    var rows: [ScratchBirdMetadataSchemaTreeRow] = [
        ScratchBirdMetadataSchemaTreeRow(
            kind: .database,
            database: databaseName,
            parentPath: "",
            path: databaseName,
            name: databaseName,
            terminal: false,
            topLevelBranch: false
        )
    ]

    for root in tree.schemas {
        appendMetadataTreeRowsDepthFirst(
            root,
            databaseName: databaseName,
            parentPath: databaseName,
            topLevel: true,
            rows: &rows
        )
    }

    return rows
}

public func splitMetadataSchemaPath(_ schemaName: String) -> [String] {
    schemaName
        .split(separator: ".", omittingEmptySubsequences: false)
        .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
        .filter { !$0.isEmpty }
}

public func normalizeMetadataSchemaPath(_ schemaName: String) -> String? {
    let normalized = splitMetadataSchemaPath(schemaName).joined(separator: ".")
    return normalized.isEmpty ? nil : normalized
}

private func appendMetadataTreeRowsDepthFirst(
    _ node: ScratchBirdMetadataSchemaTreeNode,
    databaseName: String,
    parentPath: String,
    topLevel: Bool,
    rows: inout [ScratchBirdMetadataSchemaTreeRow]
) {
    rows.append(
        ScratchBirdMetadataSchemaTreeRow(
            kind: .schema,
            database: databaseName,
            parentPath: parentPath,
            path: node.path,
            name: node.name,
            terminal: node.terminal,
            topLevelBranch: topLevel
        )
    )
    for child in node.children {
        appendMetadataTreeRowsDepthFirst(
            child,
            databaseName: databaseName,
            parentPath: node.path,
            topLevel: false,
            rows: &rows
        )
    }
}

private func metadataSchemaNameColumnIndex(_ columns: [ScratchBirdColumn]) -> Int? {
    for (index, column) in columns.enumerated() {
        let lowered = column.name.lowercased()
        if lowered == "schema_name" || lowered == "nspname" || lowered == "schema" {
            return index
        }
    }
    return nil
}

private func inferredSchemaNameColumnIndex(_ rows: [[Any?]]) -> Int? {
    guard let first = rows.first else { return nil }
    if first.count > 1 {
        return 1
    }
    if first.count == 1 {
        return 0
    }
    return nil
}

private func metadataStringValue(_ value: Any?) -> String? {
    guard let value else { return nil }
    if let text = value as? String {
        return text
    }
    if let raw = value as? RawValue {
        return String(data: raw.data, encoding: .utf8)
    }
    if let data = value as? Data {
        return String(data: data, encoding: .utf8)
    }
    if let value = value as? CustomStringConvertible {
        return value.description
    }
    return nil
}
