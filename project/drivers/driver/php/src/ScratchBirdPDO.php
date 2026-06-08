<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

namespace ScratchBird\PDO;

final class ScratchBirdPDO
{
    private Connection $connection;

    public function __construct(string $dsn, ?string $username = null, ?string $password = null, array $options = [])
    {
        $this->connection = new Connection($dsn, $username, $password, $options);
    }

    public function prepare(string $statement, array $options = []): Statement|false
    {
        try {
            return $this->connection->prepare($statement, $options);
        } catch (\Throwable) {
            return false;
        }
    }

    public function query(string $statement, mixed ...$fetchModeArgs): Statement|false
    {
        try {
            return $this->connection->query($statement, ...$fetchModeArgs);
        } catch (\Throwable) {
            return false;
        }
    }

    public function nativeSql(string $sql, array $params = []): string
    {
        return $this->connection->nativeSql($sql, $params);
    }

    public function nativeCallableSql(string $sql, array $params = []): string
    {
        return $this->connection->nativeCallableSql($sql, $params);
    }

    public function call(string $sql, array $params = []): Statement
    {
        return $this->connection->call($sql, $params);
    }

    /**
     * @return array<int, array{rows: array, rowCount: int, fields: array, command: string, lastId: int|false}>
     */
    public function queryMulti(string $sql, array $params = []): array
    {
        return $this->connection->queryMulti($sql, $params);
    }

    /**
     * @return array<int, array{rows: array, rowCount: int, fields: array, command: string, lastId: int|false}>
     */
    public function executeMulti(string $sql, array $params = []): array
    {
        return $this->connection->executeMulti($sql, $params);
    }

    /**
     * @return array{items: array<int, array{index: int, rowCount: int, fields: array, command: string, lastId: int|false}>, totalRowCount: int}
     */
    public function executeBatch(string $sql, iterable $batchParams): array
    {
        return $this->connection->executeBatch($sql, $batchParams);
    }

    /**
     * @return array{items: array<int, array{index: int, rowCount: int, fields: array, command: string, lastId: int|false}>, totalRowCount: int}
     */
    public function queryBatch(string $sql, iterable $batchParams): array
    {
        return $this->connection->queryBatch($sql, $batchParams);
    }

    /**
     * @return array<int, array{0: int}>
     */
    public function executeWithGeneratedKeys(string $sql, array $params = []): array
    {
        return $this->connection->executeWithGeneratedKeys($sql, $params);
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function getSchema(string $collectionName = 'tables', array $restrictions = []): array
    {
        return $this->connection->getSchema($collectionName, $restrictions);
    }

    /**
     * @return array{database: ?string, schemas: array<int, array{name: string, path: string, terminal: bool, children: array}>}
     */
    public function getSchemaTree(?bool $expandParents = null, ?string $database = null, array $restrictions = []): array
    {
        return $this->connection->getSchemaTree($expandParents, $database, $restrictions);
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function schemas(?string $catalog = null): array
    {
        return $this->connection->schemas($catalog);
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function tables(?string $schema = null, ?string $table = null, ?string $type = null): array
    {
        return $this->connection->tables($schema, $table, $type);
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function columns(?string $schema = null, ?string $table = null, ?string $column = null, ?string $type = null): array
    {
        return $this->connection->columns($schema, $table, $column, $type);
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function indexes(?string $schema = null, ?string $table = null, ?string $index = null): array
    {
        return $this->connection->indexes($schema, $table, $index);
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function indexColumns(?string $schema = null, ?string $table = null, ?string $index = null, ?string $column = null): array
    {
        return $this->connection->indexColumns($schema, $table, $index, $column);
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function constraints(?string $schema = null, ?string $table = null, ?string $constraint = null): array
    {
        return $this->connection->constraints($schema, $table, $constraint);
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function catalogs(?string $catalog = null): array
    {
        return $this->connection->catalogs($catalog);
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function primaryKeys(?string $catalog = null, ?string $schema = null, ?string $table = null, ?string $constraint = null): array
    {
        return $this->connection->primaryKeys($catalog, $schema, $table, $constraint);
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function foreignKeys(?string $catalog = null, ?string $schema = null, ?string $table = null, ?string $constraint = null): array
    {
        return $this->connection->foreignKeys($catalog, $schema, $table, $constraint);
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function procedures(?string $catalog = null, ?string $schema = null, ?string $procedure = null): array
    {
        return $this->connection->procedures($catalog, $schema, $procedure);
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function functions(?string $catalog = null, ?string $schema = null, ?string $function = null): array
    {
        return $this->connection->functions($catalog, $schema, $function);
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function routines(?string $catalog = null, ?string $schema = null, ?string $routine = null): array
    {
        return $this->connection->routines($catalog, $schema, $routine);
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function tablePrivileges(?string $catalog = null, ?string $schema = null, ?string $table = null): array
    {
        return $this->connection->tablePrivileges($catalog, $schema, $table);
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function columnPrivileges(?string $catalog = null, ?string $schema = null, ?string $table = null, ?string $column = null): array
    {
        return $this->connection->columnPrivileges($catalog, $schema, $table, $column);
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function typeInfo(?string $type = null): array
    {
        return $this->connection->typeInfo($type);
    }

    public function getSessionSchema(): ?string
    {
        return $this->connection->getSessionSchema();
    }

    public function setSessionSchema(?string $schema): void
    {
        $this->connection->setSessionSchema($schema);
    }

    public function exec(string $statement): int|false
    {
        $result = $this->connection->exec($statement);
        if ($result !== false) {
            return $result;
        }
        [$sqlState, , $message] = $this->connection->errorInfo();
        throw ErrorMapper::map((string) ($sqlState ?? ''), (string) ($message ?? 'statement execution failed'));
    }

    public function beginTransaction(): bool
    {
        return $this->connection->beginTransaction();
    }

    public function beginTransactionEx(array $options = []): bool
    {
        return $this->connection->beginTransactionEx($options);
    }

    public function commit(): bool
    {
        return $this->connection->commit();
    }

    public function inTransaction(): bool
    {
        return $this->connection->inTransaction();
    }

    public function rollBack(): bool
    {
        return $this->connection->rollBack();
    }

    public function savepoint(string $name): void
    {
        $this->connection->savepoint($name);
    }

    public function releaseSavepoint(string $name): void
    {
        $this->connection->releaseSavepoint($name);
    }

    public function rollbackToSavepoint(string $name): void
    {
        $this->connection->rollbackToSavepoint($name);
    }

    public function lastInsertId(?string $name = null): string|false
    {
        return $this->connection->lastInsertId($name);
    }

    public function setAttribute(int $attribute, mixed $value): bool
    {
        return $this->connection->setAttribute($attribute, $value);
    }

    public function getAttribute(int $attribute): mixed
    {
        return $this->connection->getAttribute($attribute);
    }

    public function errorInfo(): array
    {
        return $this->connection->errorInfo();
    }

    public function errorCode(): ?string
    {
        return $this->connection->errorCode();
    }

    public function close(): void
    {
        $this->connection->close();
    }
}
