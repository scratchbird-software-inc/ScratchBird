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
use ScratchBird\PDO\Protocol;

final class ProtocolConnAuthTest extends TestCase
{
    public function testBuildStartupPayloadCarriesFeatureMaskAndParameters(): void
    {
        $payload = Protocol::buildStartupPayload(
            Protocol::FEATURE_STREAMING,
            ['database' => 'demo', 'user' => 'app']
        );
        $this->assertSame(Protocol::VERSION_MAJOR, ord($payload[0]));
        $this->assertSame(Protocol::VERSION_MINOR, ord($payload[1]));
        $this->assertSame(Protocol::FEATURE_STREAMING, $this->readUInt64Le(substr($payload, 8, 8)));
        $this->assertSame(
            ['database' => 'demo', 'user' => 'app'],
            $this->parseStartupParams($payload)
        );
    }

    public function testParseAuthRequestScramPayload(): void
    {
        $data = 'n,,n=app,r=clientnonce';
        $payload = chr(Protocol::AUTH_SCRAM_SHA256) . "\0\0\0" . $data;
        [$method, $parsed] = Protocol::parseAuthRequest($payload);
        $this->assertSame(Protocol::AUTH_SCRAM_SHA256, $method);
        $this->assertSame($data, $parsed);
    }

    public function testParseAuthContinueScramPayload(): void
    {
        $data = 'r=nonce,s=salt,i=4096';
        $payload = chr(Protocol::AUTH_SCRAM_SHA256) . chr(1) . "\0\0" . pack('V', strlen($data)) . $data;
        [$method, $stage, $parsed] = Protocol::parseAuthContinue($payload);
        $this->assertSame(Protocol::AUTH_SCRAM_SHA256, $method);
        $this->assertSame(1, $stage);
        $this->assertSame($data, $parsed);
    }

    public function testParseAuthContinueScramSha512Payload(): void
    {
        $data = 'r=nonce,s=salt,i=4096';
        $payload = chr(Protocol::AUTH_SCRAM_SHA512) . chr(1) . "\0\0" . pack('V', strlen($data)) . $data;
        [$method, $stage, $parsed] = Protocol::parseAuthContinue($payload);
        $this->assertSame(Protocol::AUTH_SCRAM_SHA512, $method);
        $this->assertSame(1, $stage);
        $this->assertSame($data, $parsed);
    }

    public function testParseAuthOkPayload(): void
    {
        $sessionId = random_bytes(16);
        $info = 'v=server-signature';
        $payload = $sessionId . pack('V', strlen($info)) . $info;
        [$parsedSessionId, $parsedInfo] = Protocol::parseAuthOk($payload);
        $this->assertSame($sessionId, $parsedSessionId);
        $this->assertSame($info, $parsedInfo);
    }

    /**
     * @return array<string, string>
     */
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

    private function readUInt64Le(string $bytes): int
    {
        $parts = unpack('Vlow/Vhigh', $bytes);
        return (int) ($parts['low'] + ($parts['high'] << 32));
    }
}
