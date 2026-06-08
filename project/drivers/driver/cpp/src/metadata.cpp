// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/client/metadata.h"

#include <cctype>
#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "nlohmann/json.hpp"

namespace scratchbird {
namespace client {

namespace {

std::string trim(std::string_view value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string(value.substr(start, end - start));
}

std::vector<std::string> splitSchemaPath(const std::string& schema_name) {
    std::vector<std::string> segments;
    size_t start = 0;
    while (start <= schema_name.size()) {
        size_t dot = schema_name.find('.', start);
        size_t end = (dot == std::string::npos) ? schema_name.size() : dot;
        std::string segment = trim(std::string_view(schema_name).substr(start, end - start));
        if (!segment.empty()) {
            segments.push_back(segment);
        }
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }
    return segments;
}

std::string normalizeSchemaPath(const std::string& schema_name) {
    std::vector<std::string> parts = splitSchemaPath(schema_name);
    std::string normalized;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            normalized.push_back('.');
        }
        normalized.append(parts[i]);
    }
    return normalized;
}

void appendRowsDepthFirst(const MetadataSchemaTreeNode& node,
                          const std::string& database_name,
                          const std::string& parent_path,
                          bool top_level,
                          std::vector<MetadataSchemaTreeRow>* rows) {
    if (!rows) {
        return;
    }

    rows->push_back(MetadataSchemaTreeRow{
        MetadataTreeRowKind::SCHEMA,
        database_name,
        parent_path,
        node.full_path,
        node.name,
        node.terminal,
        top_level});

    for (const auto& child : node.children) {
        if (!child) {
            continue;
        }
        appendRowsDepthFirst(*child, database_name, node.full_path, false, rows);
    }
}

constexpr const char* kDefaultMetadataCollection = "tables";
constexpr const char* kMetadataSchemasQuery =
    "SELECT schema_id, schema_name, owner_id, default_tablespace_id FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name";
constexpr const char* kMetadataTablesQuery =
    "SELECT table_id, schema_id, table_name, table_type, owner_id FROM sys.tables WHERE is_valid = 1 ORDER BY table_name";
constexpr const char* kMetadataColumnsQuery =
    "SELECT column_id, table_id, column_name, data_type_id, data_type_name, ordinal_position, is_nullable, default_value, domain_id, collation_id, charset_id, is_identity, is_generated, generation_expression FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position";
constexpr const char* kMetadataIndexesQuery =
    "SELECT index_id, table_id, index_name, index_type, is_unique FROM sys.indexes WHERE is_valid = 1 ORDER BY table_id, index_name";
constexpr const char* kMetadataIndexColumnsQuery =
    "SELECT index_id, column_id, column_name, ordinal_position, is_included FROM sys.index_columns ORDER BY index_id, ordinal_position";
constexpr const char* kMetadataConstraintsQuery =
    "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 ORDER BY table_id, constraint_name";
constexpr const char* kMetadataProceduresQuery =
    "SELECT procedure_id, schema_id, procedure_name, routine_type FROM sys.procedures WHERE is_valid = 1 ORDER BY schema_id, procedure_name";
constexpr const char* kMetadataFunctionsQuery =
    "SELECT function_id, schema_id, function_name FROM sys.functions WHERE is_valid = 1 ORDER BY schema_id, function_name";
constexpr const char* kMetadataRoutinesQuery =
    "SELECT procedure_id AS routine_id, schema_id, procedure_name AS routine_name, routine_type FROM sys.procedures WHERE is_valid = 1 UNION ALL SELECT function_id AS routine_id, schema_id, function_name AS routine_name, 'FUNCTION' AS routine_type FROM sys.functions WHERE is_valid = 1 ORDER BY schema_id, routine_name";
constexpr const char* kMetadataCatalogsQuery =
    "SELECT schema_id AS catalog_id, schema_name AS catalog_name FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name";
constexpr const char* kMetadataPrimaryKeysQuery =
    "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 AND lower(constraint_type) IN ('primary key', 'primary') ORDER BY table_id, constraint_name";
constexpr const char* kMetadataForeignKeysQuery =
    "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 AND lower(constraint_type) IN ('foreign key', 'foreign') ORDER BY table_id, constraint_name";
constexpr const char* kMetadataTablePrivilegesQuery =
    "SELECT table_id, table_name, owner_id AS grantor_id, owner_id AS grantee_id, 'ALL' AS privilege_type FROM sys.tables WHERE is_valid = 1 ORDER BY table_id, table_name";
constexpr const char* kMetadataColumnPrivilegesQuery =
    "SELECT table_id, column_id, column_name, 'ALL' AS privilege_type FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position";
constexpr const char* kMetadataTypeInfoQuery =
    "SELECT DISTINCT data_type_id, data_type_name FROM sys.columns WHERE is_valid = 1 ORDER BY data_type_name";
constexpr const char* kMetadataDdlFieldsQuery =
    "SELECT table_id, column_id, column_name, data_type_name, default_value, generation_expression, is_nullable, is_identity, is_generated FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position";

const std::unordered_map<std::string, std::string> kMetadataCollectionAliases = {
    {"schema", "schemas"},
    {"schemas", "schemas"},
    {"table", "tables"},
    {"tables", "tables"},
    {"column", "columns"},
    {"columns", "columns"},
    {"index", "indexes"},
    {"indexes", "indexes"},
    {"index_column", "index_columns"},
    {"index_columns", "index_columns"},
    {"indexcolumn", "index_columns"},
    {"indexcolumns", "index_columns"},
    {"constraint", "constraints"},
    {"constraints", "constraints"},
    {"procedure", "procedures"},
    {"procedures", "procedures"},
    {"function", "functions"},
    {"functions", "functions"},
    {"routine", "routines"},
    {"routines", "routines"},
    {"catalog", "catalogs"},
    {"catalogs", "catalogs"},
    {"primary_key", "primary_keys"},
    {"primary_keys", "primary_keys"},
    {"primarykey", "primary_keys"},
    {"primarykeys", "primary_keys"},
    {"foreign_key", "foreign_keys"},
    {"foreign_keys", "foreign_keys"},
    {"foreignkey", "foreign_keys"},
    {"foreignkeys", "foreign_keys"},
    {"table_privilege", "table_privileges"},
    {"table_privileges", "table_privileges"},
    {"tableprivilege", "table_privileges"},
    {"tableprivileges", "table_privileges"},
    {"column_privilege", "column_privileges"},
    {"column_privileges", "column_privileges"},
    {"columnprivilege", "column_privileges"},
    {"columnprivileges", "column_privileges"},
    {"type", "type_info"},
    {"types", "type_info"},
    {"type_info", "type_info"},
    {"typeinfo", "type_info"},
    {"ddlfield", "ddl_fields"},
    {"ddlfields", "ddl_fields"},
    {"ddl_field", "ddl_fields"},
    {"ddl_fields", "ddl_fields"},
};

const std::unordered_map<std::string, std::string> kMetadataCollectionQueries = {
    {"schemas", kMetadataSchemasQuery},
    {"tables", kMetadataTablesQuery},
    {"columns", kMetadataColumnsQuery},
    {"indexes", kMetadataIndexesQuery},
    {"index_columns", kMetadataIndexColumnsQuery},
    {"constraints", kMetadataConstraintsQuery},
    {"procedures", kMetadataProceduresQuery},
    {"functions", kMetadataFunctionsQuery},
    {"routines", kMetadataRoutinesQuery},
    {"catalogs", kMetadataCatalogsQuery},
    {"primary_keys", kMetadataPrimaryKeysQuery},
    {"foreign_keys", kMetadataForeignKeysQuery},
    {"table_privileges", kMetadataTablePrivilegesQuery},
    {"column_privileges", kMetadataColumnPrivilegesQuery},
    {"type_info", kMetadataTypeInfoQuery},
    {"ddl_fields", kMetadataDdlFieldsQuery},
};

std::string normalizeMetadataCollectionKey(const std::string& collection_name) {
    std::string normalized = trim(collection_name);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    std::replace(normalized.begin(), normalized.end(), '-', '_');
    std::replace(normalized.begin(), normalized.end(), ' ', '_');
    if (normalized.empty()) {
        normalized = kDefaultMetadataCollection;
    }
    return normalized;
}

std::string collapseUnderscores(const std::string& value) {
    std::string collapsed;
    collapsed.reserve(value.size());
    for (char ch : value) {
        if (ch == '_') {
            continue;
        }
        collapsed.push_back(ch);
    }
    return collapsed;
}

} // namespace

std::vector<std::string> metadataSchemaPathsForNavigation(
    const std::vector<std::string>& schema_names,
    bool expand_schema_parents) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;

    for (const std::string& raw : schema_names) {
        std::string normalized = normalizeSchemaPath(raw);
        if (normalized.empty()) {
            continue;
        }

        if (!expand_schema_parents) {
            if (seen.insert(normalized).second) {
                out.push_back(normalized);
            }
            continue;
        }

        std::string current;
        std::vector<std::string> parts = splitSchemaPath(normalized);
        for (size_t i = 0; i < parts.size(); ++i) {
            if (!current.empty()) {
                current.push_back('.');
            }
            current.append(parts[i]);
            if (seen.insert(current).second) {
                out.push_back(current);
            }
        }
    }

    return out;
}

MetadataSchemaTree buildMetadataSchemaTree(
    const std::vector<std::string>& schema_names,
    const std::string& database,
    bool expand_schema_parents) {
    MetadataSchemaTree tree;
    tree.database = trim(database);

    std::vector<std::string> schema_paths =
        metadataSchemaPathsForNavigation(schema_names, expand_schema_parents);
    std::unordered_set<std::string> terminal_paths(schema_paths.begin(), schema_paths.end());
    std::unordered_map<std::string, MetadataSchemaTreeNode*> nodes_by_path;

    for (const std::string& schema_path : schema_paths) {
        std::vector<std::string> parts = splitSchemaPath(schema_path);
        if (parts.empty()) {
            continue;
        }

        MetadataSchemaTreeNode* parent = nullptr;
        std::string current_path;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (!current_path.empty()) {
                current_path.push_back('.');
            }
            current_path.append(parts[i]);

            MetadataSchemaTreeNode* node = nullptr;
            auto existing = nodes_by_path.find(current_path);
            if (existing == nodes_by_path.end()) {
                auto created = std::make_unique<MetadataSchemaTreeNode>();
                created->name = parts[i];
                created->full_path = current_path;
                node = created.get();
                nodes_by_path[current_path] = node;

                if (parent == nullptr) {
                    tree.schemas.push_back(std::move(created));
                } else {
                    parent->children.push_back(std::move(created));
                }
            } else {
                node = existing->second;
            }

            if (terminal_paths.find(current_path) != terminal_paths.end()) {
                node->terminal = true;
            }
            parent = node;
        }
    }

    return tree;
}

std::vector<MetadataSchemaTreeRow> buildMetadataSchemaTreeRows(
    const std::vector<std::string>& schema_names,
    const std::string& database,
    bool expand_schema_parents) {
    MetadataSchemaTree tree = buildMetadataSchemaTree(schema_names, database, expand_schema_parents);

    std::string database_name = tree.database.empty() ? std::string("default") : tree.database;
    std::vector<MetadataSchemaTreeRow> rows;
    rows.push_back(MetadataSchemaTreeRow{
        MetadataTreeRowKind::DATABASE,
        database_name,
        std::string(),
        database_name,
        database_name,
        false,
        false});

    for (const auto& root : tree.schemas) {
        if (!root) {
            continue;
        }
        appendRowsDepthFirst(*root, database_name, database_name, true, &rows);
    }

    return rows;
}

bool normalizeMetadataCollectionName(
    const std::string& collection_name,
    std::string* normalized_collection) {
    if (!normalized_collection) {
        return false;
    }

    std::string normalized_key = normalizeMetadataCollectionKey(collection_name);
    auto alias_it = kMetadataCollectionAliases.find(normalized_key);
    if (alias_it == kMetadataCollectionAliases.end()) {
        const std::string collapsed = collapseUnderscores(normalized_key);
        alias_it = kMetadataCollectionAliases.find(collapsed);
    }
    if (alias_it == kMetadataCollectionAliases.end()) {
        return false;
    }

    *normalized_collection = alias_it->second;
    return true;
}

bool resolveMetadataCollectionQuery(
    const std::string& collection_name,
    std::string* query_sql,
    std::string* normalized_collection) {
    if (!query_sql) {
        return false;
    }

    std::string normalized;
    if (!normalizeMetadataCollectionName(collection_name, &normalized)) {
        return false;
    }

    auto query_it = kMetadataCollectionQueries.find(normalized);
    if (query_it == kMetadataCollectionQueries.end()) {
        return false;
    }

    *query_sql = query_it->second;
    if (normalized_collection) {
        *normalized_collection = normalized;
    }
    return true;
}

namespace {

bool hasPatternText(const std::string* pattern) {
    return pattern != nullptr && !trim(*pattern).empty();
}

std::string quoteSqlLiteral(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('\'');
    for (char ch : value) {
        if (ch == '\'') {
            out.push_back('\'');
        }
        out.push_back(ch);
    }
    out.push_back('\'');
    return out;
}

} // namespace

std::string buildMetadataSchemasQuerySql(const std::string* schema_pattern) {
    if (!hasPatternText(schema_pattern)) {
        return kMetadataSchemasQuery;
    }

    return "SELECT schema_id, schema_name, owner_id, default_tablespace_id "
           "FROM sys.schemas "
           "WHERE is_valid = 1 AND schema_name LIKE " +
           quoteSqlLiteral(trim(*schema_pattern)) +
           " ORDER BY schema_name";
}

std::string buildMetadataTablesQuerySql(const std::string* schema_pattern,
                                        const std::string* table_pattern) {
    if (!hasPatternText(schema_pattern) && !hasPatternText(table_pattern)) {
        return kMetadataTablesQuery;
    }

    std::string sql =
        "SELECT t.table_id, t.schema_id, t.table_name, t.table_type, t.owner_id "
        "FROM sys.tables t "
        "JOIN sys.schemas s ON s.schema_id = t.schema_id "
        "WHERE t.is_valid = 1 AND s.is_valid = 1";
    if (hasPatternText(schema_pattern)) {
        sql += " AND s.schema_name LIKE " + quoteSqlLiteral(trim(*schema_pattern));
    }
    if (hasPatternText(table_pattern)) {
        sql += " AND t.table_name LIKE " + quoteSqlLiteral(trim(*table_pattern));
    }
    sql += " ORDER BY s.schema_name, t.table_name";
    return sql;
}

std::string buildMetadataColumnsQuerySql(const std::string* schema_pattern,
                                         const std::string* table_pattern) {
    if (!hasPatternText(schema_pattern) && !hasPatternText(table_pattern)) {
        return kMetadataColumnsQuery;
    }

    std::string sql =
        "SELECT c.column_id, c.table_id, c.column_name, c.data_type_id, c.data_type_name, "
        "c.ordinal_position, c.is_nullable, c.default_value, c.domain_id, "
        "c.collation_id, c.charset_id, c.is_identity, c.is_generated, "
        "c.generation_expression "
        "FROM sys.columns c "
        "JOIN sys.tables t ON t.table_id = c.table_id "
        "JOIN sys.schemas s ON s.schema_id = t.schema_id "
        "WHERE c.is_valid = 1 AND t.is_valid = 1 AND s.is_valid = 1";
    if (hasPatternText(schema_pattern)) {
        sql += " AND s.schema_name LIKE " + quoteSqlLiteral(trim(*schema_pattern));
    }
    if (hasPatternText(table_pattern)) {
        sql += " AND t.table_name LIKE " + quoteSqlLiteral(trim(*table_pattern));
    }
    sql += " ORDER BY t.table_id, c.ordinal_position";
    return sql;
}

std::string buildMetadataIndexesQuerySql(const std::string* schema_pattern,
                                         const std::string* table_pattern) {
    if (!hasPatternText(schema_pattern) && !hasPatternText(table_pattern)) {
        return kMetadataIndexesQuery;
    }

    std::string sql =
        "SELECT i.index_id, i.table_id, i.index_name, i.index_type, i.is_unique "
        "FROM sys.indexes i "
        "JOIN sys.tables t ON t.table_id = i.table_id "
        "JOIN sys.schemas s ON s.schema_id = t.schema_id "
        "WHERE i.is_valid = 1 AND t.is_valid = 1 AND s.is_valid = 1";
    if (hasPatternText(schema_pattern)) {
        sql += " AND s.schema_name LIKE " + quoteSqlLiteral(trim(*schema_pattern));
    }
    if (hasPatternText(table_pattern)) {
        sql += " AND t.table_name LIKE " + quoteSqlLiteral(trim(*table_pattern));
    }
    sql += " ORDER BY s.schema_name, t.table_name, i.index_name";
    return sql;
}

std::string metadataCollectionNotSupportedMessage(const std::string& collection_name) {
    const std::string candidate = collection_name.empty()
        ? std::string(kDefaultMetadataCollection)
        : collection_name;
    return "metadata collection '" + candidate + "' is not supported";
}

namespace {

std::string normalizeLikeText(const std::string& input) {
    std::string out = input;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

bool likeMatchChars(const std::string& value,
                    const std::string& pattern,
                    size_t value_index,
                    size_t pattern_index) {
    while (pattern_index < pattern.size()) {
        char token = pattern[pattern_index];
        if (token == '%') {
            while (pattern_index + 1 < pattern.size() && pattern[pattern_index + 1] == '%') {
                ++pattern_index;
            }
            if (pattern_index + 1 == pattern.size()) {
                return true;
            }
            for (size_t cursor = value_index; cursor <= value.size(); ++cursor) {
                if (likeMatchChars(value, pattern, cursor, pattern_index + 1)) {
                    return true;
                }
            }
            return false;
        }
        if (token == '_') {
            if (value_index >= value.size()) {
                return false;
            }
            ++value_index;
            ++pattern_index;
            continue;
        }
        if (token == '\\' && pattern_index + 1 < pattern.size()) {
            ++pattern_index;
            token = pattern[pattern_index];
        }
        if (value_index >= value.size() || value[value_index] != token) {
            return false;
        }
        ++value_index;
        ++pattern_index;
    }
    return value_index == value.size();
}

bool metadataLikeMatch(const std::string& value, const std::string& pattern) {
    return likeMatchChars(
        normalizeLikeText(value),
        normalizeLikeText(pattern),
        0,
        0);
}

nlohmann::json schemaTreeNodesToPayload(
    const std::vector<std::unique_ptr<MetadataSchemaTreeNode>>& nodes) {
    nlohmann::json payload = nlohmann::json::array();
    for (const auto& node : nodes) {
        if (!node) {
            continue;
        }
        payload.push_back({
            {"name", node->name},
            {"path", node->full_path},
            {"terminal", node->terminal},
            {"children", schemaTreeNodesToPayload(node->children)},
        });
    }
    return payload;
}

} // namespace

std::string buildMetadataDdlEditorSchemaPayloadJson(
    const std::vector<std::string>& schema_names,
    const std::string* schema_pattern,
    bool expand_schema_parents) {
    std::vector<std::string> filtered;
    filtered.reserve(schema_names.size());
    std::unordered_set<std::string> seen;

    for (const auto& raw : schema_names) {
        const std::string normalized = normalizeSchemaPath(raw);
        if (normalized.empty()) {
            continue;
        }
        if (schema_pattern != nullptr && !metadataLikeMatch(normalized, *schema_pattern)) {
            continue;
        }
        if (seen.insert(normalized).second) {
            filtered.push_back(normalized);
        }
    }

    const std::vector<std::string> schema_paths =
        metadataSchemaPathsForNavigation(filtered, expand_schema_parents);
    const MetadataSchemaTree schema_tree =
        buildMetadataSchemaTree(filtered, "", expand_schema_parents);

    nlohmann::json payload = nlohmann::json::object();
    if (schema_pattern != nullptr) {
        payload["schemaPattern"] = *schema_pattern;
    } else {
        payload["schemaPattern"] = nullptr;
    }
    payload["expandSchemaParents"] = expand_schema_parents;
    payload["schemaPaths"] = schema_paths;
    payload["schemaTree"] = schemaTreeNodesToPayload(schema_tree.schemas);
    return payload.dump();
}

} // namespace client
} // namespace scratchbird
