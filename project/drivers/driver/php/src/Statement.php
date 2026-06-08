<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

namespace ScratchBird\PDO;

final class Statement
{
    /** @var array<int, array{rows: array<int, array>, columns: array, rowCount: int, statusMessage: string, lastInsertId: int|false}> */
    private array $bufferedResultSets = [];
    private Connection $connection;
    private string $sql;
    private array $options;
    private array $boundValues = [];
    private array $boundParams = [];
    private ?int $bufferedResultIndex = null;
    private int $bufferedRowIndex = 0;
    private ?ResultStream $stream = null;
    private array $currentRow = [];
    private int $fetchMode = \PDO::FETCH_ASSOC;
    private int $rowCount = 0;
    private ?int $lastInsertId = null;
    private string $statusMessage = '';
    /** @var array<int, array{0: int}> */
    private array $generatedKeys = [];
    private int $lastCompletionCount = 0;

    public function __construct(Connection $connection, string $sql, array $options = [])
    {
        $this->connection = $connection;
        $this->sql = $sql;
        $this->options = $options;
    }

    public function bindParam(string|int $param, mixed &$var, int $type = \PDO::PARAM_STR, int $length = 0, mixed $driverOptions = null): bool
    {
        $this->boundParams[$param] = &$var;
        return true;
    }

    public function bindValue(string|int $param, mixed $value, int $type = \PDO::PARAM_STR): bool
    {
        $this->boundValues[$param] = $value;
        return true;
    }

    public function execute(?array $params = null): bool
    {
        $finalParams = $this->gatherParams($params);
        $normalized = Sql::normalize($this->sql, $finalParams);
        $this->resetExecutionState();
        $splitStatements = Sql::splitExecutableStatements($normalized['sql'], $normalized['params']);
        if ($splitStatements !== null) {
            $this->loadBufferedResultSets($splitStatements);
            return true;
        }
        $this->stream = $this->connection->executeQuery($normalized['sql'], $normalized['params']);
        return true;
    }

    public function fetch(int $mode = \PDO::FETCH_ASSOC, mixed ...$args): mixed
    {
        if ($this->bufferedResultIndex !== null) {
            $current = $this->currentBufferedResultSet();
            if ($current === null) {
                return false;
            }
            if (!array_key_exists($this->bufferedRowIndex, $current['rows'])) {
                return false;
            }
            $row = $current['rows'][$this->bufferedRowIndex];
            $this->bufferedRowIndex++;
            $this->currentRow = $row;
            return $this->formatRow($row, $mode);
        }
        if ($this->stream === null) {
            return false;
        }
        $row = $this->stream->readRow();
        if ($row === null) {
            $this->rowCount = $this->stream->rowsAffected();
            $this->statusMessage = $this->stream->commandTag();
            $this->lastInsertId = $this->stream->lastInsertId();
            $completionCount = $this->stream->completionCount();
            if ($completionCount > $this->lastCompletionCount) {
                $this->lastCompletionCount = $completionCount;
                $this->captureGeneratedKey($this->lastInsertId);
            }
            return false;
        }
        $this->currentRow = $row;
        return $this->formatRow($row, $mode);
    }

    public function fetchAll(int $mode = \PDO::FETCH_ASSOC, mixed ...$args): array
    {
        $rows = [];
        while (true) {
            $row = $this->fetch($mode);
            if ($row === false) {
                break;
            }
            $rows[] = $row;
        }
        return $rows;
    }

    public function fetchColumn(int $column = 0): mixed
    {
        $row = $this->fetch(\PDO::FETCH_NUM);
        if ($row === false) {
            return false;
        }
        return $row[$column] ?? false;
    }

    public function rowCount(): int
    {
        return $this->rowCount;
    }

    public function columnCount(): int
    {
        return count($this->currentColumns());
    }

    public function getColumnMeta(int $column): array
    {
        $meta = $this->currentColumns()[$column] ?? null;
        if ($meta === null) {
            return [];
        }
        return [
            'name' => $meta['name'],
            'native_type' => TypeDecoder::oidName($meta['typeOid']),
            'len' => $meta['typeModifier'],
            'format' => $meta['format'],
        ];
    }

    public function closeCursor(): bool
    {
        $this->stream = null;
        $this->resetExecutionState();
        return true;
    }

    public function setFetchMode(int $mode, mixed ...$args): bool
    {
        $this->fetchMode = $mode;
        return true;
    }

    public function nextRowset(): bool
    {
        if ($this->bufferedResultIndex !== null) {
            $nextIndex = $this->bufferedResultIndex + 1;
            if (!array_key_exists($nextIndex, $this->bufferedResultSets)) {
                return false;
            }
            $this->activateBufferedResultSet($nextIndex);
            return true;
        }
        if ($this->stream === null) {
            return false;
        }
        while ($this->stream->readRow() !== null) {
            // Drain active result set before advancing.
        }
        if (!$this->stream->hasNextResultSet()) {
            return false;
        }
        if (!$this->stream->nextResultSet()) {
            return false;
        }
        $this->rowCount = 0;
        $this->currentRow = [];
        $this->lastInsertId = null;
        $this->statusMessage = '';
        return true;
    }

    public function nextset(): bool
    {
        return $this->nextRowset();
    }

    public function statusMessage(): string
    {
        return $this->statusMessage;
    }

    public function lastInsertId(): int|false
    {
        return $this->lastInsertId ?? false;
    }

    /**
     * @return array<int, array{0: int}>
     */
    public function getGeneratedKeys(): array
    {
        return $this->generatedKeys;
    }

    public function fields(): array
    {
        return $this->currentColumns();
    }

    private function gatherParams(?array $params): array
    {
        $finalParams = [];
        foreach ($this->boundParams as $key => $value) {
            $finalParams[$key] = $value;
        }
        foreach ($this->boundValues as $key => $value) {
            $finalParams[$key] = $value;
        }
        if ($params !== null) {
            foreach ($params as $key => $value) {
                $finalParams[$key] = $value;
            }
        }
        return $finalParams;
    }

    private function formatRow(array $row, int $mode): array
    {
        $columns = $this->currentColumns();
        if ($mode === \PDO::FETCH_NUM) {
            return array_values($row);
        }
        if ($mode === \PDO::FETCH_ASSOC) {
            $assoc = [];
            foreach ($row as $idx => $value) {
                $name = $columns[$idx]['name'] ?? (string)$idx;
                $assoc[$name] = $value;
            }
            return $assoc;
        }
        if ($mode === \PDO::FETCH_BOTH) {
            $assoc = $this->formatRow($row, \PDO::FETCH_ASSOC);
            foreach ($row as $idx => $value) {
                $assoc[$idx] = $value;
            }
            return $assoc;
        }
        return $row;
    }

    private function resetExecutionState(): void
    {
        $this->bufferedResultSets = [];
        $this->bufferedResultIndex = null;
        $this->bufferedRowIndex = 0;
        $this->stream = null;
        $this->rowCount = 0;
        $this->currentRow = [];
        $this->lastInsertId = null;
        $this->statusMessage = '';
        $this->generatedKeys = [];
        $this->lastCompletionCount = 0;
    }

    private function captureGeneratedKey(?int $lastInsertId): void
    {
        if ($lastInsertId === null) {
            return;
        }
        $this->generatedKeys[] = [$lastInsertId];
    }

    /**
     * @param array<int, array{sql: string, params: array<int, mixed>}> $splitStatements
     */
    private function loadBufferedResultSets(array $splitStatements): void
    {
        foreach ($splitStatements as $statement) {
            $stream = $this->connection->executeQuery($statement['sql'], $statement['params']);
            $rows = [];
            while (true) {
                $row = $stream->readRow();
                if ($row === null) {
                    break;
                }
                $rows[] = $row;
            }
            if ($stream->completionCount() > 0) {
                $this->captureGeneratedKey($stream->lastInsertId());
            }
            $this->bufferedResultSets[] = [
                'rows' => $rows,
                'columns' => $stream->columns(),
                'rowCount' => $stream->rowsAffected(),
                'statusMessage' => $stream->commandTag(),
                'lastInsertId' => $stream->lastInsertId(),
            ];
        }
        if ($this->bufferedResultSets !== []) {
            $this->activateBufferedResultSet(0);
        }
    }

    private function activateBufferedResultSet(int $index): void
    {
        $this->bufferedResultIndex = $index;
        $this->bufferedRowIndex = 0;
        $this->currentRow = [];
        $current = $this->bufferedResultSets[$index];
        $this->rowCount = $current['rowCount'];
        $this->statusMessage = $current['statusMessage'];
        $this->lastInsertId = $current['lastInsertId'] === false ? null : (int) $current['lastInsertId'];
    }

    /**
     * @return array{rows: array<int, array>, columns: array, rowCount: int, statusMessage: string, lastInsertId: int|false}|null
     */
    private function currentBufferedResultSet(): ?array
    {
        if ($this->bufferedResultIndex === null) {
            return null;
        }
        return $this->bufferedResultSets[$this->bufferedResultIndex] ?? null;
    }

    private function currentColumns(): array
    {
        $current = $this->currentBufferedResultSet();
        if ($current !== null) {
            return $current['columns'];
        }
        return $this->stream?->columns() ?? [];
    }
}
