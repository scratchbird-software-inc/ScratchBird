// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace scratchbird {
namespace client {

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

std::vector<std::string> metadataSchemaPathsForNavigation(
    const std::vector<std::string>& schema_names,
    bool expand_schema_parents);

MetadataSchemaTree buildMetadataSchemaTree(
    const std::vector<std::string>& schema_names,
    const std::string& database,
    bool expand_schema_parents);

std::vector<MetadataSchemaTreeRow> buildMetadataSchemaTreeRows(
    const std::vector<std::string>& schema_names,
    const std::string& database,
    bool expand_schema_parents);

bool normalizeMetadataCollectionName(
    const std::string& collection_name,
    std::string* normalized_collection);

bool resolveMetadataCollectionQuery(
    const std::string& collection_name,
    std::string* query_sql,
    std::string* normalized_collection = nullptr);

std::string buildMetadataSchemasQuerySql(
    const std::string* schema_pattern = nullptr);

std::string buildMetadataTablesQuerySql(
    const std::string* schema_pattern = nullptr,
    const std::string* table_pattern = nullptr);

std::string buildMetadataColumnsQuerySql(
    const std::string* schema_pattern = nullptr,
    const std::string* table_pattern = nullptr);

std::string buildMetadataIndexesQuerySql(
    const std::string* schema_pattern = nullptr,
    const std::string* table_pattern = nullptr);

std::string metadataCollectionNotSupportedMessage(const std::string& collection_name);

// Build deterministic DDL-editor metadata payload JSON:
// {"schemaPattern", "expandSchemaParents", "schemaPaths", "schemaTree"}.
std::string buildMetadataDdlEditorSchemaPayloadJson(
    const std::vector<std::string>& schema_names,
    const std::string* schema_pattern,
    bool expand_schema_parents);

} // namespace client
} // namespace scratchbird
