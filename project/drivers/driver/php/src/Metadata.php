<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

namespace ScratchBird\PDO;

final class Metadata
{
    private const DEFAULT_COLLECTION = 'tables';

    private const SCHEMA_FIELD_CANDIDATES = [
        'schema_name',
        'TABLE_SCHEM',
        'table_schem',
        'table_schema',
        'TABLE_SCHEMA',
        'schema',
    ];

    public const SCHEMAS_QUERY = "SELECT schema_id, schema_name, owner_id, default_tablespace_id FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name";
    public const TABLES_QUERY = "SELECT table_id, schema_id, table_name, table_type, owner_id FROM sys.tables WHERE is_valid = 1 ORDER BY table_name";
    public const COLUMNS_QUERY = "SELECT column_id, table_id, column_name, data_type_id, data_type_name, ordinal_position, is_nullable, default_value, domain_id, collation_id, charset_id, is_identity, is_generated, generation_expression FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position";
    public const INDEXES_QUERY = "SELECT index_id, table_id, index_name, index_type, is_unique FROM sys.indexes WHERE is_valid = 1 ORDER BY table_id, index_name";
    public const INDEX_COLUMNS_QUERY = "SELECT index_id, column_id, column_name, ordinal_position, is_included FROM sys.index_columns ORDER BY index_id, ordinal_position";
    public const CONSTRAINTS_QUERY = "SELECT * FROM information_schema.table_constraints";
    public const PROCEDURES_QUERY = "SELECT * FROM information_schema.routines";
    public const FUNCTIONS_QUERY = self::PROCEDURES_QUERY;
    public const ROUTINES_QUERY = self::PROCEDURES_QUERY;
    public const CATALOGS_QUERY = "SELECT schema_id AS catalog_id, schema_name AS catalog_name FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name";
    public const PRIMARY_KEYS_QUERY = self::CONSTRAINTS_QUERY;
    public const FOREIGN_KEYS_QUERY = self::CONSTRAINTS_QUERY;
    public const TABLE_PRIVILEGES_QUERY = "SELECT table_id, table_name, owner_id AS grantor_id, owner_id AS grantee_id, 'ALL' AS privilege_type FROM sys.tables WHERE is_valid = 1 ORDER BY table_id, table_name";
    public const COLUMN_PRIVILEGES_QUERY = "SELECT table_id, column_id, column_name, 'ALL' AS privilege_type FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position";
    public const TYPE_INFO_QUERY = "SELECT DISTINCT data_type_id, data_type_name FROM sys.columns WHERE is_valid = 1 ORDER BY data_type_name";

    private const COLLECTION_QUERY_MAP = [
        'schemas' => self::SCHEMAS_QUERY,
        'tables' => self::TABLES_QUERY,
        'columns' => self::COLUMNS_QUERY,
        'indexes' => self::INDEXES_QUERY,
        'index_columns' => self::INDEX_COLUMNS_QUERY,
        'constraints' => self::CONSTRAINTS_QUERY,
        'procedures' => self::PROCEDURES_QUERY,
        'functions' => self::FUNCTIONS_QUERY,
        'routines' => self::ROUTINES_QUERY,
        'catalogs' => self::CATALOGS_QUERY,
        'primary_keys' => self::PRIMARY_KEYS_QUERY,
        'foreign_keys' => self::FOREIGN_KEYS_QUERY,
        'table_privileges' => self::TABLE_PRIVILEGES_QUERY,
        'column_privileges' => self::COLUMN_PRIVILEGES_QUERY,
        'type_info' => self::TYPE_INFO_QUERY,
    ];

    private const COLLECTION_ALIASES = [
        'schemas' => 'schemas',
        'schema' => 'schemas',
        'tables' => 'tables',
        'table' => 'tables',
        'columns' => 'columns',
        'column' => 'columns',
        'indexes' => 'indexes',
        'index' => 'indexes',
        'index_columns' => 'index_columns',
        'indexcolumns' => 'index_columns',
        'constraints' => 'constraints',
        'constraint' => 'constraints',
        'procedures' => 'procedures',
        'procedure' => 'procedures',
        'functions' => 'functions',
        'function' => 'functions',
        'routines' => 'routines',
        'routine' => 'routines',
        'catalogs' => 'catalogs',
        'catalog' => 'catalogs',
        'primary_keys' => 'primary_keys',
        'primary_key' => 'primary_keys',
        'primarykeys' => 'primary_keys',
        'primarykey' => 'primary_keys',
        'foreign_keys' => 'foreign_keys',
        'foreign_key' => 'foreign_keys',
        'foreignkeys' => 'foreign_keys',
        'foreignkey' => 'foreign_keys',
        'table_privileges' => 'table_privileges',
        'table_privilege' => 'table_privileges',
        'tableprivileges' => 'table_privileges',
        'tableprivilege' => 'table_privileges',
        'column_privileges' => 'column_privileges',
        'column_privilege' => 'column_privileges',
        'columnprivileges' => 'column_privileges',
        'columnprivilege' => 'column_privileges',
        'type_info' => 'type_info',
        'typeinfo' => 'type_info',
    ];

    private const RESTRICTION_KEY_ALIASES = [
        'catalog' => ['catalog_name', 'table_catalog', 'table_cat', 'catalog'],
        'schema' => ['schema_name', 'table_schema', 'table_schem', 'schema', 'constraint_schema', 'routine_schema'],
        'table' => ['table_name', 'table', 'relname'],
        'column' => ['column_name', 'column'],
        'index' => ['index_name', 'index'],
        'constraint' => ['constraint_name', 'constraint'],
        'procedure' => ['procedure_name', 'routine_name', 'procedure'],
        'function' => ['function_name', 'routine_name', 'function'],
        'routine' => ['routine_name', 'procedure_name', 'function_name', 'routine'],
        'type' => ['type_name', 'data_type_name', 'data_type', 'udt_name'],
    ];

    private const COLLECTION_RESTRICTION_KEYS = [
        'catalogs' => ['catalog'],
        'schemas' => ['catalog', 'schema'],
        'tables' => ['catalog', 'schema', 'table', 'type'],
        'columns' => ['catalog', 'schema', 'table', 'column', 'type'],
        'indexes' => ['catalog', 'schema', 'table', 'index'],
        'index_columns' => ['catalog', 'schema', 'table', 'index', 'column'],
        'constraints' => ['catalog', 'schema', 'table', 'constraint'],
        'primary_keys' => ['catalog', 'schema', 'table', 'constraint'],
        'foreign_keys' => ['catalog', 'schema', 'table', 'constraint'],
        'table_privileges' => ['catalog', 'schema', 'table'],
        'column_privileges' => ['catalog', 'schema', 'table', 'column'],
        'procedures' => ['catalog', 'schema', 'procedure'],
        'functions' => ['catalog', 'schema', 'function'],
        'type_info' => ['type'],
    ];

    public static function normalizeCollectionName(?string $collectionName = null): string
    {
        $value = $collectionName ?? self::DEFAULT_COLLECTION;
        $normalized = strtolower(trim($value));
        $normalized = str_replace(['-', ' '], '_', $normalized);
        if ($normalized === '') {
            $normalized = self::DEFAULT_COLLECTION;
        }
        $collapsed = str_replace('_', '', $normalized);
        $resolved = self::COLLECTION_ALIASES[$normalized] ?? self::COLLECTION_ALIASES[$collapsed] ?? null;
        if ($resolved === null) {
            throw new \InvalidArgumentException("Metadata collection '{$value}' is not supported");
        }
        return $resolved;
    }

    public static function resolveCollectionQuery(?string $collectionName = null): string
    {
        $resolved = self::normalizeCollectionName($collectionName);
        return self::COLLECTION_QUERY_MAP[$resolved];
    }

    /**
     * @param array<string, mixed> $restrictions
     * @return array<string, mixed>
     */
    public static function normalizeRestrictions(array $restrictions): array
    {
        if ($restrictions === []) {
            return [];
        }

        $out = [];
        foreach ($restrictions as $key => $value) {
            $normalizedKey = self::normalizeIdentifier((string) $key);
            if ($normalizedKey === '') {
                continue;
            }
            $out[$normalizedKey] = $value;
        }
        return $out;
    }

    /**
     * @param array<int, array<string, mixed>> $rows
     * @return array<int, array<string, mixed>>
     */
    public static function filterRowsForCollectionFamily(array $rows, ?string $collectionName = null): array
    {
        $resolved = self::normalizeCollectionName($collectionName);
        return match ($resolved) {
            'primary_keys' => array_values(array_filter(
                $rows,
                fn (array $row): bool => self::rowHasExpectedText($row, ['constraint_type', 'CONSTRAINT_TYPE'], ['primary key', 'primary'])
            )),
            'foreign_keys' => array_values(array_filter(
                $rows,
                fn (array $row): bool => self::rowHasExpectedText($row, ['constraint_type', 'CONSTRAINT_TYPE'], ['foreign key', 'foreign'])
            )),
            'procedures' => array_values(array_filter(
                $rows,
                fn (array $row): bool => self::rowHasExpectedText($row, ['routine_type', 'ROUTINE_TYPE'], ['procedure'])
            )),
            'functions' => array_values(array_filter(
                $rows,
                fn (array $row): bool => self::rowHasExpectedText($row, ['routine_type', 'ROUTINE_TYPE'], ['function'])
            )),
            default => $rows,
        };
    }

    /**
     * @param array<int, array<string, mixed>> $rows
     * @param array<string, mixed> $restrictions
     * @return array<int, array<string, mixed>>
     */
    public static function filterRowsByRestrictions(array $rows, array $restrictions, ?string $collectionName = null): array
    {
        $normalizedRestrictions = self::normalizeRestrictions($restrictions);
        if ($normalizedRestrictions === []) {
            return $rows;
        }

        $bindings = self::buildRestrictionBindings($rows, $normalizedRestrictions, $collectionName);
        if ($bindings === []) {
            return $rows;
        }

        $out = [];
        foreach ($rows as $row) {
            if (self::rowMatchesRestrictions($row, $bindings)) {
                $out[] = $row;
            }
        }
        return $out;
    }

    public static function schemasQuery(): string
    {
        return self::SCHEMAS_QUERY;
    }

    public static function tablesQuery(): string
    {
        return self::TABLES_QUERY;
    }

    public static function columnsQuery(): string
    {
        return self::COLUMNS_QUERY;
    }

    public static function indexesQuery(): string
    {
        return self::INDEXES_QUERY;
    }

    public static function indexColumnsQuery(): string
    {
        return self::INDEX_COLUMNS_QUERY;
    }

    public static function constraintsQuery(): string
    {
        return self::CONSTRAINTS_QUERY;
    }

    public static function proceduresQuery(): string
    {
        return self::PROCEDURES_QUERY;
    }

    public static function functionsQuery(): string
    {
        return self::FUNCTIONS_QUERY;
    }

    public static function routinesQuery(): string
    {
        return self::ROUTINES_QUERY;
    }

    public static function catalogsQuery(): string
    {
        return self::CATALOGS_QUERY;
    }

    public static function primaryKeysQuery(): string
    {
        return self::PRIMARY_KEYS_QUERY;
    }

    public static function foreignKeysQuery(): string
    {
        return self::FOREIGN_KEYS_QUERY;
    }

    public static function tablePrivilegesQuery(): string
    {
        return self::TABLE_PRIVILEGES_QUERY;
    }

    public static function columnPrivilegesQuery(): string
    {
        return self::COLUMN_PRIVILEGES_QUERY;
    }

    public static function typeInfoQuery(): string
    {
        return self::TYPE_INFO_QUERY;
    }

    /**
     * @param array<mixed> $schemaNames
     * @return array<string>
     */
    public static function schemaPathsForNavigation(array $schemaNames, bool $expandParents = false): array
    {
        $out = [];
        $seen = [];
        foreach ($schemaNames as $schemaName) {
            if (!is_string($schemaName) && !is_numeric($schemaName)) {
                continue;
            }
            $normalized = self::normalizeSchemaPath((string)$schemaName);
            if ($normalized === null || isset($seen[$normalized])) {
                continue;
            }
            $seen[$normalized] = true;
            $out[] = $normalized;
        }
        if (!$expandParents) {
            return $out;
        }
        return self::expandSchemaPaths($out);
    }

    /**
     * @param array<string> $schemaPaths
     * @return array<string>
     */
    public static function expandSchemaPaths(array $schemaPaths): array
    {
        $out = [];
        $seen = [];
        foreach ($schemaPaths as $schemaPath) {
            $segments = self::splitSchemaPath($schemaPath);
            if ($segments === []) {
                continue;
            }
            $currentPath = '';
            foreach ($segments as $segment) {
                $currentPath = $currentPath === '' ? $segment : $currentPath . '.' . $segment;
                if (isset($seen[$currentPath])) {
                    continue;
                }
                $seen[$currentPath] = true;
                $out[] = $currentPath;
            }
        }
        return $out;
    }

    /**
     * @param array<mixed> $rows
     * @return array<string>
     */
    public static function listMetadataSchemaPaths(array $rows, bool $expandParents = false): array
    {
        $deduped = [];
        $seen = [];
        foreach ($rows as $row) {
            $schemaPath = self::readSchemaPath($row);
            if ($schemaPath === null || isset($seen[$schemaPath])) {
                continue;
            }
            $seen[$schemaPath] = true;
            $deduped[] = $schemaPath;
        }
        if (!$expandParents) {
            return $deduped;
        }
        return self::expandSchemaPaths($deduped);
    }

    /**
     * @param array<mixed> $rows
     * @return array{database: ?string, schemas: array<int, array{name: string, path: string, terminal: bool, children: array}>}
     */
    public static function buildMetadataSchemaTree(array $rows, bool $expandParents = false, ?string $database = null): array
    {
        $basePaths = self::listMetadataSchemaPaths($rows, false);
        $expandedPaths = $expandParents ? self::expandSchemaPaths($basePaths) : $basePaths;
        $terminalPaths = [];
        foreach ($expandParents ? $expandedPaths : $basePaths as $terminalPath) {
            $terminalPaths[$terminalPath] = true;
        }

        $roots = [];
        foreach ($expandedPaths as $schemaPath) {
            $segments = self::splitSchemaPath($schemaPath);
            if ($segments === []) {
                continue;
            }

            $currentPath = '';
            $children = &$roots;
            foreach ($segments as $segment) {
                $currentPath = $currentPath === '' ? $segment : $currentPath . '.' . $segment;
                $node = &self::upsertSchemaTreeNode($children, $segment, $currentPath, isset($terminalPaths[$currentPath]));
                $children = &$node['children'];
                unset($node);
            }
            unset($children);
        }

        $database = $database === null ? null : trim($database);
        if ($database === '') {
            $database = null;
        }

        return [
            'database' => $database,
            'schemas' => $roots,
        ];
    }

    /**
     * @param array<mixed> $rows
     * @return array<mixed>
     */
    public static function expandSchemaMetadataRows(array $rows): array
    {
        $out = [];
        $seen = [];
        foreach ($rows as $row) {
            $schemaPath = self::readSchemaPath($row);
            if ($schemaPath === null) {
                $out[] = $row;
                continue;
            }
            $segments = self::splitSchemaPath($schemaPath);
            if ($segments === []) {
                $out[] = $row;
                continue;
            }

            $currentPath = '';
            $segmentCount = count($segments);
            foreach ($segments as $index => $segment) {
                $currentPath = $currentPath === '' ? $segment : $currentPath . '.' . $segment;
                if (isset($seen[$currentPath])) {
                    continue;
                }
                $seen[$currentPath] = true;
                if ($index === ($segmentCount - 1)) {
                    $out[] = $row;
                } else {
                    $out[] = is_array($row)
                        ? self::createSyntheticSchemaRow($row, $currentPath)
                        : ['schema_name' => $currentPath];
                }
            }
        }
        return $out;
    }

    /**
     * @param array<string, mixed> $sample
     * @return array<string, mixed>
     */
    private static function createSyntheticSchemaRow(array $sample, string $schemaPath): array
    {
        $synthetic = [];
        foreach ($sample as $key => $_value) {
            if (!is_string($key)) {
                continue;
            }
            $synthetic[$key] = null;
        }

        $assigned = false;
        foreach (self::SCHEMA_FIELD_CANDIDATES as $candidate) {
            $key = self::metadataRowKey($synthetic, $candidate);
            if ($key === null) {
                continue;
            }
            $synthetic[$key] = $schemaPath;
            $assigned = true;
        }
        if (!$assigned) {
            $synthetic['schema_name'] = $schemaPath;
        }

        return $synthetic;
    }

    /**
     * @param array<mixed> $row
     */
    private static function readSchemaPath(mixed $row): ?string
    {
        if (is_string($row) || is_numeric($row)) {
            return self::normalizeSchemaPath((string)$row);
        }
        if (!is_array($row)) {
            return null;
        }

        foreach (self::SCHEMA_FIELD_CANDIDATES as $candidate) {
            $value = self::metadataRowValue($row, $candidate);
            if (!is_string($value)) {
                continue;
            }
            $normalized = self::normalizeSchemaPath($value);
            if ($normalized !== null) {
                return $normalized;
            }
        }
        return null;
    }

    /**
     * @param array<string, mixed> $row
     */
    private static function metadataRowValue(array $row, string $key): mixed
    {
        if (array_key_exists($key, $row)) {
            return $row[$key];
        }
        foreach ($row as $candidate => $value) {
            if (!is_string($candidate)) {
                continue;
            }
            if (strcasecmp($candidate, $key) === 0) {
                return $value;
            }
        }
        return null;
    }

    /**
     * @param array<string, mixed> $row
     */
    private static function metadataRowKey(array $row, string $key): ?string
    {
        if (array_key_exists($key, $row)) {
            return $key;
        }
        foreach ($row as $candidate => $_value) {
            if (!is_string($candidate)) {
                continue;
            }
            if (strcasecmp($candidate, $key) === 0) {
                return $candidate;
            }
        }
        return null;
    }

    /**
     * @param array<int, array<string, mixed>> $rows
     * @param array<string, mixed> $restrictions
     * @return array<int, array{aliases: array<int, string>, expectNull: bool, expectedText: string}>
     */
    private static function buildRestrictionBindings(array $rows, array $restrictions, ?string $collectionName): array
    {
        $allowedAliases = [];
        if ($collectionName !== null) {
            $resolvedCollection = self::normalizeCollectionName($collectionName);
            foreach (self::COLLECTION_RESTRICTION_KEYS[$resolvedCollection] ?? [] as $key) {
                foreach (self::restrictionAliases($key) as $alias) {
                    $allowedAliases[$alias] = true;
                }
            }
        }

        $bindings = [];
        foreach ($restrictions as $restrictionKey => $restrictionValue) {
            $aliases = [];
            foreach (self::restrictionAliases($restrictionKey) as $alias) {
                if ($allowedAliases !== [] && !isset($allowedAliases[$alias]) && $alias !== $restrictionKey) {
                    continue;
                }
                $aliases[$alias] = true;
            }
            if ($aliases === []) {
                continue;
            }

            $aliasList = array_keys($aliases);
            if (!self::rowsHaveAnyAlias($rows, $aliasList)) {
                continue;
            }

            $expected = self::normalizeMatchText($restrictionValue);
            $bindings[] = [
                'aliases' => $aliasList,
                'expectNull' => $expected === 'null',
                'expectedText' => $expected,
            ];
        }
        return $bindings;
    }

    /**
     * @param array<int, array<string, mixed>> $rows
     * @param array<int, string> $aliases
     */
    private static function rowsHaveAnyAlias(array $rows, array $aliases): bool
    {
        foreach ($rows as $row) {
            foreach ($aliases as $alias) {
                $rowKey = self::metadataRowKeyByAlias($row, $alias);
                if ($rowKey !== null) {
                    return true;
                }
            }
        }
        return false;
    }

    /**
     * @param array<string, mixed> $row
     * @param array<int, array{aliases: array<int, string>, expectNull: bool, expectedText: string}> $bindings
     */
    private static function rowMatchesRestrictions(array $row, array $bindings): bool
    {
        foreach ($bindings as $binding) {
            $matched = false;
            foreach ($binding['aliases'] as $alias) {
                $rowKey = self::metadataRowKeyByAlias($row, $alias);
                if ($rowKey === null) {
                    continue;
                }
                $value = $row[$rowKey] ?? null;
                if ($binding['expectNull']) {
                    if ($value === null) {
                        $matched = true;
                        break;
                    }
                    continue;
                }
                if ($value !== null && self::normalizeMatchText($value) === $binding['expectedText']) {
                    $matched = true;
                    break;
                }
            }
            if (!$matched) {
                return false;
            }
        }
        return true;
    }

    /**
     * @param array<string, mixed> $row
     * @param array<int, string> $aliases
     * @param array<int, string> $expectedTexts
     */
    private static function rowHasExpectedText(array $row, array $aliases, array $expectedTexts): bool
    {
        $normalizedExpected = array_map(
            fn (string $value): string => self::normalizeMatchText($value),
            $expectedTexts
        );
        foreach ($aliases as $alias) {
            $rowKey = self::metadataRowKeyByAlias($row, $alias);
            if ($rowKey === null) {
                continue;
            }
            $value = $row[$rowKey] ?? null;
            if ($value === null) {
                continue;
            }
            if (in_array(self::normalizeMatchText($value), $normalizedExpected, true)) {
                return true;
            }
        }
        return false;
    }

    /**
     * @param array<string, mixed> $row
     */
    private static function metadataRowKeyByAlias(array $row, string $alias): ?string
    {
        if (array_key_exists($alias, $row)) {
            return $alias;
        }
        $target = self::normalizeIdentifier($alias);
        foreach ($row as $candidate => $_value) {
            if (!is_string($candidate)) {
                continue;
            }
            if (self::normalizeIdentifier($candidate) === $target) {
                return $candidate;
            }
        }
        return null;
    }

    /**
     * @return array<int, string>
     */
    private static function restrictionAliases(string $key): array
    {
        $canonical = self::normalizeIdentifier($key);
        $aliases = self::RESTRICTION_KEY_ALIASES[$canonical] ?? [];
        if ($canonical !== '') {
            $aliases[] = $canonical;
        }
        return array_values(array_unique(array_map(
            fn (string $alias): string => self::normalizeIdentifier($alias),
            $aliases
        )));
    }

    private static function normalizeIdentifier(string $value): string
    {
        $lower = strtolower(trim($value));
        if ($lower === '') {
            return '';
        }
        return preg_replace('/[^a-z0-9]/', '', $lower) ?? '';
    }

    private static function normalizeMatchText(mixed $value): string
    {
        return strtolower(trim((string) $value));
    }

    /**
     * @return array<string>
     */
    private static function splitSchemaPath(string $value): array
    {
        $parts = [];
        foreach (explode('.', $value) as $part) {
            $part = trim($part);
            if ($part === '') {
                continue;
            }
            $parts[] = $part;
        }
        return $parts;
    }

    private static function normalizeSchemaPath(string $value): ?string
    {
        $parts = self::splitSchemaPath($value);
        if ($parts === []) {
            return null;
        }
        return implode('.', $parts);
    }

    /**
     * @param array<int, array{name: string, path: string, terminal: bool, children: array}> $children
     * @return array{name: string, path: string, terminal: bool, children: array}
     */
    private static function &upsertSchemaTreeNode(array &$children, string $name, string $path, bool $terminal): array
    {
        foreach ($children as $index => $child) {
            if (($child['path'] ?? '') !== $path) {
                continue;
            }
            if ($terminal) {
                $children[$index]['terminal'] = true;
            }
            return $children[$index];
        }

        $children[] = [
            'name' => $name,
            'path' => $path,
            'terminal' => $terminal,
            'children' => [],
        ];
        $lastIndex = array_key_last($children);
        return $children[$lastIndex];
    }
}
