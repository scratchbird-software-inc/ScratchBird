<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

require_once __DIR__ . '/bootstrap.php';

use ScratchBird\PDO\Metadata;

/**
 * @param mixed $expected
 * @param mixed $actual
 */
function assertSameValue(mixed $expected, mixed $actual, string $message): void
{
    if ($expected !== $actual) {
        throw new RuntimeException(
            $message
            . "\nExpected: " . var_export($expected, true)
            . "\nActual: " . var_export($actual, true)
        );
    }
}

/**
 * @param array<int, array<string, mixed>> $nodes
 * @return array<string, mixed>|null
 */
function findNodeByPath(array $nodes, string $path): ?array
{
    foreach ($nodes as $node) {
        if (($node['path'] ?? null) === $path) {
            return $node;
        }
        if (!isset($node['children']) || !is_array($node['children'])) {
            continue;
        }
        $child = findNodeByPath($node['children'], $path);
        if ($child !== null) {
            return $child;
        }
    }
    return null;
}

function testDatabaseDefaultBranchStyleMetadataRows(): void
{
    $rows = [
        [
            'schema_id' => 11,
            'TABLE_SCHEM' => 'database.default.users',
            'TABLE_CATALOG' => 'database',
        ],
        [
            'schema_id' => 12,
            'TABLE_SCHEM' => 'database.default.audit',
            'TABLE_CATALOG' => 'database',
        ],
    ];

    $expanded = Metadata::expandSchemaMetadataRows($rows);

    $schemas = [];
    $schemaIds = [];
    foreach ($expanded as $row) {
        $schemas[] = $row['TABLE_SCHEM'] ?? null;
        $schemaIds[] = $row['schema_id'] ?? null;
    }

    assertSameValue(
        ['database', 'database.default', 'database.default.users', 'database.default.audit'],
        $schemas,
        'database/default branch-style metadata rows should expand with synthetic parents first'
    );
    assertSameValue(
        [null, null, 11, 12],
        $schemaIds,
        'synthetic parent rows should null non-schema metadata while preserving leaf rows'
    );
}

function testDottedParentExpansion(): void
{
    $rows = [
        ['schema_name' => 'users.alice.dev'],
        ['schema_name' => 'sys'],
        ['schema_name' => 'users.bob.dev'],
        ['schema_name' => 'users.bob.dev'],
        ['schema_name' => 'users..bob.dev'],
        ['schema_name' => ''],
    ];

    $expanded = Metadata::listMetadataSchemaPaths($rows, true);
    assertSameValue(
        ['users', 'users.alice', 'users.alice.dev', 'sys', 'users.bob', 'users.bob.dev'],
        $expanded,
        'dotted schema names should expand into parent paths with first-seen ordering'
    );
}

function testNoDuplicatesWithinSameParent(): void
{
    $rows = [
        ['schema_name' => 'users.bob.dev'],
        ['schema_name' => 'users.bob.dev'],
        ['schema_name' => 'users.bob.prod'],
    ];
    $tree = Metadata::buildMetadataSchemaTree($rows);
    $bob = findNodeByPath($tree['schemas'] ?? [], 'users.bob');
    if ($bob === null) {
        throw new RuntimeException('missing users.bob node');
    }
    $children = $bob['children'] ?? [];
    $childPaths = array_map(static fn (array $node): string => $node['path'], $children);
    $devCount = count(array_filter($children, static fn (array $node): bool => $node['name'] === 'dev'));

    assertSameValue(
        ['users.bob.dev', 'users.bob.prod'],
        $childPaths,
        'duplicate schema rows should not create duplicate child entries under the same parent'
    );
    assertSameValue(
        1,
        $devCount,
        'same leaf under same parent should appear once'
    );
}

function testSameLeafAllowedUnderDifferentParents(): void
{
    $rows = [
        ['schema_name' => 'users.alice.dev'],
        ['schema_name' => 'users.bob.dev'],
    ];
    $tree = Metadata::buildMetadataSchemaTree($rows, true, 'demo');
    $aliceDev = findNodeByPath($tree['schemas'] ?? [], 'users.alice.dev');
    $bobDev = findNodeByPath($tree['schemas'] ?? [], 'users.bob.dev');

    assertSameValue('demo', $tree['database'] ?? null, 'database label should be preserved');
    if ($aliceDev === null || $bobDev === null) {
        throw new RuntimeException('expected both users.alice.dev and users.bob.dev nodes');
    }
    assertSameValue('dev', $aliceDev['name'] ?? null, 'alice leaf should retain dev name');
    assertSameValue('dev', $bobDev['name'] ?? null, 'bob leaf should retain dev name');
    if (($aliceDev['path'] ?? null) === ($bobDev['path'] ?? null)) {
        throw new RuntimeException('same leaf name under different parents must remain distinct paths');
    }
}

try {
    testDatabaseDefaultBranchStyleMetadataRows();
    testDottedParentExpansion();
    testNoDuplicatesWithinSameParent();
    testSameLeafAllowedUnderDifferentParents();
    fwrite(STDOUT, "metadata recursive schema smoke tests: PASS (4/4)\n");
    exit(0);
} catch (Throwable $ex) {
    fwrite(STDERR, "metadata recursive schema smoke tests: FAIL\n");
    fwrite(STDERR, $ex->getMessage() . "\n");
    exit(1);
}
