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
use ScratchBird\PDO\Connection;
use ScratchBird\PDO\ErrorMapper;
use ScratchBird\PDO\ScratchBirdAuthException;
use ScratchBird\PDO\ScratchBirdConnectionException;
use ScratchBird\PDO\ScratchBirdDataException;
use ScratchBird\PDO\ScratchBirdException;
use ScratchBird\PDO\ScratchBirdIntegrityException;
use ScratchBird\PDO\ScratchBirdInternalException;
use ScratchBird\PDO\ScratchBirdNotSupportedException;
use ScratchBird\PDO\ScratchBirdSyntaxException;
use ScratchBird\PDO\ScratchBirdTransactionException;
use ScratchBird\PDO\ScratchBirdWarning;
use ScratchBird\PDO\RetryScope;

final class ErrorsTest extends TestCase
{
    public function testErrorMapperMapsRepresentativeSqlStatesToTypedExceptions(): void
    {
        $cases = [
            ['01000', ScratchBirdWarning::class],
            ['08006', ScratchBirdConnectionException::class],
            ['0A000', ScratchBirdNotSupportedException::class],
            ['22P02', ScratchBirdDataException::class],
            ['22ZZZ', ScratchBirdDataException::class],
            ['23505', ScratchBirdIntegrityException::class],
            ['28P01', ScratchBirdAuthException::class],
            ['42601', ScratchBirdSyntaxException::class],
            ['08ZZZ', ScratchBirdConnectionException::class],
            ['40001', ScratchBirdTransactionException::class],
            ['XX000', ScratchBirdInternalException::class],
            ['99999', ScratchBirdException::class],
            ['1234', ScratchBirdException::class],
        ];

        foreach ($cases as [$sqlState, $expectedClass]) {
            $exception = ErrorMapper::map($sqlState, 'boom', 'detail', 'hint');
            $this->assertInstanceOf($expectedClass, $exception, "sqlstate {$sqlState}");
            $this->assertSame($sqlState, $exception->sqlState);
            $this->assertSame('detail', $exception->detail);
            $this->assertSame('hint', $exception->hint);
        }
    }

    public function testBuildQueryExceptionParsesDetailAndHintFromErrorPayload(): void
    {
        $connection = $this->newConnectionWithoutConstructor();
        $payload = 'SERROR' . "\0"
            . 'C23505' . "\0"
            . 'Mduplicate key' . "\0"
            . 'DKey (id)=(1) already exists.' . "\0"
            . 'HUse a new id.' . "\0"
            . "\0";

        $exception = $connection->buildQueryException($payload);

        $this->assertInstanceOf(ScratchBirdIntegrityException::class, $exception);
        $this->assertSame('23505', $exception->sqlState);
        $this->assertSame('duplicate key', $exception->getMessage());
        $this->assertSame('Key (id)=(1) already exists.', $exception->detail);
        $this->assertSame('Use a new id.', $exception->hint);
    }

    public function testRecordErrorStoresSqlStateAndMessageForScratchBirdException(): void
    {
        $connection = $this->newConnectionWithoutConstructor();
        $recordError = $this->recordErrorMethod();

        $recordError->invoke($connection, new ScratchBirdDataException('bad integer input', '22P02'));

        $this->assertSame(['22P02', 0, 'bad integer input'], $connection->errorInfo());
    }

    public function testRecordErrorUsesHy000ForNonScratchBirdExceptions(): void
    {
        $connection = $this->newConnectionWithoutConstructor();
        $recordError = $this->recordErrorMethod();

        $recordError->invoke($connection, new RuntimeException('socket read failed'));

        $this->assertSame(['HY000', 0, 'socket read failed'], $connection->errorInfo());
    }

    public function testRetryScopeClassifiesStatementAndReconnectBoundaries(): void
    {
        $this->assertSame(RetryScope::STATEMENT, ErrorMapper::retryScopeForSqlState('40001'));
        $this->assertSame(RetryScope::STATEMENT, ErrorMapper::retryScopeForSqlState('40P01'));
        $this->assertSame(RetryScope::RECONNECT, ErrorMapper::retryScopeForSqlState('08006'));
        $this->assertSame(RetryScope::NONE, ErrorMapper::retryScopeForSqlState('57014'));
        $this->assertSame(RetryScope::NONE, ErrorMapper::retryScopeForSqlState(null));
    }

    public function testIsRetryableSqlStateOnlyAllowsFreshBoundaryRetries(): void
    {
        $this->assertTrue(ErrorMapper::isRetryableSqlState('40001'));
        $this->assertTrue(ErrorMapper::isRetryableSqlState('08003'));
        $this->assertFalse(ErrorMapper::isRetryableSqlState('57014'));
        $this->assertFalse(ErrorMapper::isRetryableSqlState(''));
    }

    private function newConnectionWithoutConstructor(): Connection
    {
        $class = new ReflectionClass(Connection::class);
        /** @var Connection $connection */
        $connection = $class->newInstanceWithoutConstructor();
        return $connection;
    }

    private function recordErrorMethod(): ReflectionMethod
    {
        $class = new ReflectionClass(Connection::class);
        $method = $class->getMethod('recordError');
        $method->setAccessible(true);
        return $method;
    }
}
