<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

require_once __DIR__ . '/bootstrap.php';

use PHPUnit\Framework\TestCase;
use ScratchBird\PDO\Metadata;

final class MetadataRecursiveSchemaTest extends TestCase
{
    public function testExpandSchemaMetadataRowsSupportsDatabaseDefaultBranchStyleRows(): void
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

        $this->assertSame(
            ['database', 'database.default', 'database.default.users', 'database.default.audit'],
            $this->collectSchemaValues($expanded, 'TABLE_SCHEM')
        );
        $this->assertSame([null, null, 11, 12], $this->collectColumnValues($expanded, 'schema_id'));
    }

    public function testListMetadataSchemaPathsExpandsDottedSchemaParents(): void
    {
        $rows = [
            $this->schemaRow('users.alice.dev'),
            $this->schemaRow('sys'),
            $this->schemaRow('users.bob.dev'),
            $this->schemaRow('users.bob.dev'),
            $this->schemaRow('users..bob.dev'),
            $this->schemaRow(''),
        ];

        $expanded = Metadata::listMetadataSchemaPaths($rows, true);
        $this->assertSame(
            [
                'users',
                'users.alice',
                'users.alice.dev',
                'sys',
                'users.bob',
                'users.bob.dev',
            ],
            $expanded
        );
    }

    public function testBuildMetadataSchemaTreeNoDuplicatesWithinSameParent(): void
    {
        $rows = [
            $this->schemaRow('users.bob.dev'),
            $this->schemaRow('users.bob.dev'),
            $this->schemaRow('users.bob.prod'),
        ];

        $tree = Metadata::buildMetadataSchemaTree($rows);
        $bob = $this->findNodeByPath($tree['schemas'] ?? [], 'users.bob');
        $this->assertNotNull($bob);
        $this->assertCount(2, $bob['children']);
        $this->assertSame(
            ['users.bob.dev', 'users.bob.prod'],
            array_values(array_map(static fn (array $node): string => $node['path'], $bob['children']))
        );
        $this->assertSame(
            1,
            count(array_filter($bob['children'], static fn (array $node): bool => $node['name'] === 'dev'))
        );
    }

    public function testBuildMetadataSchemaTreeAllowsSameLeafUnderDifferentParents(): void
    {
        $rows = [
            $this->schemaRow('users.alice.dev'),
            $this->schemaRow('users.bob.dev'),
        ];

        $tree = Metadata::buildMetadataSchemaTree($rows, true, 'demo');
        $aliceDev = $this->findNodeByPath($tree['schemas'] ?? [], 'users.alice.dev');
        $bobDev = $this->findNodeByPath($tree['schemas'] ?? [], 'users.bob.dev');

        $this->assertSame('demo', $tree['database']);
        $this->assertNotNull($aliceDev);
        $this->assertNotNull($bobDev);
        $this->assertSame('dev', $aliceDev['name']);
        $this->assertSame('dev', $bobDev['name']);
        $this->assertNotSame($aliceDev['path'], $bobDev['path']);
    }

    private function schemaRow(string $schema): array
    {
        return ['schema_name' => $schema];
    }

    /**
     * @param array<int, array<string, mixed>> $rows
     * @return array<int, mixed>
     */
    private function collectColumnValues(array $rows, string $column): array
    {
        return array_values(
            array_map(
                static fn (array $row): mixed => $row[$column] ?? null,
                $rows
            )
        );
    }

    /**
     * @param array<int, array<string, mixed>> $rows
     * @return array<int, string>
     */
    private function collectSchemaValues(array $rows, string $key): array
    {
        $out = [];
        foreach ($rows as $row) {
            if (!isset($row[$key]) || !is_string($row[$key])) {
                continue;
            }
            $out[] = $row[$key];
        }
        return $out;
    }

    /**
     * @param array<int, array<string, mixed>> $nodes
     * @return array<string, mixed>|null
     */
    private function findNodeByPath(array $nodes, string $path): ?array
    {
        foreach ($nodes as $node) {
            if (($node['path'] ?? null) === $path) {
                return $node;
            }
            if (!isset($node['children']) || !is_array($node['children'])) {
                continue;
            }
            $child = $this->findNodeByPath($node['children'], $path);
            if ($child !== null) {
                return $child;
            }
        }
        return null;
    }
}
