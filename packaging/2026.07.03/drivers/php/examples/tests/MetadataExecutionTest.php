<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

require_once __DIR__ . '/bootstrap.php';
require_once dirname(__DIR__) . '/src/CircuitBreaker.php';

if (!class_exists('ScratchBird\\TelemetryCollector')) {
    class MetadataExecutionTelemetryCollectorStub
    {
        public function startSpan(string $name): mixed
        {
            return null;
        }

        public function endSpan(mixed $span, bool $success = true): void
        {
        }

        public static function sanitizeQuery(?string $sql): ?string
        {
            return $sql;
        }
    }
    class_alias(MetadataExecutionTelemetryCollectorStub::class, 'ScratchBird\\TelemetryCollector');
}

use PHPUnit\Framework\TestCase;
use ScratchBird\CircuitBreaker;
use ScratchBird\TelemetryCollector;
use ScratchBird\PDO\Config;
use ScratchBird\PDO\Connection;
use ScratchBird\PDO\Metadata;
use ScratchBird\PDO\Protocol;
use ScratchBird\PDO\ScratchBirdPDO;
use ScratchBird\PDO\ScratchBirdNotSupportedException;
use ScratchBird\PDO\TypeDecoder;

final class MetadataExecutionTest extends TestCase
{
    public function testNormalizeCollectionNameSupportsExtendedAliases(): void
    {
        $this->assertSame('catalogs', Metadata::normalizeCollectionName('catalog'));
        $this->assertSame('primary_keys', Metadata::normalizeCollectionName('primaryKeys'));
        $this->assertSame('foreign_keys', Metadata::normalizeCollectionName('foreign-keys'));
        $this->assertSame('table_privileges', Metadata::normalizeCollectionName('table privileges'));
        $this->assertSame('column_privileges', Metadata::normalizeCollectionName('columnprivilege'));
        $this->assertSame('type_info', Metadata::normalizeCollectionName('typeinfo'));
    }

    public function testResolveCollectionQuerySupportsExtendedFamilies(): void
    {
        $this->assertSame(Metadata::CATALOGS_QUERY, Metadata::resolveCollectionQuery('catalog'));
        $this->assertSame(Metadata::PRIMARY_KEYS_QUERY, Metadata::resolveCollectionQuery('primarykeys'));
        $this->assertSame(Metadata::FOREIGN_KEYS_QUERY, Metadata::resolveCollectionQuery('foreign_keys'));
        $this->assertSame(Metadata::PROCEDURES_QUERY, Metadata::resolveCollectionQuery('procedure'));
        $this->assertSame(Metadata::FUNCTIONS_QUERY, Metadata::resolveCollectionQuery('functions'));
        $this->assertSame(Metadata::ROUTINES_QUERY, Metadata::resolveCollectionQuery('routine'));
        $this->assertSame(Metadata::TABLE_PRIVILEGES_QUERY, Metadata::resolveCollectionQuery('tableprivileges'));
        $this->assertSame(Metadata::COLUMN_PRIVILEGES_QUERY, Metadata::resolveCollectionQuery('column_privileges'));
        $this->assertSame(Metadata::TYPE_INFO_QUERY, Metadata::resolveCollectionQuery('type_info'));
    }

    public function testGetSchemaExecutesResolvedMetadataQuery(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);

        try {
            $this->queueCommandComplete($server, 0, 'SELECT 0');
            $this->queueReady($server, 0);

            $rows = $conn->getSchema('primaryKeys');
            $this->assertSame([], $rows);

            [$type, $payload] = $this->readSentMessage($server);
            $this->assertSame(Protocol::MSG_QUERY, $type);
            $this->assertSame(Metadata::PRIMARY_KEYS_QUERY, $this->extractQuerySql($payload));
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testFilterRowsByRestrictionsSupportsAliasesAndNull(): void
    {
        $rows = [
            ['schema_name' => 'sys', 'table_name' => 'events', 'owner_id' => null],
            ['schema_name' => 'users', 'table_name' => 'events', 'owner_id' => null],
            ['schema_name' => 'users', 'table_name' => 'profiles', 'owner_id' => 7],
        ];

        $filtered = Metadata::filterRowsByRestrictions($rows, ['schema' => 'users', 'table' => 'events'], 'tables');
        $this->assertSame([['schema_name' => 'users', 'table_name' => 'events', 'owner_id' => null]], $filtered);

        $filtered = Metadata::filterRowsByRestrictions(
            $rows,
            ['owner_id' => 'null', 'missing_filter' => 'ignored'],
            'tables'
        );
        $this->assertCount(2, $filtered);
        $this->assertSame('sys', $filtered[0]['schema_name']);
        $this->assertSame('users', $filtered[1]['schema_name']);
    }

    public function testGetSchemaSupportsRestrictions(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);

        try {
            $this->queueRowDescription($server, ['schema_name', 'table_name', 'owner_id']);
            $this->queueDataRow($server, ['sys', 'events', null]);
            $this->queueDataRow($server, ['users', 'events', null]);
            $this->queueDataRow($server, ['users', 'profiles', '7']);
            $this->queueCommandComplete($server, 3, 'SELECT 3');
            $this->queueReady($server, 0);

            $rows = $conn->getSchema('tables', ['schema' => 'users', 'table' => 'events']);
            $this->assertCount(1, $rows);
            $this->assertSame('users', $rows[0]['schema_name']);
            $this->assertSame('events', $rows[0]['table_name']);
            $this->assertNull($rows[0]['owner_id']);
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testProcedureAndRoutineConvenienceWrappersApplyRestrictions(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);

        try {
            $this->queueRowDescription($server, ['schema_name', 'procedure_name', 'routine_type']);
            $this->queueDataRow($server, ['users', 'upsert_event', 'PROCEDURE']);
            $this->queueDataRow($server, ['sys', 'other_proc', 'PROCEDURE']);
            $this->queueCommandComplete($server, 2, 'SELECT 2');
            $this->queueReady($server, 0);

            $rows = $conn->procedures(null, 'users', 'upsert_event');
            $this->assertSame([['schema_name' => 'users', 'procedure_name' => 'upsert_event', 'routine_type' => 'PROCEDURE']], $rows);
            [$type, $payload] = $this->readSentMessage($server);
            $this->assertSame(Protocol::MSG_QUERY, $type);
            $this->assertSame(Metadata::PROCEDURES_QUERY, $this->extractQuerySql($payload));

            $this->queueRowDescription($server, ['schema_name', 'routine_name', 'routine_type']);
            $this->queueDataRow($server, ['users', 'event_count', 'FUNCTION']);
            $this->queueDataRow($server, ['sys', 'other_routine', 'PROCEDURE']);
            $this->queueCommandComplete($server, 2, 'SELECT 2');
            $this->queueReady($server, 0);

            $routineRows = $conn->routines(null, 'users', 'event_count');
            $this->assertSame([['schema_name' => 'users', 'routine_name' => 'event_count', 'routine_type' => 'FUNCTION']], $routineRows);
            [$type, $payload] = $this->readSentMessage($server);
            $this->assertSame(Protocol::MSG_QUERY, $type);
            $this->assertSame(Metadata::ROUTINES_QUERY, $this->extractQuerySql($payload));
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testScratchBirdPdoMetadataConvenienceWrapperForwardsToConnection(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);
        $pdoClass = new ReflectionClass(ScratchBirdPDO::class);
        /** @var ScratchBirdPDO $pdo */
        $pdo = $pdoClass->newInstanceWithoutConstructor();
        $this->setPrivate($pdo, 'connection', $conn);

        try {
            $this->queueRowDescription($server, ['schema_name', 'function_name', 'routine_type']);
            $this->queueDataRow($server, ['users', 'event_count', 'FUNCTION']);
            $this->queueCommandComplete($server, 1, 'SELECT 1');
            $this->queueReady($server, 0);

            $rows = $pdo->functions(null, 'users', 'event_count');
            $this->assertSame([['schema_name' => 'users', 'function_name' => 'event_count', 'routine_type' => 'FUNCTION']], $rows);
            [$type, $payload] = $this->readSentMessage($server);
            $this->assertSame(Protocol::MSG_QUERY, $type);
            $this->assertSame(Metadata::FUNCTIONS_QUERY, $this->extractQuerySql($payload));
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testGetSchemaRejectsUnsupportedCollection(): void
    {
        [$client, $server] = $this->newSocketPair();
        $conn = $this->newConnectionWithSocket($client);

        try {
            $conn->getSchema('unsupported_collection');
            $this->fail('Expected unsupported metadata collection to throw');
        } catch (ScratchBirdNotSupportedException $ex) {
            $this->assertSame('0A000', $ex->sqlState);
            $this->assertStringContainsString('not supported', strtolower($ex->getMessage()));
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
        $this->setPrivate($conn, 'circuitBreaker', new CircuitBreaker());
        $this->setPrivate($conn, 'telemetry', new TelemetryCollector());
        return $conn;
    }

    private function setPrivate(object $object, string $property, mixed $value): void
    {
        $class = new ReflectionClass($object);
        $prop = $class->getProperty($property);
        $prop->setAccessible(true);
        $prop->setValue($object, $value);
    }

    private function newSocketPair(): array
    {
        $pair = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
        if ($pair === false) {
            $listener = stream_socket_server('tcp://127.0.0.1:0', $errno, $errstr);
            if ($listener === false) {
                $this->fail('socket pair or TCP loopback listener is required for metadata wire-fixture tests: ' . $errstr);
            }
            $address = stream_socket_get_name($listener, false);
            if ($address === false) {
                fclose($listener);
                $this->fail('TCP loopback listener did not expose an address');
            }
            $client = stream_socket_client('tcp://' . $address, $errno, $errstr, 1);
            if ($client === false) {
                fclose($listener);
                $this->fail('TCP loopback client connection failed for metadata wire-fixture tests: ' . $errstr);
            }
            $server = stream_socket_accept($listener, 1);
            fclose($listener);
            if ($server === false) {
                fclose($client);
                $this->fail('TCP loopback accept failed for metadata wire-fixture tests');
            }
            $pair = [$client, $server];
        }
        stream_set_blocking($pair[0], true);
        stream_set_blocking($pair[1], true);
        return $pair;
    }

    private function readSentMessage($server): array
    {
        $header = $this->readExact($server, Protocol::HEADER_SIZE);
        [$type, , $length] = Protocol::decodeHeader($header);
        $payload = $length > 0 ? $this->readExact($server, $length) : '';
        return [$type, $payload];
    }

    private function extractQuerySql(string $payload): string
    {
        if (strlen($payload) < 13) {
            $this->fail('query payload truncated');
        }
        return substr($payload, 12);
    }

    private function queueReady($server, int $txnId): void
    {
        $payload = chr(0) . "\0\0\0" . $this->uint64Le($txnId) . $this->uint64Le(0);
        $this->sendServerMessage($server, Protocol::MSG_READY, $payload);
    }

    private function queueCommandComplete($server, int $rows, string $tag): void
    {
        $payload = chr(0) . "\0\0\0" . $this->uint64Le($rows) . $this->uint64Le(0) . $tag . "\0";
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
            $payload .= pack('V', 0); // tableOid
            $payload .= pack('v', $index + 1); // columnIndex
            $payload .= pack('V', TypeDecoder::OID_TEXT);
            $payload .= pack('v', 0); // typeSize
            $payload .= pack('V', 0); // typeModifier
            $payload .= chr(TypeDecoder::FORMAT_TEXT);
            $payload .= chr(1); // nullable
            $payload .= "\0\0"; // reserved
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
