// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "scratchbird/client/metadata.h"
#include "scratchbird/client/scratchbird_client.h"

namespace {

const scratchbird::client::MetadataSchemaTreeRow* findRowByPath(
    const std::vector<scratchbird::client::MetadataSchemaTreeRow>& rows,
    const std::string& path) {
    for (const auto& row : rows) {
        if (row.path == path) {
            return &row;
        }
    }
    return nullptr;
}

const scratchbird::client::MetadataSchemaTreeNode* findNodeByPath(
    const std::vector<std::unique_ptr<scratchbird::client::MetadataSchemaTreeNode>>& nodes,
    const std::string& path) {
    for (const auto& node : nodes) {
        if (!node) {
            continue;
        }
        if (node->full_path == path) {
            return node.get();
        }
        const auto* nested = findNodeByPath(node->children, path);
        if (nested != nullptr) {
            return nested;
        }
    }
    return nullptr;
}

} // namespace

TEST(MetadataSchemaTreeTest, TreeRowsStartAtDatabaseAndExposeTopBranches) {
    std::vector<scratchbird::client::MetadataSchemaTreeRow> rows =
        scratchbird::client::buildMetadataSchemaTreeRows(
            {"sys", "users.alice.dev", "users.bob.dev", "analytics.prod"},
            "main",
            false);

    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows[0].kind, scratchbird::client::MetadataTreeRowKind::DATABASE);
    EXPECT_EQ(rows[0].path, "main");

    std::vector<std::string> top_branches;
    for (const auto& row : rows) {
        if (row.kind == scratchbird::client::MetadataTreeRowKind::SCHEMA &&
            row.top_level_branch) {
            top_branches.push_back(row.path);
        }
    }

    EXPECT_EQ(top_branches, (std::vector<std::string>{"sys", "users", "analytics"}));
}

TEST(MetadataSchemaTreeTest, ParentExpansionAddsDottedSchemaAncestors) {
    std::vector<std::string> expanded = scratchbird::client::metadataSchemaPathsForNavigation(
        {"users.alice.dev", "users.bob.dev", "users.bob.dev"},
        true);

    EXPECT_EQ(
        expanded,
        (std::vector<std::string>{"users", "users.alice", "users.alice.dev", "users.bob", "users.bob.dev"}));
}

TEST(MetadataSchemaTreeTest, ParentDoesNotAllowDuplicateChildNames) {
    scratchbird::client::MetadataSchemaTree tree = scratchbird::client::buildMetadataSchemaTree(
        {"users.bob.dev", "users.bob.dev"},
        "main",
        false);

    const auto* bob = findNodeByPath(tree.schemas, "users.bob");
    ASSERT_NE(bob, nullptr);
    ASSERT_EQ(bob->children.size(), 1U);
    EXPECT_EQ(bob->children.front()->name, "dev");
    EXPECT_EQ(bob->children.front()->full_path, "users.bob.dev");
}

TEST(MetadataSchemaTreeTest, SameLeafNameUnderDifferentParentsIsPreserved) {
    std::vector<scratchbird::client::MetadataSchemaTreeRow> rows =
        scratchbird::client::buildMetadataSchemaTreeRows(
            {"users.alice.orders", "users.bob.orders"},
            "main",
            false);

    const auto* alice_orders = findRowByPath(rows, "users.alice.orders");
    const auto* bob_orders = findRowByPath(rows, "users.bob.orders");
    ASSERT_NE(alice_orders, nullptr);
    ASSERT_NE(bob_orders, nullptr);
    EXPECT_EQ(alice_orders->name, "orders");
    EXPECT_EQ(bob_orders->name, "orders");
    EXPECT_NE(alice_orders->parent_path, bob_orders->parent_path);
}

TEST(MetadataSchemaTreeTest, NormalizesCollectionAliasesForExtendedFamilies) {
    std::string normalized;
    ASSERT_TRUE(scratchbird::client::normalizeMetadataCollectionName("primaryKeys", &normalized));
    EXPECT_EQ(normalized, "primary_keys");

    ASSERT_TRUE(scratchbird::client::normalizeMetadataCollectionName("table privileges", &normalized));
    EXPECT_EQ(normalized, "table_privileges");

    ASSERT_TRUE(scratchbird::client::normalizeMetadataCollectionName("column-privileges", &normalized));
    EXPECT_EQ(normalized, "column_privileges");

    ASSERT_TRUE(scratchbird::client::normalizeMetadataCollectionName("catalog", &normalized));
    EXPECT_EQ(normalized, "catalogs");
}

TEST(MetadataSchemaTreeTest, ResolvesExtendedCollectionQueries) {
    std::string query_sql;
    std::string normalized;

    ASSERT_TRUE(scratchbird::client::resolveMetadataCollectionQuery("primarykeys", &query_sql, &normalized));
    EXPECT_EQ(normalized, "primary_keys");
    EXPECT_EQ(query_sql, sb_metadata_primary_keys_query());

    ASSERT_TRUE(scratchbird::client::resolveMetadataCollectionQuery("foreign_keys", &query_sql, &normalized));
    EXPECT_EQ(normalized, "foreign_keys");
    EXPECT_EQ(query_sql, sb_metadata_foreign_keys_query());

    ASSERT_TRUE(scratchbird::client::resolveMetadataCollectionQuery("tableprivileges", &query_sql, &normalized));
    EXPECT_EQ(normalized, "table_privileges");
    EXPECT_EQ(query_sql, sb_metadata_table_privileges_query());

    ASSERT_TRUE(scratchbird::client::resolveMetadataCollectionQuery("type_info", &query_sql, &normalized));
    EXPECT_EQ(normalized, "type_info");
    EXPECT_EQ(query_sql, sb_metadata_type_info_query());

    ASSERT_TRUE(scratchbird::client::resolveMetadataCollectionQuery("ddlfields", &query_sql, &normalized));
    EXPECT_EQ(normalized, "ddl_fields");
    EXPECT_EQ(query_sql, sb_metadata_ddl_fields_query());
}

TEST(MetadataSchemaTreeTest, RejectsUnsupportedCollection) {
    std::string query_sql;
    EXPECT_FALSE(scratchbird::client::resolveMetadataCollectionQuery("nope_collection", &query_sql, nullptr));
    EXPECT_EQ(
        scratchbird::client::metadataCollectionNotSupportedMessage("nope_collection"),
        "metadata collection 'nope_collection' is not supported");
}

TEST(MetadataSchemaTreeTest, BuildsDdlEditorSchemaPayloadJsonWithPatternAndParentExpansion) {
    const std::string schema_pattern = "users.%";
    const std::string payload_json = scratchbird::client::buildMetadataDdlEditorSchemaPayloadJson(
        {"users.alice.dev", "users.bob.dev", "sys"},
        &schema_pattern,
        true);

    const auto payload = nlohmann::json::parse(payload_json);
    ASSERT_TRUE(payload.is_object());
    EXPECT_EQ(payload["schemaPattern"], "users.%");
    EXPECT_TRUE(payload["expandSchemaParents"].get<bool>());
    EXPECT_EQ(
        payload["schemaPaths"].get<std::vector<std::string>>(),
        (std::vector<std::string>{"users", "users.alice", "users.alice.dev", "users.bob", "users.bob.dev"}));

    ASSERT_TRUE(payload["schemaTree"].is_array());
    ASSERT_FALSE(payload["schemaTree"].empty());
    const auto& root = payload["schemaTree"][0];
    EXPECT_EQ(root["name"], "users");
    EXPECT_EQ(root["path"], "users");
    EXPECT_TRUE(root["terminal"].get<bool>());
    ASSERT_TRUE(root["children"].is_array());
}

TEST(MetadataSchemaTreeTest, BuildsDdlEditorSchemaPayloadJsonWithoutPattern) {
    const std::string payload_json = scratchbird::client::buildMetadataDdlEditorSchemaPayloadJson(
        {"users.alice.dev", "sys"},
        nullptr,
        false);

    const auto payload = nlohmann::json::parse(payload_json);
    ASSERT_TRUE(payload.is_object());
    EXPECT_TRUE(payload["schemaPattern"].is_null());
    EXPECT_FALSE(payload["expandSchemaParents"].get<bool>());
    EXPECT_EQ(
        payload["schemaPaths"].get<std::vector<std::string>>(),
        (std::vector<std::string>{"users.alice.dev", "sys"}));
}
