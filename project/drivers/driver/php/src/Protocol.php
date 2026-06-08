<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

namespace ScratchBird\PDO;

final class Protocol
{
    private const MAGIC_BYTES = "SBWP";
    public const VERSION_MAJOR = 1;
    public const VERSION_MINOR = 1;
    public const VERSION = (self::VERSION_MAJOR << 8) | self::VERSION_MINOR;
    public const HEADER_SIZE = 40;
    public const MAX_MESSAGE_SIZE = 1073741824;

    public const MSG_STARTUP = 0x01;
    public const MSG_AUTH_RESPONSE = 0x02;
    public const MSG_QUERY = 0x03;
    public const MSG_PARSE = 0x04;
    public const MSG_BIND = 0x05;
    public const MSG_DESCRIBE = 0x06;
    public const MSG_EXECUTE = 0x07;
    public const MSG_CLOSE = 0x08;
    public const MSG_SYNC = 0x09;
    public const MSG_FLUSH = 0x0A;
    public const MSG_CANCEL = 0x0B;
    public const MSG_TERMINATE = 0x0C;
    public const MSG_COPY_DATA = 0x0D;
    public const MSG_COPY_DONE = 0x0E;
    public const MSG_COPY_FAIL = 0x0F;
    public const MSG_SBLR_EXECUTE = 0x10;
    public const MSG_SUBSCRIBE = 0x11;
    public const MSG_UNSUBSCRIBE = 0x12;
    public const MSG_FEDERATED_QUERY = 0x13;
    public const MSG_STREAM_CONTROL = 0x14;
    public const MSG_TXN_BEGIN = 0x15;
    public const MSG_TXN_COMMIT = 0x16;
    public const MSG_TXN_ROLLBACK = 0x17;
    public const MSG_TXN_SAVEPOINT = 0x18;
    public const MSG_TXN_RELEASE = 0x19;
    public const MSG_TXN_ROLLBACK_TO = 0x1A;
    public const MSG_PING = 0x1B;
    public const MSG_SET_OPTION = 0x1C;
    public const MSG_CLUSTER_AUTH = 0x1D;
    public const MSG_ATTACH_CREATE = 0x1E;
    public const MSG_ATTACH_DETACH = 0x1F;
    public const MSG_ATTACH_LIST = 0x20;

    public const MSG_AUTH_REQUEST = 0x40;
    public const MSG_AUTH_OK = 0x41;
    public const MSG_AUTH_CONTINUE = 0x42;
    public const MSG_READY = 0x43;
    public const MSG_ROW_DESCRIPTION = 0x44;
    public const MSG_DATA_ROW = 0x45;
    public const MSG_COMMAND_COMPLETE = 0x46;
    public const MSG_EMPTY_QUERY = 0x47;
    public const MSG_ERROR = 0x48;
    public const MSG_NOTICE = 0x49;
    public const MSG_PARSE_COMPLETE = 0x4A;
    public const MSG_BIND_COMPLETE = 0x4B;
    public const MSG_CLOSE_COMPLETE = 0x4C;
    public const MSG_PORTAL_SUSPENDED = 0x4D;
    public const MSG_NO_DATA = 0x4E;
    public const MSG_PARAMETER_STATUS = 0x4F;
    public const MSG_PARAMETER_DESCRIPTION = 0x50;
    public const MSG_COPY_IN_RESPONSE = 0x51;
    public const MSG_COPY_OUT_RESPONSE = 0x52;
    public const MSG_COPY_BOTH_RESPONSE = 0x53;
    public const MSG_NOTIFICATION = 0x54;
    public const MSG_FUNCTION_RESULT = 0x55;
    public const MSG_NEGOTIATE_VERSION = 0x56;
    public const MSG_SBLR_COMPILED = 0x57;
    public const MSG_QUERY_PLAN = 0x58;
    public const MSG_STREAM_READY = 0x59;
    public const MSG_STREAM_DATA = 0x5A;
    public const MSG_STREAM_END = 0x5B;
    public const MSG_TXN_STATUS = 0x5C;
    public const MSG_PONG = 0x5D;
    public const MSG_CLUSTER_AUTH_OK = 0x5E;
    public const MSG_FEDERATED_RESULT = 0x5F;
    public const MSG_HEARTBEAT = 0x80;
    public const MSG_EXTENSION = 0x81;

    public const AUTH_OK = 0;
    public const AUTH_PASSWORD = 1;
    public const AUTH_MD5 = 2;
    public const AUTH_SCRAM_SHA256 = 3;
    public const AUTH_SCRAM_SHA512 = 4;
    public const AUTH_TOKEN = 5;
    public const AUTH_PEER = 6;
    public const AUTH_REATTACH = 7;

    public const MSG_FLAG_COMPRESSED = 0x01;
    public const MSG_FLAG_CONTINUED = 0x02;
    public const MSG_FLAG_FINAL = 0x04;
    public const MSG_FLAG_URGENT = 0x08;
    public const MSG_FLAG_ENCRYPTED = 0x10;
    public const MSG_FLAG_CHECKSUM = 0x20;

    public const FEATURE_COMPRESSION = 1;
    public const FEATURE_STREAMING = 2;
    public const FEATURE_SBLR = 4;
    public const FEATURE_FEDERATION = 8;
    public const FEATURE_NOTIFICATIONS = 16;
    public const FEATURE_QUERY_PLAN = 32;
    public const FEATURE_BATCH = 64;
    public const FEATURE_PIPELINE = 128;
    public const FEATURE_BINARY_COPY = 256;
    public const FEATURE_SAVEPOINTS = 512;
    public const FEATURE_2PC = 1024;
    public const FEATURE_CHECKSUMS = 2048;

    public const QUERY_FLAG_DESCRIBE_ONLY = 0x01;
    public const QUERY_FLAG_NO_PORTAL = 0x02;
    public const QUERY_FLAG_BINARY_RESULT = 0x04;
    public const QUERY_FLAG_INCLUDE_PLAN = 0x08;
    public const QUERY_FLAG_RETURN_SBLR = 0x10;
    public const QUERY_FLAG_NO_CACHE = 0x20;

    public const ISOLATION_READ_UNCOMMITTED = 0;
    public const ISOLATION_READ_COMMITTED = 1;
    public const ISOLATION_REPEATABLE_READ = 2;
    public const ISOLATION_SERIALIZABLE = 3;

    public const READ_COMMITTED_MODE_DEFAULT = 0;
    public const READ_COMMITTED_MODE_READ_CONSISTENCY = 1;
    public const READ_COMMITTED_MODE_RECORD_VERSION = 2;
    public const READ_COMMITTED_MODE_NO_RECORD_VERSION = 3;

    public const TXN_FLAG_HAS_ISOLATION = 0x0001;
    public const TXN_FLAG_HAS_ACCESS = 0x0002;
    public const TXN_FLAG_HAS_DEFERRABLE = 0x0004;
    public const TXN_FLAG_HAS_WAIT = 0x0008;
    public const TXN_FLAG_HAS_TIMEOUT = 0x0010;
    public const TXN_FLAG_HAS_AUTOCOMMIT = 0x0020;
    public const TXN_FLAG_HAS_READ_COMMITTED_MODE = 0x0100;

    public const STREAM_START = 0;
    public const STREAM_PAUSE = 1;
    public const STREAM_RESUME = 2;
    public const STREAM_CANCEL = 3;
    public const STREAM_ACK = 4;

    public const SUB_TYPE_CHANNEL = 0;
    public const SUB_TYPE_TABLE = 1;
    public const SUB_TYPE_QUERY = 2;
    public const SUB_TYPE_EVENT = 3;

    public static function encodeMessage(int $type, string $payload, int $flags, int $sequence, string $attachmentId, int $txnId): string
    {
        $header = self::MAGIC_BYTES
            . chr(self::VERSION_MAJOR)
            . chr(self::VERSION_MINOR)
            . chr($type)
            . chr($flags)
            . self::writeUInt32LE(strlen($payload))
            . self::writeUInt32LE($sequence)
            . self::padBytes($attachmentId, 16)
            . self::writeUInt64LE($txnId);
        return $header . $payload;
    }

    public static function decodeHeader(string $header): array
    {
        if (strlen($header) !== self::HEADER_SIZE) {
            throw new \RuntimeException('Invalid header length');
        }
        if (substr($header, 0, 4) !== self::MAGIC_BYTES) {
            throw new \RuntimeException('Invalid protocol magic');
        }
        $major = ord($header[4]);
        $minor = ord($header[5]);
        if ($major !== self::VERSION_MAJOR || $minor !== self::VERSION_MINOR) {
            throw new \RuntimeException('Unsupported protocol version');
        }
        $type = ord($header[6]);
        $flags = ord($header[7]);
        $length = self::readUInt32LE(substr($header, 8, 4));
        if ($length > self::MAX_MESSAGE_SIZE) {
            throw new \RuntimeException('Payload too large');
        }
        $sequence = self::readUInt32LE(substr($header, 12, 4));
        $attachmentId = substr($header, 16, 16);
        $txnId = self::readUInt64LE(substr($header, 32, 8));
        return [$type, $flags, $length, $sequence, $attachmentId, $txnId];
    }

    public static function buildStartupPayload(int $features, array $parameters): string
    {
        $payload = chr(self::VERSION_MAJOR) . chr(self::VERSION_MINOR) . self::writeUInt16LE(0);
        $payload .= self::writeUInt64LE($features);
        $payload .= self::buildParamList($parameters);
        return $payload;
    }

    private static function buildParamList(array $parameters): string
    {
        $buffer = '';
        foreach ($parameters as $key => $value) {
            $buffer .= $key . "\0" . $value . "\0";
        }
        $buffer .= "\0";
        return $buffer;
    }

    public static function parseAuthRequest(string $payload): array
    {
        if (strlen($payload) < 4) {
            throw new \RuntimeException('Auth request truncated');
        }
        $method = ord($payload[0]);
        $data = substr($payload, 4);
        return [$method, $data];
    }

    public static function parseAuthContinue(string $payload): array
    {
        if (strlen($payload) < 8) {
            throw new \RuntimeException('Auth continue truncated');
        }
        $method = ord($payload[0]);
        $stage = ord($payload[1]);
        $dataLen = self::readUInt32LE(substr($payload, 4, 4));
        if (8 + $dataLen > strlen($payload)) {
            throw new \RuntimeException('Auth continue truncated');
        }
        $data = substr($payload, 8, $dataLen);
        return [$method, $stage, $data];
    }

    public static function parseAuthOk(string $payload): array
    {
        if (strlen($payload) < 20) {
            throw new \RuntimeException('Auth ok truncated');
        }
        $sessionId = substr($payload, 0, 16);
        $infoLen = self::readUInt32LE(substr($payload, 16, 4));
        if (20 + $infoLen > strlen($payload)) {
            throw new \RuntimeException('Auth ok truncated');
        }
        $serverInfo = substr($payload, 20, $infoLen);
        return [$sessionId, $serverInfo];
    }

    public static function buildQueryPayload(string $sql, int $flags, int $maxRows, int $timeoutMs): string
    {
        return self::writeUInt32LE($flags)
            . self::writeUInt32LE($maxRows)
            . self::writeUInt32LE($timeoutMs)
            . $sql . "\0";
    }

    public static function buildParsePayload(string $statementName, string $sql, array $paramTypes): string
    {
        $nameBytes = $statementName;
        $sqlBytes = $sql;
        $payload = self::writeUInt32LE(strlen($nameBytes)) . $nameBytes;
        $payload .= self::writeUInt32LE(strlen($sqlBytes)) . $sqlBytes;
        $payload .= self::writeUInt16LE(count($paramTypes));
        $payload .= self::writeUInt16LE(0);
        foreach ($paramTypes as $oid) {
            $payload .= self::writeUInt32LE($oid);
        }
        return $payload;
    }

    public static function buildBindPayload(string $portalName, string $statementName, array $params, array $resultFormats): string
    {
        $portalBytes = $portalName;
        $stmtBytes = $statementName;
        $payload = self::writeUInt32LE(strlen($portalBytes)) . $portalBytes;
        $payload .= self::writeUInt32LE(strlen($stmtBytes)) . $stmtBytes;

        $payload .= self::writeUInt16LE(count($params));
        foreach ($params as $param) {
            $payload .= self::writeUInt16LE($param['format']);
        }

        $payload .= self::writeUInt16LE(count($params));
        $payload .= self::writeUInt16LE(0);
        foreach ($params as $param) {
            if (!empty($param['isNull'])) {
                $payload .= self::writeInt32LE(-1);
                continue;
            }
            $data = $param['data'] ?? '';
            $payload .= self::writeInt32LE(strlen($data)) . $data;
        }

        $payload .= self::writeUInt16LE(count($resultFormats));
        foreach ($resultFormats as $fmt) {
            $payload .= self::writeUInt16LE($fmt);
        }

        return $payload;
    }

    public static function buildDescribePayload(int $describeType, string $name): string
    {
        $nameBytes = $name;
        return chr($describeType) . "\0\0\0" . self::writeUInt32LE(strlen($nameBytes)) . $nameBytes;
    }

    public static function buildExecutePayload(string $portalName, int $maxRows): string
    {
        $portalBytes = $portalName;
        return self::writeUInt32LE(strlen($portalBytes)) . $portalBytes . self::writeUInt32LE($maxRows);
    }

    public static function buildClosePayload(int $closeType, string $name): string
    {
        $nameBytes = $name;
        return chr($closeType) . "\0\0\0" . self::writeUInt32LE(strlen($nameBytes)) . $nameBytes;
    }

    public static function buildCancelPayload(int $cancelType, int $targetSequence): string
    {
        return self::writeUInt32LE($cancelType) . self::writeUInt32LE($targetSequence);
    }

    public static function buildSblrExecutePayload(int $sblrHash, ?string $sblrBytecode, array $params): string
    {
        $bytecode = $sblrBytecode ?? '';
        $payload = self::writeUInt64LE($sblrHash);
        $payload .= self::writeUInt32LE(strlen($bytecode));
        $payload .= self::writeUInt16LE(count($params));
        $payload .= self::writeUInt16LE(0);
        $payload .= $bytecode;
        foreach ($params as $param) {
            if ($param['is_null'] ?? false) {
                $payload .= self::writeInt32LE(-1);
            } else {
                $data = $param['data'] ?? '';
                $payload .= self::writeInt32LE(strlen($data));
                $payload .= $data;
            }
        }
        return $payload;
    }

    public static function buildSubscribePayload(int $subscribeType, string $channel, string $filterExpr = ''): string
    {
        $payload = chr($subscribeType) . "\0\0\0";
        $payload .= self::writeUInt32LE(strlen($channel)) . $channel;
        $payload .= self::writeUInt32LE(strlen($filterExpr)) . $filterExpr;
        return $payload;
    }

    public static function buildUnsubscribePayload(string $channel): string
    {
        return self::writeUInt32LE(strlen($channel)) . $channel;
    }

    public static function buildTxnBeginPayload(
        int $flags,
        int $conflictAction,
        int $autocommitMode,
        int $isolationLevel,
        int $accessMode,
        int $deferrable,
        int $waitMode,
        int $timeoutMs,
        int $readCommittedMode = self::READ_COMMITTED_MODE_DEFAULT
    ): string {
        $payload = self::writeUInt16LE($flags);
        $payload .= chr($conflictAction)
            . chr($autocommitMode)
            . chr($isolationLevel)
            . chr($accessMode)
            . chr($deferrable)
            . chr($waitMode);
        $payload .= self::writeUInt32LE($timeoutMs);
        if (($flags & self::TXN_FLAG_HAS_READ_COMMITTED_MODE) !== 0) {
            $payload .= chr($readCommittedMode) . "\0\0\0";
        }
        return $payload;
    }

    public static function canonicalReadCommittedModeLabel(int $mode): string
    {
        return match ($mode) {
            self::READ_COMMITTED_MODE_DEFAULT => 'READ COMMITTED',
            self::READ_COMMITTED_MODE_READ_CONSISTENCY => 'READ COMMITTED READ CONSISTENCY',
            self::READ_COMMITTED_MODE_RECORD_VERSION => 'READ COMMITTED RECORD VERSION',
            self::READ_COMMITTED_MODE_NO_RECORD_VERSION => 'READ COMMITTED NO RECORD VERSION',
            default => 'UNKNOWN(' . $mode . ')',
        };
    }

    public static function buildTxnCommitPayload(int $flags): string
    {
        return chr($flags) . "\0\0\0";
    }

    public static function buildTxnRollbackPayload(int $flags): string
    {
        return chr($flags) . "\0\0\0";
    }

    public static function buildTxnSavepointPayload(string $name): string
    {
        return self::writeUInt32LE(strlen($name)) . $name;
    }

    public static function buildTxnReleasePayload(string $name): string
    {
        return self::buildTxnSavepointPayload($name);
    }

    public static function buildTxnRollbackToPayload(string $name): string
    {
        return self::buildTxnSavepointPayload($name);
    }

    public static function buildSetOptionPayload(string $name, string $value): string
    {
        $payload = self::writeUInt32LE(strlen($name)) . $name;
        $payload .= self::writeUInt32LE(strlen($value)) . $value;
        return $payload;
    }

    public static function buildStreamControlPayload(int $controlType, int $windowSize, int $timeoutMs): string
    {
        return chr($controlType) . "\0\0\0"
            . self::writeUInt32LE($windowSize)
            . self::writeUInt32LE($timeoutMs);
    }

    public static function buildAttachCreatePayload(string $emulationMode, string $dbName): string
    {
        $payload = self::writeUInt32LE(strlen($emulationMode)) . $emulationMode;
        $payload .= self::writeUInt32LE(strlen($dbName)) . $dbName;
        return $payload;
    }

    public static function parseReady(string $payload): array
    {
        if (strlen($payload) < 20) {
            throw new \RuntimeException('Ready truncated');
        }
        $status = ord($payload[0]);
        $txnId = self::readUInt64LE(substr($payload, 4, 8));
        $visibility = self::readUInt64LE(substr($payload, 12, 8));
        return [$status, $txnId, $visibility];
    }

    public static function parseTxnStatus(string $payload): array
    {
        if (strlen($payload) < 12) {
            throw new \RuntimeException('Txn status truncated');
        }
        $status = ord($payload[0]);
        $txnId = self::readUInt64LE(substr($payload, 4, 8));
        return [$status, $txnId];
    }

    public static function parseParameterStatus(string $payload): array
    {
        if (strlen($payload) < 8) {
            throw new \RuntimeException('Parameter status truncated');
        }
        $offset = 0;
        $nameLen = self::readUInt32LE(substr($payload, $offset, 4));
        $offset += 4;
        $name = substr($payload, $offset, $nameLen);
        $offset += $nameLen;
        $valueLen = self::readUInt32LE(substr($payload, $offset, 4));
        $offset += 4;
        $value = substr($payload, $offset, $valueLen);
        return [$name, $value];
    }

    public static function parseParameterDescription(string $payload): array
    {
        if (strlen($payload) < 4) {
            throw new \RuntimeException('Parameter description truncated');
        }
        $count = self::readUInt16LE(substr($payload, 0, 2));
        $offset = 4;
        $types = [];
        for ($i = 0; $i < $count; $i++) {
            if ($offset + 4 > strlen($payload)) {
                throw new \RuntimeException('Parameter description truncated');
            }
            $types[] = self::readUInt32LE(substr($payload, $offset, 4));
            $offset += 4;
        }
        return $types;
    }

    public static function parseRowDescription(string $payload): array
    {
        if (strlen($payload) < 4) {
            throw new \RuntimeException('Row description truncated');
        }
        $offset = 0;
        $count = self::readUInt16LE(substr($payload, $offset, 2));
        $offset += 4;
        $columns = [];
        for ($i = 0; $i < $count; $i++) {
            $nameLen = self::readUInt32LE(substr($payload, $offset, 4));
            $offset += 4;
            $name = substr($payload, $offset, $nameLen);
            $offset += $nameLen;
            $tableOid = self::readUInt32LE(substr($payload, $offset, 4));
            $offset += 4;
            $columnIndex = self::readUInt16LE(substr($payload, $offset, 2));
            $offset += 2;
            $typeOid = self::readUInt32LE(substr($payload, $offset, 4));
            $offset += 4;
            $typeSize = self::readInt16LE(substr($payload, $offset, 2));
            $offset += 2;
            $typeModifier = self::readInt32LE(substr($payload, $offset, 4));
            $offset += 4;
            $format = ord($payload[$offset]);
            $offset += 1;
            $nullable = ord($payload[$offset]) === 1;
            $offset += 1;
            $offset += 2;
            $columns[] = [
                'name' => $name,
                'tableOid' => $tableOid,
                'columnIndex' => $columnIndex,
                'typeOid' => $typeOid,
                'typeSize' => $typeSize,
                'typeModifier' => $typeModifier,
                'format' => $format,
                'nullable' => $nullable,
            ];
        }
        return $columns;
    }

    public static function parseDataRow(string $payload): array
    {
        if (strlen($payload) < 4) {
            throw new \RuntimeException('Row data truncated');
        }
        $offset = 0;
        $count = self::readUInt16LE(substr($payload, $offset, 2));
        $offset += 2;
        $nullBytes = self::readUInt16LE(substr($payload, $offset, 2));
        $offset += 2;
        $nullBitmap = substr($payload, $offset, $nullBytes);
        $offset += $nullBytes;
        $values = [];
        for ($i = 0; $i < $count; $i++) {
            $byteIndex = intdiv($i, 8);
            $bitIndex = $i % 8;
            $isNull = $byteIndex < $nullBytes && ((ord($nullBitmap[$byteIndex]) & (1 << $bitIndex)) !== 0);
            if ($isNull) {
                $values[] = ['data' => null];
                continue;
            }
            $length = self::readInt32LE(substr($payload, $offset, 4));
            $offset += 4;
            if ($length < 0) {
                $values[] = ['data' => null];
                continue;
            }
            $data = substr($payload, $offset, $length);
            $offset += $length;
            $values[] = ['data' => $data];
        }
        return $values;
    }

    public static function parseCommandComplete(string $payload): array
    {
        if (strlen($payload) < 20) {
            throw new \RuntimeException('Command complete truncated');
        }
        $commandType = ord($payload[0]);
        $rows = self::readUInt64LE(substr($payload, 4, 8));
        $lastId = self::readUInt64LE(substr($payload, 12, 8));
        $tagBytes = substr($payload, 20);
        $nullPos = strpos($tagBytes, "\0");
        $tag = $nullPos === false ? $tagBytes : substr($tagBytes, 0, $nullPos);
        return [$commandType, $rows, $lastId, $tag];
    }

    public static function parseErrorMessage(string $payload): array
    {
        $offset = 0;
        $severity = '';
        $sqlState = '';
        $message = '';
        $detail = '';
        $hint = '';

        while ($offset < strlen($payload)) {
            $field = ord($payload[$offset]);
            $offset += 1;
            if ($field === 0) {
                break;
            }
            $start = $offset;
            while ($offset < strlen($payload) && $payload[$offset] !== "\0") {
                $offset += 1;
            }
            if ($offset >= strlen($payload)) {
                break;
            }
            $value = substr($payload, $start, $offset - $start);
            $offset += 1;
            switch (chr($field)) {
                case 'S':
                    $severity = $value;
                    break;
                case 'C':
                    $sqlState = $value;
                    break;
                case 'M':
                    $message = $value;
                    break;
                case 'D':
                    $detail = $value;
                    break;
                case 'H':
                    $hint = $value;
                    break;
            }
        }
        return [$severity, $sqlState, $message, $detail, $hint];
    }

    private static function writeUInt16LE(int $value): string
    {
        return pack('v', $value);
    }

    private static function writeInt32LE(int $value): string
    {
        if ($value < 0) {
            $value = 0x100000000 + $value;
        }
        return pack('V', $value);
    }

    private static function writeUInt32LE(int $value): string
    {
        return pack('V', $value);
    }

    private static function writeUInt64LE(int $value): string
    {
        $low = $value & 0xFFFFFFFF;
        $high = ($value >> 32) & 0xFFFFFFFF;
        return pack('V2', $low, $high);
    }

    private static function readUInt16LE(string $data): int
    {
        return unpack('v', $data)[1];
    }

    private static function readInt16LE(string $data): int
    {
        $value = unpack('v', $data)[1];
        return $value >= 0x8000 ? $value - 0x10000 : $value;
    }

    private static function readUInt32LE(string $data): int
    {
        $value = unpack('V', $data)[1];
        if ($value < 0) {
            $value += 0x100000000;
        }
        return $value;
    }

    private static function readInt32LE(string $data): int
    {
        $value = self::readUInt32LE($data);
        return $value >= 0x80000000 ? $value - 0x100000000 : $value;
    }

    private static function readUInt64LE(string $data): int
    {
        $parts = unpack('V2', $data);
        return (int)($parts[1] + ($parts[2] << 32));
    }

    private static function padBytes(string $data, int $length): string
    {
        if (strlen($data) === $length) {
            return $data;
        }
        if (strlen($data) > $length) {
            return substr($data, 0, $length);
        }
        return $data . str_repeat("\0", $length - strlen($data));
    }
}
