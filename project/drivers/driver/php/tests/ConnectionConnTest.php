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
use ScratchBird\CircuitBreaker;
use ScratchBird\PDO\Config;
use ScratchBird\PDO\Connection;
use ScratchBird\PDO\Protocol;
use ScratchBird\PDO\ScratchBirdAuthException;
use ScratchBird\PDO\ScratchBirdConnectionException;
use ScratchBird\PDO\ScratchBirdNotSupportedException;
use ScratchBird\KeepaliveManager;
use ScratchBird\LeakDetector;
use ScratchBird\TelemetryCollector;

final class ConnectionConnTest extends TestCase
{
    private const MCP_MSG_CONNECT_RESPONSE = 0x02;
    private const MCP_MSG_AUTH_CHALLENGE = 0x12;
    private const MCP_MSG_AUTH_RESPONSE = 0x11;
    private const MCP_MSG_STATUS_RESPONSE = 0x64;
    private const MCP_MSG_HELLO = 0x65;
    private const MCP_MSG_AUTH_START = 0x66;
    private const MCP_MSG_AUTH_CONTINUE = 0x67;
    private const MCP_MSG_DB_CONNECT = 0x69;

    public function testBinaryTransferFalseDoesNotThrowNotSupportedDuringConnect(): void
    {
        $conn = $this->newConnectionWithoutConstructor(
            Config::fromDsn('scratchbird://user:pass@127.0.0.1:1/mydb?connect_timeout=1&binary_transfer=false')
        );

        try {
            $this->invokeConnect($conn);
            $this->fail('Expected connect to fail without a running server');
        } catch (ScratchBirdNotSupportedException $ex) {
            $this->fail('binary_transfer=false should not be rejected at connect validation');
        } catch (ScratchBirdConnectionException $ex) {
            $this->assertNotSame('0A000', $ex->sqlState);
        }
    }

    public function testCompressionZstdDoesNotThrowNotSupportedDuringConnect(): void
    {
        $conn = $this->newConnectionWithoutConstructor(
            Config::fromDsn('scratchbird://user:pass@127.0.0.1:1/mydb?connect_timeout=1&compression=zstd')
        );

        try {
            $this->invokeConnect($conn);
            $this->fail('Expected connect to fail without a running server');
        } catch (ScratchBirdNotSupportedException $ex) {
            $this->fail('compression=zstd should not be rejected at connect validation');
        } catch (ScratchBirdConnectionException $ex) {
            $this->assertNotSame('0A000', $ex->sqlState);
        }
    }

    public function testBuildStartupFeaturesIncludesStreamingWhenBinaryTransferEnabled(): void
    {
        $cfg = new Config();
        $cfg->binaryTransfer = true;
        $cfg->compression = 'off';
        $conn = $this->newConnectionWithoutConstructor($cfg);
        $features = $this->invokeBuildStartupFeatures($conn);
        $this->assertSame(Protocol::FEATURE_STREAMING, $features);
    }

    public function testBuildStartupFeaturesIncludesCompressionWhenConfigured(): void
    {
        $cfg = new Config();
        $cfg->binaryTransfer = false;
        $cfg->compression = 'zstd';
        $conn = $this->newConnectionWithoutConstructor($cfg);
        $features = $this->invokeBuildStartupFeatures($conn);
        $this->assertSame(Protocol::FEATURE_COMPRESSION, $features);
    }

    public function testBuildStartupFeaturesIncludesCompressionAndStreamingTogether(): void
    {
        $cfg = new Config();
        $cfg->binaryTransfer = true;
        $cfg->compression = 'zstd';
        $conn = $this->newConnectionWithoutConstructor($cfg);
        $features = $this->invokeBuildStartupFeatures($conn);
        $this->assertSame(Protocol::FEATURE_COMPRESSION | Protocol::FEATURE_STREAMING, $features);
    }

    public function testApplyTlsAllowsSslModeDisableWithoutHandshake(): void
    {
        [$client, $server] = $this->newSocketPair();
        $cfg = Config::fromDsn('scratchbird://user:pass@localhost:3092/mydb?sslmode=disable');
        $conn = $this->newConnectionWithSocket($cfg, $client);

        try {
            $this->invokeApplyTls($conn);
            fwrite($client, "PING");
            $this->assertSame("PING", $this->readExact($server, 4));
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testCloseClearsAbandonedSessionStateForFreshReconnectBoundaries(): void
    {
        [$client, $server] = $this->newSocketPair();
        $cfg = new Config();
        $conn = $this->newConnectionWithSocket($cfg, $client);
        $this->setPrivate($conn, 'attachmentId', str_repeat("\x01", 16));
        $this->setPrivate($conn, 'txnId', 42);
        $this->setPrivate($conn, 'inTransaction', true);
        $this->setPrivate($conn, 'sequence', 9);
        $this->setPrivate($conn, 'lastQuerySequence', 7);
        $this->setPrivate($conn, 'lastMaxRows', 88);
        $this->setPrivate($conn, 'parameters', ['attachment_id' => 'stale', 'current_txn_id' => '42']);
        $this->setPrivate($conn, 'lastPlan', ['format' => 1]);
        $this->setPrivate($conn, 'lastSblr', ['hash' => 5]);

        try {
            $conn->close();

            $this->assertFalse($conn->inTransaction());
            $this->assertNull($conn->lastPlan());
            $this->assertNull($conn->lastSblr());
            $this->assertSame(str_repeat("\0", 16), $this->getPrivate($conn, 'attachmentId'));
            $this->assertSame(0, $this->getPrivate($conn, 'txnId'));
            $this->assertSame(0, $this->getPrivate($conn, 'sequence'));
            $this->assertSame(0, $this->getPrivate($conn, 'lastQuerySequence'));
            $this->assertSame(0, $this->getPrivate($conn, 'lastMaxRows'));
            $this->assertSame([], $this->getPrivate($conn, 'parameters'));
        } finally {
            if (is_resource($client)) {
                fclose($client);
            }
            if (is_resource($server)) {
                fclose($server);
            }
        }
    }

    public function testPerformManagerConnectSuccessFastPath(): void
    {
        [$client, $server] = $this->newSocketPair();
        $cfg = Config::fromDsn('scratchbird://user:pass@localhost:3092/mydb?front_door_mode=manager_proxy&manager_auth_token=token');
        $conn = $this->newConnectionWithSocket($cfg, $client);

        try {
            $this->queueManagerFrame($server, self::MCP_MSG_STATUS_RESPONSE, '');
            $this->queueManagerFrame($server, self::MCP_MSG_AUTH_RESPONSE, $this->managerAuthPayloadSuccess());
            $this->queueManagerFrame($server, self::MCP_MSG_CONNECT_RESPONSE, $this->managerConnectPayloadSuccess());

            $this->invokePerformManagerConnect($conn);

            $types = [
                $this->readManagerFrameType($server),
                $this->readManagerFrameType($server),
                $this->readManagerFrameType($server),
            ];
            $this->assertSame(
                [self::MCP_MSG_HELLO, self::MCP_MSG_AUTH_START, self::MCP_MSG_DB_CONNECT],
                $types
            );
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testPerformManagerConnectSuccessChallengePath(): void
    {
        [$client, $server] = $this->newSocketPair();
        $cfg = Config::fromDsn('scratchbird://user:pass@localhost:3092/mydb?front_door_mode=manager_proxy&manager_auth_token=token');
        $cfg->managerAuthFastPath = false;
        $conn = $this->newConnectionWithSocket($cfg, $client);

        try {
            $this->queueManagerFrame($server, self::MCP_MSG_STATUS_RESPONSE, '');
            $this->queueManagerFrame($server, self::MCP_MSG_AUTH_CHALLENGE, '');
            $this->queueManagerFrame($server, self::MCP_MSG_AUTH_RESPONSE, $this->managerAuthPayloadSuccess());
            $this->queueManagerFrame($server, self::MCP_MSG_CONNECT_RESPONSE, $this->managerConnectPayloadSuccess());

            $this->invokePerformManagerConnect($conn);

            $types = [
                $this->readManagerFrameType($server),
                $this->readManagerFrameType($server),
                $this->readManagerFrameType($server),
                $this->readManagerFrameType($server),
            ];
            $this->assertSame(
                [self::MCP_MSG_HELLO, self::MCP_MSG_AUTH_START, self::MCP_MSG_AUTH_CONTINUE, self::MCP_MSG_DB_CONNECT],
                $types
            );
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testPerformManagerConnectMapsAuthFailureToTypedException(): void
    {
        [$client, $server] = $this->newSocketPair();
        $cfg = Config::fromDsn('scratchbird://user:pass@localhost:3092/mydb?front_door_mode=manager_proxy&manager_auth_token=token');
        $conn = $this->newConnectionWithSocket($cfg, $client);

        try {
            $this->queueManagerFrame($server, self::MCP_MSG_STATUS_RESPONSE, '');
            $this->queueManagerFrame($server, self::MCP_MSG_AUTH_RESPONSE, $this->managerAuthPayloadFailure('bad token'));

            try {
                $this->invokePerformManagerConnect($conn);
                $this->fail('Expected manager auth failure to throw');
            } catch (ScratchBirdAuthException $ex) {
                $this->assertStringContainsString('bad token', $ex->getMessage());
                $this->assertSame('28000', $ex->sqlState);
            }
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testHandshakeStartupIncludesAuthPluginAndPinningParams(): void
    {
        [$client, $server] = $this->newSocketPair();
        $cfg = Config::fromDsn(
            'scratchbird://user:pass@localhost:3092/mydb'
            . '?connect_client_flags=257'
            . '&auth_method_id=scratchbird.auth.proxy_assertion'
            . '&auth_method_payload=opaque'
            . '&auth_payload_json=%7B%22subject%22%3A%22alice%22%7D'
            . '&auth_payload_b64=YWJj'
            . '&auth_provider_profile=corp_primary'
            . '&auth_required_methods=SCRAM_SHA_256%2CTOKEN'
            . '&auth_forbidden_methods=MD5'
            . '&auth_require_channel_binding=true'
            . '&workload_identity_token=jwt-token'
            . '&proxy_principal_assertion=signed-assertion'
        );
        $conn = $this->newConnectionWithSocket($cfg, $client);

        try {
            $attachment = str_repeat("\0", 16);
            $authOkPayload = chr(Protocol::AUTH_OK) . "\0\0\0";
            $readyPayload = chr(0) . "\0\0\0" . pack('V4', 0, 0, 0, 0);
            $this->writeExact($server, Protocol::encodeMessage(Protocol::MSG_AUTH_REQUEST, $authOkPayload, 0, 1, $attachment, 0));
            $this->writeExact($server, Protocol::encodeMessage(Protocol::MSG_READY, $readyPayload, 0, 2, $attachment, 0));

            $this->invokeHandshake($conn);

            [$startupType, $startupPayload] = $this->readProtocolFrame($server);
            $this->assertSame(Protocol::MSG_STARTUP, $startupType);
            $params = $this->parseStartupParams($startupPayload);
            $this->assertSame('257', $params['client_flags'] ?? null);
            $this->assertSame('scratchbird.auth.proxy_assertion', $params['auth_method_id'] ?? null);
            $this->assertSame('opaque', $params['auth_method_payload'] ?? null);
            $this->assertSame('{"subject":"alice"}', $params['auth_payload_json'] ?? null);
            $this->assertSame('YWJj', $params['auth_payload_b64'] ?? null);
            $this->assertSame('corp_primary', $params['auth_provider_profile'] ?? null);
            $this->assertSame('SCRAM_SHA_256,TOKEN', $params['auth_required_methods'] ?? null);
            $this->assertSame('MD5', $params['auth_forbidden_methods'] ?? null);
            $this->assertSame('1', $params['auth_require_channel_binding'] ?? null);
            $this->assertSame('jwt-token', $params['workload_identity_token'] ?? null);
            $this->assertSame('signed-assertion', $params['proxy_principal_assertion'] ?? null);
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testHandshakeRejectsInvalidAuthMethodNamespace(): void
    {
        $cfg = Config::fromDsn('scratchbird://user:pass@localhost:3092/mydb?auth_method_id=invalid.namespace');
        $conn = $this->newConnectionWithoutConstructor($cfg);

        $this->expectException(ScratchBirdAuthException::class);
        $this->expectExceptionMessage('invalid auth_method_id namespace');
        $this->invokeHandshake($conn);
    }

    public function testProbeDirectAuthSurfaceReportsScramSha512(): void
    {
        [$client, $server] = $this->newSocketPair();
        $cfg = Config::fromDsn('scratchbird://user:pass@localhost:3092/mydb?sslmode=disable');
        $conn = $this->newConnectionWithSocket($cfg, $client);

        try {
            $attachment = str_repeat("\0", 16);
            $authPayload = chr(Protocol::AUTH_SCRAM_SHA512) . "\0\0\0";
            $this->writeExact($server, Protocol::encodeMessage(Protocol::MSG_AUTH_REQUEST, $authPayload, 0, 1, $attachment, 0));

            $probe = $this->invokeProbeDirectAuthSurface($conn);

            [$startupType, ] = $this->readProtocolFrame($server);
            $this->assertSame(Protocol::MSG_STARTUP, $startupType);
            $this->assertTrue($probe['reachable']);
            $this->assertSame('direct', $probe['front_door_mode']);
            $this->assertSame('SCRAM_SHA_512', $probe['required_method']);
            $this->assertSame('scratchbird.auth.scram_sha_512', $probe['required_plugin_method_id']);
            $this->assertCount(1, $probe['admitted_methods']);
            $this->assertSame('SCRAM_SHA_512', $probe['admitted_methods'][0]['method_name']);
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testProbeManagerAuthSurfaceReportsToken(): void
    {
        [$client, $server] = $this->newSocketPair();
        $cfg = Config::fromDsn('scratchbird://user:pass@localhost:3092/mydb?front_door_mode=manager_proxy&sslmode=disable');
        $conn = $this->newConnectionWithSocket($cfg, $client);

        try {
            $this->queueManagerFrame($server, self::MCP_MSG_STATUS_RESPONSE, '');
            $this->queueManagerFrame($server, self::MCP_MSG_AUTH_CHALLENGE, '');

            $probe = $this->invokeProbeManagerAuthSurface($conn);

            $types = [
                $this->readManagerFrameType($server),
                $this->readManagerFrameType($server),
            ];
            $this->assertSame([self::MCP_MSG_HELLO, self::MCP_MSG_AUTH_START], $types);
            $this->assertTrue($probe['reachable']);
            $this->assertSame('manager_proxy', $probe['front_door_mode']);
            $this->assertSame('TOKEN', $probe['required_method']);
            $this->assertSame('scratchbird.auth.authkey_token', $probe['required_plugin_method_id']);
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testHandshakeSupportsScramSha512AndResolvedAuthContext(): void
    {
        [$client, $server] = $this->newSocketPair();
        $cfg = Config::fromDsn('scratchbird://alice:secret@localhost:3092/mydb?sslmode=disable');
        $conn = $this->newConnectionWithSocket($cfg, $client);

        try {
            $attachment = random_bytes(16);
            $authRequest = chr(Protocol::AUTH_SCRAM_SHA512) . "\0\0\0";
            $authOk = str_repeat("\0", 16) . pack('V', 0);
            $readyPayload = chr(0) . "\0\0\0" . pack('V4', 0, 0, 0, 0);
            $this->writeExact($server, Protocol::encodeMessage(Protocol::MSG_AUTH_REQUEST, $authRequest, 0, 1, $attachment, 0));
            $this->writeExact($server, Protocol::encodeMessage(Protocol::MSG_AUTH_OK, $authOk, 0, 2, $attachment, 0));
            $this->writeExact($server, Protocol::encodeMessage(Protocol::MSG_READY, $readyPayload, 0, 3, $attachment, 0));

            $this->invokeHandshake($conn);

            [$startupType] = $this->readProtocolFrame($server); // startup
            $this->assertSame(Protocol::MSG_STARTUP, $startupType);
            [, $firstResponse] = $this->readProtocolFrame($server);
            $this->assertStringStartsWith('n,,n=alice,r=', $firstResponse);
            $context = $conn->getResolvedAuthContext();
            $this->assertSame('SCRAM_SHA_512', $context['resolved_auth_method']);
            $this->assertSame('scratchbird.auth.scram_sha_512', $context['resolved_auth_plugin_id']);
            $this->assertTrue($context['attached']);
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testHandshakeSupportsTokenAuthAndResolvedAuthContext(): void
    {
        [$client, $server] = $this->newSocketPair();
        $cfg = Config::fromDsn('scratchbird://alice:secret@localhost:3092/mydb?sslmode=disable&auth_token=bearer-token');
        $conn = $this->newConnectionWithSocket($cfg, $client);

        try {
            $attachment = random_bytes(16);
            $authRequest = chr(Protocol::AUTH_TOKEN) . "\0\0\0";
            $authOk = str_repeat("\0", 16) . pack('V', 0);
            $readyPayload = chr(0) . "\0\0\0" . pack('V4', 0, 0, 0, 0);
            $this->writeExact($server, Protocol::encodeMessage(Protocol::MSG_AUTH_REQUEST, $authRequest, 0, 1, $attachment, 0));
            $this->writeExact($server, Protocol::encodeMessage(Protocol::MSG_AUTH_OK, $authOk, 0, 2, $attachment, 0));
            $this->writeExact($server, Protocol::encodeMessage(Protocol::MSG_READY, $readyPayload, 0, 3, $attachment, 0));

            $this->invokeHandshake($conn);

            [$startupType] = $this->readProtocolFrame($server); // startup
            $this->assertSame(Protocol::MSG_STARTUP, $startupType);
            [, $authResponse] = $this->readProtocolFrame($server);
            $this->assertSame('bearer-token', $authResponse);
            $context = $conn->getResolvedAuthContext();
            $this->assertSame('TOKEN', $context['resolved_auth_method']);
            $this->assertSame('scratchbird.auth.authkey_token', $context['resolved_auth_plugin_id']);
            $this->assertTrue($context['attached']);
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    public function testHandshakeFailClosesPeerAndPreservesResolvedAuthContext(): void
    {
        [$client, $server] = $this->newSocketPair();
        $cfg = Config::fromDsn('scratchbird://alice:secret@localhost:3092/mydb?sslmode=disable');
        $conn = $this->newConnectionWithSocket($cfg, $client);

        try {
            $attachment = random_bytes(16);
            $authRequest = chr(Protocol::AUTH_PEER) . "\0\0\0";
            $this->writeExact($server, Protocol::encodeMessage(Protocol::MSG_AUTH_REQUEST, $authRequest, 0, 1, $attachment, 0));

            try {
                $this->invokeHandshake($conn);
                $this->fail('Expected PEER auth to fail closed');
            } catch (ScratchBirdNotSupportedException $ex) {
                $this->assertSame('0A000', $ex->sqlState);
            }

            $context = $conn->getResolvedAuthContext();
            $this->assertSame('PEER', $context['resolved_auth_method']);
            $this->assertSame('scratchbird.auth.peer_uid', $context['resolved_auth_plugin_id']);
            $this->assertFalse($context['attached']);
        } finally {
            fclose($client);
            fclose($server);
        }
    }

    private function newConnectionWithoutConstructor(Config $cfg): Connection
    {
        $class = new ReflectionClass(Connection::class);
        /** @var Connection $conn */
        $conn = $class->newInstanceWithoutConstructor();
        $prop = $class->getProperty('config');
        $prop->setAccessible(true);
        $prop->setValue($conn, $cfg);
        $this->setPrivate($conn, 'connectionId', 'test-conn');
        $this->setPrivate($conn, 'circuitBreaker', new CircuitBreaker());
        $this->setPrivate($conn, 'telemetry', new TelemetryCollector());
        $this->setPrivate($conn, 'keepaliveManager', new KeepaliveManager());
        $this->setPrivate($conn, 'keepaliveTracker', null);
        $this->setPrivate($conn, 'leakDetector', new LeakDetector());
        $this->setPrivate($conn, 'leakGuard', null);
        return $conn;
    }

    private function newConnectionWithSocket(Config $cfg, $socket): Connection
    {
        $class = new ReflectionClass(Connection::class);
        /** @var Connection $conn */
        $conn = $class->newInstanceWithoutConstructor();
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
        $this->setPrivate($conn, 'connectionId', 'test-conn');
        $this->setPrivate($conn, 'circuitBreaker', new CircuitBreaker());
        $this->setPrivate($conn, 'telemetry', new TelemetryCollector());
        $this->setPrivate($conn, 'keepaliveManager', new KeepaliveManager());
        $this->setPrivate($conn, 'keepaliveTracker', null);
        $this->setPrivate($conn, 'leakDetector', new LeakDetector());
        $this->setPrivate($conn, 'leakGuard', null);
        return $conn;
    }

    private function newSocketPair(): array
    {
        $pair = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
        if ($pair === false) {
            $listener = stream_socket_server('tcp://127.0.0.1:0', $errno, $errstr);
            if ($listener === false) {
                $this->fail('socket pair or TCP loopback listener is required for manager wire-fixture tests: ' . $errstr);
            }
            $address = stream_socket_get_name($listener, false);
            if ($address === false) {
                fclose($listener);
                $this->fail('TCP loopback listener did not expose an address');
            }
            $client = stream_socket_client('tcp://' . $address, $errno, $errstr, 1);
            if ($client === false) {
                fclose($listener);
                $this->fail('TCP loopback client connection failed for manager wire-fixture tests: ' . $errstr);
            }
            $server = stream_socket_accept($listener, 1);
            fclose($listener);
            if ($server === false) {
                fclose($client);
                $this->fail('TCP loopback accept failed for manager wire-fixture tests');
            }
            $pair = [$client, $server];
        }
        stream_set_blocking($pair[0], true);
        stream_set_blocking($pair[1], true);
        return $pair;
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

    private function invokeConnect(Connection $conn): void
    {
        $class = new ReflectionClass(Connection::class);
        $method = $class->getMethod('connect');
        $method->setAccessible(true);
        $method->invoke($conn);
    }

    private function invokePerformManagerConnect(Connection $conn): void
    {
        $class = new ReflectionClass(Connection::class);
        $method = $class->getMethod('performManagerConnect');
        $method->setAccessible(true);
        $method->invoke($conn);
    }

    private function invokeBuildStartupFeatures(Connection $conn): int
    {
        $class = new ReflectionClass(Connection::class);
        $method = $class->getMethod('buildStartupFeatures');
        $method->setAccessible(true);
        /** @var int $features */
        $features = $method->invoke($conn);
        return $features;
    }

    private function invokeApplyTls(Connection $conn): void
    {
        $class = new ReflectionClass(Connection::class);
        $method = $class->getMethod('applyTls');
        $method->setAccessible(true);
        $method->invoke($conn);
    }

    private function invokeHandshake(Connection $conn): void
    {
        $class = new ReflectionClass(Connection::class);
        $method = $class->getMethod('handshake');
        $method->setAccessible(true);
        $method->invoke($conn);
    }

    private function invokeProbeDirectAuthSurface(Connection $conn): array
    {
        $class = new ReflectionClass(Connection::class);
        $method = $class->getMethod('probeDirectAuthSurface');
        $method->setAccessible(true);
        /** @var array $result */
        $result = $method->invoke($conn);
        return $result;
    }

    private function invokeProbeManagerAuthSurface(Connection $conn): array
    {
        $class = new ReflectionClass(Connection::class);
        $method = $class->getMethod('probeManagerAuthSurface');
        $method->setAccessible(true);
        /** @var array $result */
        $result = $method->invoke($conn);
        return $result;
    }

    private function queueManagerFrame($server, int $type, string $payload): void
    {
        $frame = pack('V', 0x42444253)
            . pack('v', 0x0101)
            . chr($type)
            . chr(0)
            . pack('V', strlen($payload))
            . $payload;
        $this->writeExact($server, $frame);
    }

    private function readManagerFrameType($server): int
    {
        $header = $this->readExact($server, 12);
        $magic = unpack('V', substr($header, 0, 4))[1];
        $this->assertSame(0x42444253, $magic);
        $version = unpack('v', substr($header, 4, 2))[1];
        $this->assertSame(0x0101, $version);
        $type = ord($header[6]);
        $length = unpack('V', substr($header, 8, 4))[1];
        if ($length > 0) {
            $this->readExact($server, $length);
        }
        return $type;
    }

    private function readProtocolFrame($stream): array
    {
        $header = $this->readExact($stream, Protocol::HEADER_SIZE);
        [$type, , $length] = Protocol::decodeHeader($header);
        $payload = $length > 0 ? $this->readExact($stream, $length) : '';
        return [$type, $payload];
    }

    private function parseStartupParams(string $payload): array
    {
        $offset = 84;
        if (strlen($payload) < $offset) {
            $this->fail('startup payload truncated before params');
        }
        $params = [];
        while ($offset + 4 <= strlen($payload)) {
            $keyLen = $this->readUInt32Le(substr($payload, $offset, 4));
            $offset += 4;
            if ($keyLen === 0) {
                break;
            }
            if ($offset + $keyLen + 2 + 4 > strlen($payload)) {
                $this->fail('startup parameter key truncated');
            }
            $key = substr($payload, $offset, $keyLen);
            $offset += $keyLen + 2;
            $valueLen = $this->readUInt32Le(substr($payload, $offset, 4));
            $offset += 4;
            if ($offset + $valueLen > strlen($payload)) {
                $this->fail('startup parameter value truncated');
            }
            $params[$key] = substr($payload, $offset, $valueLen);
            $offset += $valueLen;
        }
        return $params;
    }

    private function readUInt32Le(string $bytes): int
    {
        $parts = unpack('Vvalue', $bytes);
        return (int) $parts['value'];
    }

    private function managerAuthPayloadSuccess(): string
    {
        return chr(0) . pack('V', 0) . str_repeat("\0", 256);
    }

    private function managerAuthPayloadFailure(string $message): string
    {
        return chr(1) . pack('V', 0) . str_pad($message, 256, "\0");
    }

    private function managerConnectPayloadSuccess(): string
    {
        return chr(0) . str_repeat("\0", 1 + 2 + 2 + 16 + 64 + 32 - 1);
    }

    private function writeExact($stream, string $data): void
    {
        $offset = 0;
        $length = strlen($data);
        while ($offset < $length) {
            $written = fwrite($stream, substr($data, $offset));
            if ($written === false || $written === 0) {
                $this->fail('Failed writing fixture frame');
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
                $this->fail('Failed reading fixture frame');
            }
            $data .= $chunk;
        }
        return $data;
    }
}
