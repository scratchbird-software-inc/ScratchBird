// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metadata_shaping.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace scratchbird::cli::metadata {

namespace {

struct SchemaTreeNode {
    std::string name;
    std::string full_path;
    bool terminal{false};
    std::vector<std::unique_ptr<SchemaTreeNode>> children;
};

std::string trimCopy(std::string_view value) {
    size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return std::string(value.substr(begin, end - begin));
}

std::string toUpperCopy(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return out;
}

std::vector<std::string> splitSchemaPath(const std::string& schema_name) {
    std::vector<std::string> segments;
    size_t start = 0;
    while (start <= schema_name.size()) {
        size_t dot = schema_name.find('.', start);
        size_t end = (dot == std::string::npos) ? schema_name.size() : dot;
        std::string segment =
            trimCopy(std::string_view(schema_name).substr(start, end - start));
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

void appendRowsDepthFirst(const SchemaTreeNode& node,
                          const std::string& database_name,
                          const std::string& parent_path,
                          bool top_level,
                          std::vector<SchemaTreeRow>* rows) {
    if (rows == nullptr) {
        return;
    }

    rows->push_back(SchemaTreeRow{
        SchemaTreeRowKind::kSchema,
        database_name,
        parent_path,
        node.full_path,
        node.name,
        node.terminal,
        top_level,
    });

    for (const auto& child : node.children) {
        if (!child) {
            continue;
        }
        appendRowsDepthFirst(
            *child, database_name, node.full_path, false, rows);
    }
}

}  // namespace

std::vector<std::string> schemaPathsForNavigation(
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

std::vector<SchemaTreeRow> buildSchemaTreeRows(
    const std::vector<std::string>& schema_names,
    const std::string& database,
    bool expand_schema_parents) {
    const std::vector<std::string> schema_paths =
        schemaPathsForNavigation(schema_names, expand_schema_parents);
    const std::unordered_set<std::string> terminal_paths(schema_paths.begin(),
                                                         schema_paths.end());

    std::unordered_map<std::string, SchemaTreeNode*> nodes_by_path;
    std::vector<std::unique_ptr<SchemaTreeNode>> roots;

    for (const std::string& schema_path : schema_paths) {
        const std::vector<std::string> parts = splitSchemaPath(schema_path);
        if (parts.empty()) {
            continue;
        }

        SchemaTreeNode* parent = nullptr;
        std::string current_path;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (!current_path.empty()) {
                current_path.push_back('.');
            }
            current_path.append(parts[i]);

            SchemaTreeNode* node = nullptr;
            auto existing = nodes_by_path.find(current_path);
            if (existing == nodes_by_path.end()) {
                auto created = std::make_unique<SchemaTreeNode>();
                created->name = parts[i];
                created->full_path = current_path;
                node = created.get();
                nodes_by_path[current_path] = node;

                if (parent == nullptr) {
                    roots.push_back(std::move(created));
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

    std::string database_name = trimCopy(database);
    if (database_name.empty()) {
        database_name = "default";
    }

    std::vector<SchemaTreeRow> rows;
    rows.push_back(SchemaTreeRow{
        SchemaTreeRowKind::kDatabase,
        database_name,
        std::string(),
        database_name,
        database_name,
        false,
        false,
    });

    for (const auto& root : roots) {
        if (!root) {
            continue;
        }
        appendRowsDepthFirst(*root, database_name, database_name, true, &rows);
    }

    return rows;
}

std::vector<std::string> schemaPathsFromObjectResolver(
    const std::vector<ObjectResolverEntry>& entries) {
    std::vector<std::string> raw_paths;
    raw_paths.reserve(entries.size());

    for (const ObjectResolverEntry& entry : entries) {
        const std::string type = toUpperCopy(trimCopy(entry.object_type));
        if (type.empty() || type == "COLUMN") {
            continue;
        }

        std::string schema_path = normalizeSchemaPath(entry.schema_path);
        if (type == "SCHEMA") {
            const std::string name = normalizeSchemaPath(entry.object_name);
            if (!name.empty()) {
                if (!schema_path.empty()) {
                    schema_path.push_back('.');
                }
                schema_path.append(name);
            }
            schema_path = normalizeSchemaPath(schema_path);
            if (!schema_path.empty()) {
                raw_paths.push_back(schema_path);
            }
            continue;
        }

        if (!schema_path.empty()) {
            raw_paths.push_back(schema_path);
        }
    }

    return schemaPathsForNavigation(raw_paths, false);
}

}  // namespace scratchbird::cli::metadata
