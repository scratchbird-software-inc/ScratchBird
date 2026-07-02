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
use ScratchBird\PDO\Config;
use ScratchBird\PDO\Connection;
use ScratchBird\PDO\Protocol;
use ScratchBird\PDO\ScratchBirdException;
use ScratchBird\PDO\ScratchBirdNotSupportedException;
use ScratchBird\PDO\ScratchBirdTransactionException;
use ScratchBird\PDO\TypeDecoder;

final class ConnectionTxnExecTest extends TestCase
{
    public function testCommitAfterServerAbortResetsTxnStateAfterReady(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);
        $this->setPrivate($conn, 'inTransaction', true);
        $this->setPrivate($conn, 'txnId', 91);

        try {
            $this->queueError($server, '40001', 'serialization failure');
            $this->queueReady($server, 0);

            try {
                $conn->commit();
                $this->fail('Expected commit() to surface server abort');
            } catch (ScratchBirdTransactionException $ex) {
                $this->assertSame('40001', $ex->sqlState);
            }

            $this->assertFalse($this->getPrivate($conn, 'inTransaction'));
            $this->assertSame(0, $this->getPrivate($conn, 'txnId'));
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testCommitWithoutActiveTransactionThrows(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);

        try {
            $conn->commit();
            $this->fail('Expected commit() to reject when no transaction is active');
        } catch (ScratchBirdTransactionException $ex) {
            $this->assertStringContainsString('No active transaction', $ex->getMessage());
            $this->assertSame('25000', $ex->sqlState);
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testTransactionLifecycleTracksStateAndWireMessages(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);

        try {
            $this->queueReady($server, 11, ord('T'));
            $this->assertTrue($conn->beginTransaction());
            $this->assertTrue($conn->inTransaction());
            $this->assertSame(Protocol::MSG_TXN_BEGIN, $this->readSentMessageType($server));

            $this->queueReady($server, 11, ord('T'));
            $conn->savepoint('sp_one');
            $this->assertSame(Protocol::MSG_TXN_SAVEPOINT, $this->readSentMessageType($server));

            $this->queueReady($server, 0);
            $this->assertTrue($conn->commit());
            $this->assertFalse($conn->inTransaction());
            $this->assertSame(Protocol::MSG_TXN_COMMIT, $this->readSentMessageType($server));
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testBeginTransactionExEncodesReadCommittedMode(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);

        try {
            $this->queueReady($server, 14);
            $this->assertTrue($conn->beginTransactionEx([
                'isolation_level' => Protocol::ISOLATION_READ_COMMITTED,
                'read_committed_mode' => Protocol::READ_COMMITTED_MODE_READ_CONSISTENCY,
            ]));

            [$type, $payload] = $this->readSentMessage($server);
            $this->assertSame(Protocol::MSG_TXN_BEGIN, $type);
            $this->assertSame(16, strlen($payload));
            $flags = unpack('vflags', substr($payload, 0, 2))['flags'];
            $this->assertSame(
                Protocol::TXN_FLAG_HAS_READ_COMMITTED_MODE,
                $flags & Protocol::TXN_FLAG_HAS_READ_COMMITTED_MODE
            );
            $this->assertSame(
                Protocol::READ_COMMITTED_MODE_READ_CONSISTENCY,
                ord($payload[12])
            );
            $this->assertSame(
                'READ COMMITTED READ CONSISTENCY',
                Protocol::canonicalReadCommittedModeLabel(ord($payload[12]))
            );
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testBeginTransactionExRejectsReadCommittedModeWithSnapshotAlias(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);

        try {
            $this->expectException(ScratchBirdNotSupportedException::class);
            $this->expectExceptionMessage('read_committed_mode requires a READ COMMITTED isolation alias');
            $conn->beginTransactionEx([
                'isolation_level' => Protocol::ISOLATION_SERIALIZABLE,
                'read_committed_mode' => Protocol::READ_COMMITTED_MODE_READ_CONSISTENCY,
            ]);
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testReadyStatusKeepsFreshNativeBoundaryActiveWithZeroTxnId(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);

        try {
            $conn->updateReadyState(ord('T'), 0);
            $this->assertTrue($conn->inTransaction());
            $this->assertSame(0, $this->getPrivate($conn, 'txnId'));
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testBeginTransactionRestartsImplicitBoundaryAndSendsTxnBegin(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);
        $conn->updateReadyState(ord('T'), 0);

        try {
            $this->queueReady($server, 41, ord('T'));
            $this->assertTrue($conn->beginTransaction());
            $this->assertTrue($conn->inTransaction());
            $this->assertTrue($this->getPrivate($conn, 'explicitTransaction'));
            $this->assertSame(41, $this->getPrivate($conn, 'txnId'));
            $this->assertSame(Protocol::MSG_TXN_BEGIN, $this->readSentMessageType($server));
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testBeginTransactionAllowsNonDefaultRestartOptions(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);
        $conn->updateReadyState(ord('T'), 0);

        try {
            $this->queueReady($server, 42, ord('T'));
            $this->assertTrue($conn->beginTransactionEx([
                'isolation_level' => Protocol::ISOLATION_SERIALIZABLE,
            ]));
            $this->assertSame(42, $this->getPrivate($conn, 'txnId'));
            $this->assertSame(Protocol::MSG_TXN_BEGIN, $this->readSentMessageType($server));
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testCommitDrainsImmediateReopenBoundary(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);
        $this->setPrivate($conn, 'runtimeTxnActive', true);
        $this->setPrivate($conn, 'explicitTransaction', true);
        $this->setPrivate($conn, 'inTransaction', true);

        try {
            $this->queueReady($server, 0);
            $this->queueReady($server, 0, ord('T'));
            $this->assertTrue($conn->commit());
            $this->assertTrue($conn->inTransaction());
            $this->assertFalse($this->getPrivate($conn, 'explicitTransaction'));
            $this->assertSame(Protocol::MSG_TXN_COMMIT, $this->readSentMessageType($server));
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testPreparedTransactionHelpersEmitCanonicalControlSql(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);

        try {
            $this->queueReady($server, 0);
            $this->assertTrue($conn->prepareTransaction("gid'alpha"));
            [$type, $payload] = $this->readSentMessage($server);
            $this->assertSame(Protocol::MSG_QUERY, $type);
            $this->assertSame("PREPARE TRANSACTION 'gid''alpha'", $this->parseSimpleQuerySql($payload));

            $this->queueReady($server, 0);
            $this->assertTrue($conn->commitPrepared("gid'alpha"));
            [$type, $payload] = $this->readSentMessage($server);
            $this->assertSame(Protocol::MSG_QUERY, $type);
            $this->assertSame("COMMIT PREPARED 'gid''alpha'", $this->parseSimpleQuerySql($payload));

            $this->queueReady($server, 0);
            $this->assertTrue($conn->rollbackPrepared("gid'alpha"));
            [$type, $payload] = $this->readSentMessage($server);
            $this->assertSame(Protocol::MSG_QUERY, $type);
            $this->assertSame("ROLLBACK PREPARED 'gid''alpha'", $this->parseSimpleQuerySql($payload));
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testDormantHelpersFailClosedAndCapabilitiesStayExplicit(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);

        try {
            $this->assertTrue($conn->supportsPreparedTransactions());
            $this->assertFalse($conn->supportsDormantReattach());

            $this->expectException(ScratchBirdNotSupportedException::class);
            $this->expectExceptionMessage('dormant detach/reattach is not yet exposed by the public PHP driver surface');
            $conn->detachToDormant();
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testResumePortalRejectsUnsuspendedState(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);

        try {
            $this->expectException(ScratchBirdException::class);
            $this->expectExceptionMessage('portal resume requires explicit suspended state');
            $conn->resumePortal();
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testSavepointRejectsBlankName(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);
        $this->setPrivate($conn, 'inTransaction', true);
        $this->setPrivate($conn, 'txnId', 12);

        try {
            $conn->savepoint('   ');
            $this->fail('Expected blank savepoint name to fail validation');
        } catch (ScratchBirdTransactionException $ex) {
            $this->assertStringContainsString('must not be empty', $ex->getMessage());
            $this->assertSame('3B001', $ex->sqlState);
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testExecReturnsRowsAffectedFromCommandComplete(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);

        try {
            $this->queueCommandComplete($server, 3, 'UPDATE 3');
            $this->queueReady($server, 0);
            $this->assertSame(3, $conn->exec('UPDATE t SET v = 1'));
            $this->assertSame(Protocol::MSG_QUERY, $this->readSentMessageType($server));
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testExecReturnsFalseAndRecordsErrorState(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);

        try {
            $this->queueError($server, '23505', 'duplicate key');
            $this->queueReady($server, 0);
            $this->assertFalse($conn->exec('INSERT INTO t(v) VALUES (1)'));
            $this->assertSame('23505', $conn->errorCode());
            $this->assertSame('duplicate key', $conn->errorInfo()[2]);
            $this->assertSame(Protocol::MSG_QUERY, $this->readSentMessageType($server));
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testQueryMultiTraversesMultipleResultBoundaries(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);

        try {
            $this->queueCommandComplete($server, 2, 'UPDATE 2', 7);
            $this->queueReady($server, 0);
            $this->queueCommandComplete($server, 1, 'UPDATE 1', 9);
            $this->queueReady($server, 0);

            $results = $conn->queryMulti('UPDATE t SET v = 1; UPDATE t SET v = 2');
            $this->assertCount(2, $results);
            $this->assertSame(2, $results[0]['rowCount']);
            $this->assertSame('UPDATE 2', $results[0]['command']);
            $this->assertSame(7, $results[0]['lastId']);
            $this->assertSame(1, $results[1]['rowCount']);
            $this->assertSame('UPDATE 1', $results[1]['command']);
            $this->assertSame(9, $results[1]['lastId']);
            $this->assertSame(Protocol::MSG_QUERY, $this->readSentMessageType($server));
            $this->assertSame(Protocol::MSG_QUERY, $this->readSentMessageType($server));
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testExecuteBatchReturnsSummaryAndTotalRowCount(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);

        try {
            $this->queueCommandComplete($server, 2, 'UPDATE 2', 10);
            $this->queueReady($server, 0);
            $this->queueCommandComplete($server, 3, 'UPDATE 3', 11);
            $this->queueReady($server, 0);

            $result = $conn->executeBatch('UPDATE t SET v = 1', [[], []]);
            $this->assertSame(5, $result['totalRowCount']);
            $this->assertCount(2, $result['items']);
            $this->assertSame(0, $result['items'][0]['index']);
            $this->assertSame(2, $result['items'][0]['rowCount']);
            $this->assertSame('UPDATE 2', $result['items'][0]['command']);
            $this->assertSame(10, $result['items'][0]['lastId']);
            $this->assertSame(1, $result['items'][1]['index']);
            $this->assertSame(3, $result['items'][1]['rowCount']);
            $this->assertSame('UPDATE 3', $result['items'][1]['command']);
            $this->assertSame(11, $result['items'][1]['lastId']);
            $this->assertSame(Protocol::MSG_QUERY, $this->readSentMessageType($server));
            $this->assertSame(Protocol::MSG_QUERY, $this->readSentMessageType($server));
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testExecuteWithGeneratedKeysAccumulatesAcrossResultSets(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);

        try {
            $this->queueCommandComplete($server, 1, 'INSERT 1', 101);
            $this->queueCommandComplete($server, 1, 'INSERT 1', 102);
            $this->queueReady($server, 0);

            $keys = $conn->executeWithGeneratedKeys('INSERT INTO t(v) VALUES (1)');
            $this->assertSame([[101], [102]], $keys);
            $this->assertSame(Protocol::MSG_QUERY, $this->readSentMessageType($server));
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testExecuteQueryResumesPortalAfterSuspensionAndContinuesRows(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);

        try {
            $this->queueRowDescription($server, ['value']);
            $this->queueDataRow($server, ['1']);
            $this->sendServerMessage($server, Protocol::MSG_PORTAL_SUSPENDED, '');
            $this->queueDataRow($server, ['2']);
            $this->queueCommandComplete($server, 2, 'SELECT 2');
            $this->queueReady($server, 0);

            $stream = $conn->executeQuery('SELECT 1', [], 1);
            $rows = [];
            while (($row = $stream->readRow()) !== null) {
                $rows[] = $row;
            }

            $this->assertSame([['1'], ['2']], $rows);
            $this->assertSame(2, $stream->rowsAffected());
            $this->assertSame(Protocol::MSG_QUERY, $this->readSentMessageType($server));
            $this->assertSame(Protocol::MSG_EXECUTE, $this->readSentMessageType($server));
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testExecuteQueryAppliesReadyTxnStateAtStreamEnd(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);

        try {
            $this->queueCommandComplete($server, 0, 'SELECT 0');
            $this->queueReady($server, 77, ord('T'));

            $stream = $conn->executeQuery('SELECT 1');
            $this->assertNull($stream->readRow());
            $this->assertTrue($conn->inTransaction());
            $this->assertSame(77, $this->getPrivate($conn, 'txnId'));
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testCallTranslatesJdbcCallableEscapeForSimpleCall(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);

        try {
            $this->queueCommandComplete($server, 0, 'CALL', 0);
            $this->queueReady($server, 0);
            $stmt = $conn->call('{call maintenance.reindex}');
            $stmt->fetchAll(\PDO::FETCH_ASSOC);

            [$type, $payload] = $this->readSentMessage($server);
            $this->assertSame(Protocol::MSG_QUERY, $type);
            $this->assertSame('call maintenance.reindex', $this->parseSimpleQuerySql($payload));
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testNativeSqlAndNativeCallableSqlNormalizePlaceholders(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);

        try {
            $this->assertSame('SELECT $1::int AS id', $conn->nativeSql('SELECT ?::int AS id', [42]));
            $this->assertSame(
                'select math.add($1, $2) as return_value',
                $conn->nativeCallableSql('{? = call math.add(?, ?)}', [5, 7])
            );
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testSetSessionSchemaNormalizesPublicAliasAndResetFallback(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);

        try {
            $this->queueCommandComplete($server, 0, 'SET');
            $this->queueReady($server, 0);
            $conn->setSessionSchema('public');
            $this->assertSame('users.public', $conn->getSessionSchema());
            [$type, $payload] = $this->readSentMessage($server);
            $this->assertSame(Protocol::MSG_QUERY, $type);
            $this->assertSame('SET SCHEMA "users.public"', $this->parseSimpleQuerySql($payload));

            $this->queueCommandComplete($server, 0, 'SET');
            $this->queueReady($server, 0);
            $conn->setSessionSchema(null);
            $this->assertNull($conn->getSessionSchema());
            [$type, $payload] = $this->readSentMessage($server);
            $this->assertSame(Protocol::MSG_QUERY, $type);
            $this->assertSame('SET SCHEMA "users.public"', $this->parseSimpleQuerySql($payload));
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    private function newConnectionWithSocket($socket): Connection
    {
        $class = new ReflectionClass(Connection::class);
        /** @var Connection $conn */
        $conn = $class->newInstanceWithoutConstructor();
        $cfg = new Config();
        $cfg->socketTimeoutMs = 0;
        $this->setPrivate($conn, 'config', $cfg);
        $this->setPrivate($conn, 'socket', $socket);
        $this->setPrivate($conn, 'attachmentId', str_repeat("\0", 16));
        $this->setPrivate($conn, 'txnId', 0);
        $this->setPrivate($conn, 'inTransaction', false);
        $this->setPrivate($conn, 'sequence', 1);
        $this->setPrivate($conn, 'lastQuerySequence', 0);
        $this->setPrivate($conn, 'lastMaxRows', 0);
        $this->setPrivate($conn, 'connected', true);
        $this->setPrivate($conn, 'attributes', []);
        $this->setPrivate($conn, 'parameters', []);
        $this->setPrivate($conn, 'lastError', ['00000', 0, null]);
        $this->setPrivate($conn, 'notificationHandlers', []);
        $this->setPrivate($conn, 'lastPlan', null);
        $this->setPrivate($conn, 'lastSblr', null);
        $this->setPrivate($conn, 'hasLastInsertId', false);
        $this->setPrivate($conn, 'lastInsertIdValue', 0);
        return $conn;
    }

    private function setPrivate(object $object, string $property, mixed $value): void
    {
        $class = new ReflectionClass($object);
        $prop = $class->getProperty($property);
        $prop->setAccessible(true);
        $prop->setValue($object, $value);
    }

    private function getPrivate(object $object, string $property): mixed
    {
        $class = new ReflectionClass($object);
        $prop = $class->getProperty($property);
        $prop->setAccessible(true);
        return $prop->getValue($object);
    }

    private function newSocketPair(): array
    {
        $pair = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
        if ($pair === false) {
            $listener = stream_socket_server('tcp://127.0.0.1:0', $errno, $errstr);
            if ($listener === false) {
                $this->fail('socket pair or TCP loopback listener is required for wire-fixture tests: ' . $errstr);
            }
            $address = stream_socket_get_name($listener, false);
            if ($address === false) {
                fclose($listener);
                $this->fail('TCP loopback listener did not expose an address');
            }
            $client = stream_socket_client('tcp://' . $address, $errno, $errstr, 1);
            if ($client === false) {
                fclose($listener);
                $this->fail('TCP loopback client connection failed for wire-fixture tests: ' . $errstr);
            }
            $server = stream_socket_accept($listener, 1);
            fclose($listener);
            if ($server === false) {
                fclose($client);
                $this->fail('TCP loopback accept failed for wire-fixture tests');
            }
            $pair = [$client, $server];
        }
        stream_set_blocking($pair[0], true);
        stream_set_blocking($pair[1], true);
        return $pair;
    }

    private function readSentMessageType($server): int
    {
        $header = $this->readExact($server, Protocol::HEADER_SIZE);
        [$type, , $length] = Protocol::decodeHeader($header);
        if ($length > 0) {
            $this->readExact($server, $length);
        }
        return $type;
    }

    /**
     * @return array{0: int, 1: string}
     */
    private function readSentMessage($server): array
    {
        $header = $this->readExact($server, Protocol::HEADER_SIZE);
        [$type, , $length] = Protocol::decodeHeader($header);
        $payload = $length > 0 ? $this->readExact($server, $length) : '';
        return [$type, $payload];
    }

    private function parseSimpleQuerySql(string $payload): string
    {
        if (strlen($payload) < 12) {
            $this->fail('query payload is truncated');
        }
        $sqlBytes = substr($payload, 12);
        $nullPos = strpos($sqlBytes, "\0");
        if ($nullPos === false) {
            return $sqlBytes;
        }
        return substr($sqlBytes, 0, $nullPos);
    }

    private function queueReady($server, int $txnId, int $status = 0): void
    {
        $payload = chr($status) . "\0\0\0" . $this->uint64Le($txnId) . $this->uint64Le(0);
        $this->sendServerMessage($server, Protocol::MSG_READY, $payload);
    }

    private function queueCommandComplete($server, int $rows, string $tag, int $lastId = 0): void
    {
        $payload = chr(0) . "\0\0\0" . $this->uint64Le($rows) . $this->uint64Le($lastId) . $tag . "\0";
        $this->sendServerMessage($server, Protocol::MSG_COMMAND_COMPLETE, $payload);
    }

    /**
     * @param array<int, string> $columnNames
     */
    private function queueRowDescription($server, array $columnNames): void
    {
        $payload = pack('v', count($columnNames)) . "\0\0";
        foreach ($columnNames as $index => $columnName) {
            $name = (string) $columnName;
            $payload .= pack('V', strlen($name));
            $payload .= $name;
            $payload .= pack('V', 0);
            $payload .= pack('v', $index + 1);
            $payload .= pack('V', TypeDecoder::OID_TEXT);
            $payload .= pack('v', 0);
            $payload .= pack('V', 0);
            $payload .= chr(TypeDecoder::FORMAT_TEXT);
            $payload .= chr(1);
            $payload .= "\0\0";
        }
        $this->sendServerMessage($server, Protocol::MSG_ROW_DESCRIPTION, $payload);
    }

    /**
     * @param array<int, mixed> $values
     */
    private function queueDataRow($server, array $values): void
    {
        $count = count($values);
        $nullBytes = intdiv($count + 7, 8);
        $nullBitmap = str_repeat("\0", $nullBytes);
        $encoded = '';
        foreach ($values as $index => $value) {
            if ($value === null) {
                $byteIndex = intdiv($index, 8);
                $bitIndex = $index % 8;
                $byte = ord($nullBitmap[$byteIndex]);
                $nullBitmap[$byteIndex] = chr($byte | (1 << $bitIndex));
                continue;
            }
            $raw = (string) $value;
            $encoded .= pack('V', strlen($raw)) . $raw;
        }
        $payload = pack('v', $count) . pack('v', $nullBytes) . $nullBitmap . $encoded;
        $this->sendServerMessage($server, Protocol::MSG_DATA_ROW, $payload);
    }

    private function queueError($server, string $sqlState, string $message): void
    {
        $payload = 'SERROR' . "\0" . 'C' . $sqlState . "\0" . 'M' . $message . "\0" . "\0";
        $this->sendServerMessage($server, Protocol::MSG_ERROR, $payload);
    }

    private function sendServerMessage($server, int $type, string $payload): void
    {
        $frame = Protocol::encodeMessage($type, $payload, 0, 1, str_repeat("\0", 16), 0);
        $this->writeExact($server, $frame);
    }

    private function writeExact($stream, string $data): void
    {
        $offset = 0;
        $length = strlen($data);
        while ($offset < $length) {
            $written = fwrite($stream, substr($data, $offset));
            if ($written === false || $written === 0) {
                $this->fail('Failed writing fixture message');
            }
            $offset += $written;
        }
    }

    private function readExact($stream, int $length): string
    {
        $data = '';
        while (strlen($data) < $length) {
            $chunk = fread($stream, $length - strlen($data));
            if ($chunk === false || $chunk === '') {
                $this->fail('Failed reading fixture message');
            }
            $data .= $chunk;
        }
        return $data;
    }

    private function uint64Le(int $value): string
    {
        $lo = $value & 0xFFFFFFFF;
        $hi = ($value >> 32) & 0xFFFFFFFF;
        return pack('V2', $lo, $hi);
    }
}
