// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import XCTest
@testable import ScratchBird

final class MetadataRecursiveSchemaTests: XCTestCase {
    func testMetadataCollectionAliasesResolveExtendedFamilies() throws {
        XCTAssertEqual(try normalizeMetadataCollectionName("catalog"), .catalogs)
        XCTAssertEqual(try normalizeMetadataCollectionName("pk"), .primaryKeys)
        XCTAssertEqual(try normalizeMetadataCollectionName("foreignkey"), .foreignKeys)
        XCTAssertEqual(try normalizeMetadataCollectionName("tableprivileges"), .tablePrivileges)
        XCTAssertEqual(try normalizeMetadataCollectionName("columnprivilege"), .columnPrivileges)
        XCTAssertEqual(try normalizeMetadataCollectionName("typeinfo"), .typeInfo)
        XCTAssertEqual(try normalizeMetadataCollectionName("routine"), .routines)
    }

    func testMetadataCollectionQueriesResolveExtendedFamilies() throws {
        XCTAssertEqual(try resolveMetadataCollectionQuery("catalogs"), ScratchBirdMetadata.catalogsQuery)
        XCTAssertEqual(try resolveMetadataCollectionQuery("primary_keys"), ScratchBirdMetadata.primaryKeysQuery)
        XCTAssertEqual(try resolveMetadataCollectionQuery("foreign_keys"), ScratchBirdMetadata.foreignKeysQuery)
        XCTAssertEqual(try resolveMetadataCollectionQuery("table_privileges"), ScratchBirdMetadata.tablePrivilegesQuery)
        XCTAssertEqual(try resolveMetadataCollectionQuery("column_privileges"), ScratchBirdMetadata.columnPrivilegesQuery)
        XCTAssertEqual(try resolveMetadataCollectionQuery("type_info"), ScratchBirdMetadata.typeInfoQuery)
        XCTAssertEqual(try resolveMetadataCollectionQuery("routines"), ScratchBirdMetadata.routinesQuery)
    }

    func testMetadataSchemaNamesUsesNamedSchemaColumnAndDeduplicates() {
        let result = ScratchBirdResult(
            rows: [
                [1, "sys", 0, 0],
                [2, "users.alice", 0, 0],
                [3, "users.alice", 0, 0],
                [4, " analytics.prod ", 0, 0],
            ],
            columns: [
                ScratchBirdColumn(name: "schema_id", typeOid: TypeOid.int8, format: 1),
                ScratchBirdColumn(name: "schema_name", typeOid: TypeOid.text, format: 1),
                ScratchBirdColumn(name: "owner_id", typeOid: TypeOid.int8, format: 1),
                ScratchBirdColumn(name: "default_tablespace_id", typeOid: TypeOid.int8, format: 1),
            ]
        )

        let names = metadataSchemaNames(from: result)
        XCTAssertEqual(names, ["sys", "users.alice", "analytics.prod"])
    }

    func testMetadataSchemaNamesFallsBackToSecondColumnWhenColumnMetadataMissing() {
        let result = ScratchBirdResult(
            rows: [
                [1, "sys"],
                [2, RawValue(oid: TypeOid.text, data: Data("users.bob".utf8))],
                [3, Data("users.bob.dev".utf8)],
                [4, ""],
            ],
            columns: []
        )

        let names = metadataSchemaNames(from: result)
        XCTAssertEqual(names, ["sys", "users.bob", "users.bob.dev"])
    }

    func testTreeRowsStartAtDefaultDatabaseAndExposeTopBranches() {
        let rows = buildMetadataSchemaTreeRows(
            ["sys", "users.alice.dev", "users.bob.dev", "analytics.prod"],
            database: "",
            expandSchemaParents: false
        )

        XCTAssertFalse(rows.isEmpty)
        XCTAssertEqual(rows[0].kind, .database)
        XCTAssertEqual(rows[0].path, "default")

        let topBranches = rows
            .filter { $0.kind == .schema && $0.topLevelBranch }
            .map { $0.path }
        XCTAssertEqual(topBranches, ["sys", "users", "analytics"])
    }

    func testParentExpansionAddsDottedSchemaAncestors() {
        let expanded = metadataSchemaPathsForNavigation(
            ["users.alice.dev", "users.bob.dev", "users.bob.dev"],
            expandSchemaParents: true
        )

        XCTAssertEqual(
            expanded,
            ["users", "users.alice", "users.alice.dev", "users.bob", "users.bob.dev"]
        )
    }

    func testParentDoesNotAllowDuplicateChildNames() {
        let tree = buildMetadataSchemaTree(
            ["users.bob.dev", "users.bob.dev"],
            database: "main",
            expandSchemaParents: false
        )

        let bob = findNodeByPath(tree.schemas, "users.bob")
        XCTAssertNotNil(bob)
        XCTAssertEqual(bob?.children.count, 1)
        XCTAssertEqual(bob?.children.first?.name, "dev")
        XCTAssertEqual(bob?.children.first?.path, "users.bob.dev")
    }

    func testSameLeafNameUnderDifferentParentsIsPreserved() {
        let rows = buildMetadataSchemaTreeRows(
            ["users.alice.orders", "users.bob.orders"],
            database: "main",
            expandSchemaParents: false
        )

        let aliceOrders = findRowByPath(rows, "users.alice.orders")
        let bobOrders = findRowByPath(rows, "users.bob.orders")
        XCTAssertNotNil(aliceOrders)
        XCTAssertNotNil(bobOrders)
        XCTAssertEqual(aliceOrders?.name, "orders")
        XCTAssertEqual(bobOrders?.name, "orders")
        XCTAssertNotEqual(aliceOrders?.parentPath, bobOrders?.parentPath)
    }

    private func findRowByPath(_ rows: [ScratchBirdMetadataSchemaTreeRow], _ path: String) -> ScratchBirdMetadataSchemaTreeRow? {
        for row in rows where row.path == path {
            return row
        }
        return nil
    }

    private func findNodeByPath(_ nodes: [ScratchBirdMetadataSchemaTreeNode], _ path: String) -> ScratchBirdMetadataSchemaTreeNode? {
        for node in nodes {
            if node.path == path {
                return node
            }
            if let nested = findNodeByPath(node.children, path) {
                return nested
            }
        }
        return nil
    }
}
