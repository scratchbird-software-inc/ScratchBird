// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace scratchbird::odbc::metadata {

static constexpr const char* kSchemasQuery =
    "SELECT schema_name FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name";

static constexpr const char* kTablesQuery =
    "SELECT t.table_name, s.schema_name, t.table_type FROM sys.tables t "
    "JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE t.is_valid = 1 "
    "ORDER BY t.table_name";

static constexpr const char* kColumnsQuery =
    "SELECT c.column_name, t.table_name, s.schema_name, c.data_type_id, "
    "c.ordinal_position, c.is_nullable, c.default_value FROM sys.columns c "
    "JOIN sys.tables t ON t.table_id = c.table_id "
    "JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE c.is_valid = 1 "
    "ORDER BY s.schema_name, t.table_name, c.ordinal_position";

struct MetadataSchemaTreeNode {
    std::string name;
    std::string full_path;
    bool terminal{false};
    std::vector<std::unique_ptr<MetadataSchemaTreeNode>> children;
};

struct MetadataSchemaTree {
    std::string database;
    std::vector<std::unique_ptr<MetadataSchemaTreeNode>> schemas;
};

enum class MetadataTreeRowKind : uint8_t {
    DATABASE = 0,
    SCHEMA = 1
};

struct MetadataSchemaTreeRow {
    MetadataTreeRowKind kind{MetadataTreeRowKind::SCHEMA};
    std::string database;
    std::string parent_path;
    std::string path;
    std::string name;
    bool terminal{false};
    bool top_level_branch{false};
};

namespace detail {

inline std::string trimSchemaSegment(const std::string& value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(start, end - start);
}

inline std::vector<std::string> splitSchemaPath(const std::string& schema_path) {
    std::vector<std::string> parts;
    if (schema_path.empty()) {
        return parts;
    }

    std::size_t start = 0;
    while (start <= schema_path.size()) {
        std::size_t dot = schema_path.find('.', start);
        std::size_t end = dot == std::string::npos ? schema_path.size() : dot;
        std::string part = trimSchemaSegment(schema_path.substr(start, end - start));
        if (!part.empty()) {
            parts.push_back(part);
        }
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }

    return parts;
}

inline std::string normalizeSchemaPath(const std::string& schema_path) {
    std::vector<std::string> parts = splitSchemaPath(schema_path);
    std::string normalized;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            normalized.push_back('.');
        }
        normalized.append(parts[i]);
    }
    return normalized;
}

inline void appendRowsDepthFirst(const MetadataSchemaTreeNode& node,
                                 const std::string& database_name,
                                 const std::string& parent_path,
                                 bool top_level_branch,
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
        top_level_branch
    });

    for (const auto& child : node.children) {
        if (!child) {
            continue;
        }
        appendRowsDepthFirst(*child, database_name, node.full_path, false, rows);
    }
}

}  // namespace detail

inline std::string normalizeSchemaPath(const std::string& schema_path) {
    return detail::normalizeSchemaPath(schema_path);
}

inline std::vector<std::string> metadataSchemaPathsForNavigation(
    const std::vector<std::string>& schema_names,
    bool expand_schema_parents) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;

    for (const auto& raw_schema : schema_names) {
        const std::string normalized = detail::normalizeSchemaPath(raw_schema);
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
        for (const auto& part : detail::splitSchemaPath(normalized)) {
            if (!current.empty()) {
                current.push_back('.');
            }
            current.append(part);
            if (seen.insert(current).second) {
                out.push_back(current);
            }
        }
    }

    return out;
}

inline MetadataSchemaTree buildMetadataSchemaTree(
    const std::vector<std::string>& schema_names,
    const std::string& database,
    bool expand_schema_parents) {
    MetadataSchemaTree tree;
    tree.database = detail::trimSchemaSegment(database);

    std::vector<std::string> normalized_paths;
    normalized_paths.reserve(schema_names.size());
    std::unordered_set<std::string> terminal_paths;
    terminal_paths.reserve(schema_names.size());
    for (const auto& raw_schema : schema_names) {
        const auto normalized = detail::normalizeSchemaPath(raw_schema);
        if (normalized.empty()) {
            continue;
        }
        if (terminal_paths.insert(normalized).second) {
            normalized_paths.push_back(normalized);
        }
    }

    const std::vector<std::string> schema_paths =
        metadataSchemaPathsForNavigation(normalized_paths, expand_schema_parents);

    std::unordered_map<std::string, MetadataSchemaTreeNode*> nodes_by_path;
    nodes_by_path.reserve(schema_paths.size());

    for (const auto& schema_path : schema_paths) {
        const auto parts = detail::splitSchemaPath(schema_path);
        if (parts.empty()) {
            continue;
        }

        MetadataSchemaTreeNode* parent = nullptr;
        std::string current_path;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (!current_path.empty()) {
                current_path.push_back('.');
            }
            current_path.append(parts[i]);

            MetadataSchemaTreeNode* node = nullptr;
            auto existing = nodes_by_path.find(current_path);
            if (existing != nodes_by_path.end()) {
                node = existing->second;
            } else {
                std::unique_ptr<MetadataSchemaTreeNode> created(new MetadataSchemaTreeNode());
                created->name = parts[i];
                created->full_path = current_path;
                node = created.get();
                nodes_by_path[current_path] = node;
                if (parent) {
                    parent->children.push_back(std::move(created));
                } else {
                    tree.schemas.push_back(std::move(created));
                }
            }

            if (terminal_paths.find(current_path) != terminal_paths.end()) {
                node->terminal = true;
            }
            parent = node;
        }
    }

    return tree;
}

inline const MetadataSchemaTreeNode* findMetadataSchemaNodeByPath(
    const std::vector<std::unique_ptr<MetadataSchemaTreeNode>>& nodes,
    const std::string& path) {
    for (const auto& node : nodes) {
        if (!node) {
            continue;
        }
        if (node->full_path == path) {
            return node.get();
        }
        const auto* nested = findMetadataSchemaNodeByPath(node->children, path);
        if (nested) {
            return nested;
        }
    }
    return nullptr;
}

inline std::vector<std::string> metadataSchemaChildren(
    const MetadataSchemaTree& tree,
    const std::string& parent_path) {
    std::vector<std::string> children;
    const auto normalized_parent = detail::normalizeSchemaPath(parent_path);

    const std::vector<std::unique_ptr<MetadataSchemaTreeNode>>* source = nullptr;
    if (normalized_parent.empty()) {
        source = &tree.schemas;
    } else {
        const auto* parent = findMetadataSchemaNodeByPath(tree.schemas, normalized_parent);
        if (!parent) {
            return children;
        }
        source = &parent->children;
    }

    for (const auto& node : *source) {
        if (!node || node->full_path.empty()) {
            continue;
        }
        children.push_back(node->full_path);
    }
    return children;
}

inline std::vector<MetadataSchemaTreeRow> buildDatabaseDefaultMetadataRows(
    const std::vector<std::string>& schema_names,
    const std::string& database,
    bool expand_schema_parents) {
    MetadataSchemaTree tree =
        buildMetadataSchemaTree(schema_names, database, expand_schema_parents);

    const std::string database_name = tree.database.empty() ? std::string("default") : tree.database;
    std::vector<MetadataSchemaTreeRow> rows;
    rows.push_back(MetadataSchemaTreeRow{
        MetadataTreeRowKind::DATABASE,
        database_name,
        std::string(),
        database_name,
        database_name,
        false,
        false
    });

    for (const auto& root : tree.schemas) {
        if (!root) {
            continue;
        }
        detail::appendRowsDepthFirst(*root, database_name, database_name, true, &rows);
    }

    return rows;
}

}  // namespace scratchbird::odbc::metadata
