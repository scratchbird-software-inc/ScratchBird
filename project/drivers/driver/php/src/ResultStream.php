<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

namespace ScratchBird\PDO;

final class ResultStream
{
    private Connection $connection;
    private array $columns = [];
    private int $rowsAffected = -1;
    private int $lastInsertId = 0;
    private string $commandTag = '';
    private int $completionCount = 0;
    private bool $done = false;
    private bool $hasNextResultSet = false;
    private bool $resultSetBoundary = false;
    private ?array $prefetchedMessage = null;

    public function __construct(Connection $connection)
    {
        $this->connection = $connection;
    }

    public function columns(): array
    {
        return $this->columns;
    }

    public function rowsAffected(): int
    {
        return $this->rowsAffected;
    }

    public function commandTag(): string
    {
        return $this->commandTag;
    }

    public function lastInsertId(): int
    {
        return $this->lastInsertId;
    }

    public function completionCount(): int
    {
        return $this->completionCount;
    }

    public function hasNextResultSet(): bool
    {
        return $this->hasNextResultSet;
    }

    public function nextResultSet(): bool
    {
        if ($this->done || !$this->hasNextResultSet) {
            return false;
        }
        $this->hasNextResultSet = false;
        $this->resultSetBoundary = false;
        $this->columns = [];
        $this->rowsAffected = -1;
        $this->lastInsertId = 0;
        $this->commandTag = '';
        return true;
    }

    public function readRow(): ?array
    {
        if ($this->done || $this->resultSetBoundary) {
            return null;
        }
        while (true) {
            [$type, , $payload] = $this->recvMessage();
            if ($this->connection->handleAsyncMessage($type, $payload)) {
                continue;
            }
            switch ($type) {
                case Protocol::MSG_ERROR:
                    throw $this->connection->buildQueryException($payload);
                case Protocol::MSG_ROW_DESCRIPTION:
                    $this->columns = Protocol::parseRowDescription($payload);
                    break;
                case Protocol::MSG_DATA_ROW:
                    $values = Protocol::parseDataRow($payload);
                    $row = [];
                    foreach ($values as $index => $value) {
                        $typeOid = $this->columns[$index]['typeOid'] ?? 0;
                        $format = $this->columns[$index]['format'] ?? TypeDecoder::FORMAT_BINARY;
                        $row[] = TypeDecoder::decode($typeOid, $value['data'], $format);
                    }
                    return $row;
                case Protocol::MSG_COMMAND_COMPLETE:
                    [, $rows, $lastId, $tag] = Protocol::parseCommandComplete($payload);
                    $this->commandTag = $tag;
                    $this->rowsAffected = (int)$rows;
                    $this->lastInsertId = (int)$lastId;
                    $this->completionCount++;
                    $this->connection->noteLastInsertId($this->lastInsertId);
                    $this->markResultSetBoundary();
                    return null;
                case Protocol::MSG_PORTAL_SUSPENDED:
                    $this->connection->allowPortalResume();
                    $this->connection->resumePortal();
                    break;
                case Protocol::MSG_READY:
                    [$status, $txnId] = Protocol::parseReady($payload);
                    $this->connection->updateReadyState($status, $txnId);
                    $this->done = true;
                    return null;
                case Protocol::MSG_EMPTY_QUERY:
                    break;
            }
        }
    }

    private function recvMessage(): array
    {
        if ($this->prefetchedMessage !== null) {
            $message = $this->prefetchedMessage;
            $this->prefetchedMessage = null;
            return $message;
        }
        return $this->connection->receive();
    }

    private function markResultSetBoundary(): void
    {
        while (true) {
            [$type, $flags, $payload] = $this->connection->receive();
            if ($this->connection->handleAsyncMessage($type, $payload)) {
                continue;
            }
            if ($type === Protocol::MSG_READY) {
                [$status, $txnId] = Protocol::parseReady($payload);
                $this->connection->updateReadyState($status, $txnId);
                $this->done = true;
                $this->hasNextResultSet = false;
                $this->resultSetBoundary = false;
                return;
            }
            $this->prefetchedMessage = [$type, $flags, $payload];
            $this->hasNextResultSet = true;
            $this->resultSetBoundary = true;
            return;
        }
    }
}
