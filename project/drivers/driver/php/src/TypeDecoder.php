<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

namespace ScratchBird\PDO;

final class Jsonb
{
    public string $raw;
    public mixed $value;

    public function __construct(string $raw, mixed $value = null)
    {
        $this->raw = $raw;
        $this->value = $value;
    }
}

final class Geometry
{
    public string $wkb;
    public ?int $srid;
    public ?string $wkt;

    public function __construct(string $wkb, ?int $srid = null, ?string $wkt = null)
    {
        $this->wkb = $wkb;
        $this->srid = $srid;
        $this->wkt = $wkt;
    }
}

final class Range
{
    public mixed $lower = null;
    public mixed $upper = null;
    public bool $lowerInclusive = false;
    public bool $upperInclusive = false;
    public bool $lowerInfinite = false;
    public bool $upperInfinite = false;
    public bool $empty = false;
    public ?int $rangeOid = null;

    public function __construct(array $init = [])
    {
        foreach ($init as $key => $value) {
            if (property_exists($this, $key)) {
                $this->{$key} = $value;
            }
        }
    }
}

final class CompositeField
{
    public int $oid;
    public mixed $value;
    public ?string $raw;

    public function __construct(int $oid, mixed $value = null, ?string $raw = null)
    {
        $this->oid = $oid;
        $this->value = $value;
        $this->raw = $raw;
    }
}

final class Composite
{
    /** @var CompositeField[] */
    public array $fields;
    public int $typeOid;

    public function __construct(array $fields = [], int $typeOid = 0)
    {
        $this->fields = $fields;
        $this->typeOid = $typeOid;
    }
}

final class TypeDecoder
{
    public const FORMAT_TEXT = 0;
    public const FORMAT_BINARY = 1;

    public const OID_BOOL = 16;
    public const OID_BYTEA = 17;
    public const OID_CHAR = 18;
    public const OID_INT8 = 20;
    public const OID_INT2 = 21;
    public const OID_INT4 = 23;
    public const OID_TEXT = 25;
    public const OID_JSON = 114;
    public const OID_XML = 142;
    public const OID_POINT = 600;
    public const OID_LSEG = 601;
    public const OID_PATH = 602;
    public const OID_BOX = 603;
    public const OID_POLYGON = 604;
    public const OID_LINE = 628;
    public const OID_FLOAT4 = 700;
    public const OID_FLOAT8 = 701;
    public const OID_CIRCLE = 718;
    public const OID_MONEY = 790;
    public const OID_MACADDR = 829;
    public const OID_CIDR = 650;
    public const OID_INET = 869;
    public const OID_MACADDR8 = 774;
    public const OID_BPCHAR = 1042;
    public const OID_VARCHAR = 1043;
    public const OID_DATE = 1082;
    public const OID_TIME = 1083;
    public const OID_TIMESTAMP = 1114;
    public const OID_TIMESTAMPTZ = 1184;
    public const OID_INTERVAL = 1186;
    public const OID_TIMETZ = 1266;
    public const OID_NUMERIC = 1700;
    public const OID_UUID = 2950;
    public const OID_JSONB = 3802;
    public const OID_RECORD = 2249;
    public const OID_INT4RANGE = 3904;
    public const OID_NUMRANGE = 3906;
    public const OID_TSRANGE = 3908;
    public const OID_TSTZRANGE = 3910;
    public const OID_DATERANGE = 3912;
    public const OID_INT8RANGE = 3926;
    public const OID_TSVECTOR = 3614;
    public const OID_TSQUERY = 3615;
    public const OID_SB_VECTOR = 16386;

    private const RANGE_EMPTY = 0x01;
    private const RANGE_LB_INC = 0x02;
    private const RANGE_UB_INC = 0x04;
    private const RANGE_LB_INF = 0x08;
    private const RANGE_UB_INF = 0x10;

    private const UUID_REGEX = '/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i';

    public static function encodeParam(mixed $value): array
    {
        if ($value === null) {
            return ['param' => ['format' => self::FORMAT_BINARY, 'isNull' => true], 'oid' => 0];
        }
        if ($value instanceof Jsonb) {
            $raw = $value->raw;
            if ($raw === '' && $value->value !== null) {
                $raw = json_encode($value->value, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
            }
            if ($raw === '' || $raw === false) {
                throw new \InvalidArgumentException('JSONB requires raw payload');
            }
            return ['param' => ['format' => self::FORMAT_BINARY, 'data' => self::encodeLengthPrefixed($raw)], 'oid' => self::OID_JSONB];
        }
        if ($value instanceof Geometry) {
            if ($value->wkb === '') {
                throw new \InvalidArgumentException('geometry requires WKB payload');
            }
            return ['param' => ['format' => self::FORMAT_BINARY, 'data' => self::encodeLengthPrefixed($value->wkb)], 'oid' => self::OID_POINT];
        }
        if ($value instanceof Range) {
            $encoded = self::encodeRange($value);
            return ['param' => ['format' => self::FORMAT_BINARY, 'data' => $encoded['data']], 'oid' => $encoded['oid']];
        }
        if ($value instanceof Composite) {
            $encoded = self::encodeComposite($value);
            return ['param' => ['format' => self::FORMAT_BINARY, 'data' => $encoded['data']], 'oid' => $encoded['oid']];
        }
        if ($value instanceof \DateTimeInterface) {
            return ['param' => ['format' => self::FORMAT_BINARY, 'data' => self::encodeTimestamp($value)], 'oid' => self::OID_TIMESTAMPTZ];
        }
        if (is_bool($value)) {
            return ['param' => ['format' => self::FORMAT_BINARY, 'data' => $value ? "\1" : "\0"], 'oid' => self::OID_BOOL];
        }
        if (is_int($value)) {
            if ($value >= -2147483648 && $value <= 2147483647) {
                return ['param' => ['format' => self::FORMAT_BINARY, 'data' => self::packInt32($value)], 'oid' => self::OID_INT4];
            }
            return ['param' => ['format' => self::FORMAT_BINARY, 'data' => self::packInt64($value)], 'oid' => self::OID_INT8];
        }
        if (is_float($value)) {
            return ['param' => ['format' => self::FORMAT_BINARY, 'data' => pack('e', $value)], 'oid' => self::OID_FLOAT8];
        }
        if (is_string($value)) {
            if (preg_match(self::UUID_REGEX, $value)) {
                return ['param' => ['format' => self::FORMAT_BINARY, 'data' => hex2bin(str_replace('-', '', $value))], 'oid' => self::OID_UUID];
            }
            return ['param' => ['format' => self::FORMAT_BINARY, 'data' => self::encodeLengthPrefixed($value)], 'oid' => self::OID_TEXT];
        }
        if (is_array($value)) {
            if (self::isNumericArray($value)) {
                $literal = self::formatVectorLiteral($value);
                return ['param' => ['format' => self::FORMAT_BINARY, 'data' => self::encodeLengthPrefixed($literal)], 'oid' => self::OID_SB_VECTOR];
            }
            $literal = self::formatArrayLiteral($value);
            return ['param' => ['format' => self::FORMAT_BINARY, 'data' => self::encodeLengthPrefixed($literal)], 'oid' => 0];
        }
        if (is_object($value)) {
            $raw = json_encode($value, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
            if ($raw === false) {
                throw new \InvalidArgumentException('Failed to encode JSON');
            }
            return ['param' => ['format' => self::FORMAT_BINARY, 'data' => self::encodeLengthPrefixed($raw)], 'oid' => self::OID_JSON];
        }
        throw new \InvalidArgumentException('Unsupported parameter type');
    }

    public static function decode(int $typeOid, ?string $data, int $format): mixed
    {
        if ($data === null) {
            return null;
        }
        if ($typeOid === 0) {
            if ($format === self::FORMAT_TEXT) {
                return self::parseUnknownText(self::decodeTextValue($data));
            }
            return self::decodeUnknownBinary($data);
        }
        if ($format === self::FORMAT_TEXT) {
            return self::decodeTextTypedValue($typeOid, $data);
        }
        return self::decodeBinaryValue($typeOid, $data);
    }

    public static function oidName(int $oid): string
    {
        return match ($oid) {
            self::OID_BOOL => 'boolean',
            self::OID_INT2 => 'int2',
            self::OID_INT4 => 'int4',
            self::OID_INT8 => 'int8',
            self::OID_FLOAT4 => 'float4',
            self::OID_FLOAT8 => 'float8',
            self::OID_NUMERIC => 'numeric',
            self::OID_MONEY => 'money',
            self::OID_TEXT => 'text',
            self::OID_VARCHAR => 'varchar',
            self::OID_CHAR, self::OID_BPCHAR => 'char',
            self::OID_BYTEA => 'bytea',
            self::OID_DATE => 'date',
            self::OID_TIME => 'time',
            self::OID_TIMETZ => 'timetz',
            self::OID_TIMESTAMP => 'timestamp',
            self::OID_TIMESTAMPTZ => 'timestamptz',
            self::OID_INTERVAL => 'interval',
            self::OID_UUID => 'uuid',
            self::OID_JSON => 'json',
            self::OID_JSONB => 'jsonb',
            self::OID_XML => 'xml',
            self::OID_INET => 'inet',
            self::OID_CIDR => 'cidr',
            self::OID_MACADDR => 'macaddr',
            self::OID_MACADDR8 => 'macaddr8',
            self::OID_TSVECTOR => 'tsvector',
            self::OID_TSQUERY => 'tsquery',
            self::OID_INT4RANGE => 'int4range',
            self::OID_INT8RANGE => 'int8range',
            self::OID_NUMRANGE => 'numrange',
            self::OID_TSRANGE => 'tsrange',
            self::OID_TSTZRANGE => 'tstzrange',
            self::OID_DATERANGE => 'daterange',
            self::OID_SB_VECTOR => 'vector',
            default => 'unknown',
        };
    }

    private static function decodeBinaryValue(int $typeOid, string $data): mixed
    {
        $textFallback = self::maybeDecodeBinaryTextValue($typeOid, $data);
        if ($textFallback !== null) {
            return $textFallback;
        }
        return match ($typeOid) {
            self::OID_BOOL => ord($data[0]) === 1,
            self::OID_INT2 => self::readInt16($data),
            self::OID_INT4 => self::readInt32($data),
            self::OID_INT8 => self::readInt64($data),
            self::OID_FLOAT4 => unpack('g', $data)[1],
            self::OID_FLOAT8 => unpack('e', $data)[1],
            self::OID_NUMERIC => self::stripLengthPrefixed($data),
            self::OID_MONEY => self::decodeMoney(self::readInt64($data)),
            self::OID_TEXT,
            self::OID_VARCHAR,
            self::OID_CHAR,
            self::OID_BPCHAR,
            self::OID_JSON,
            self::OID_XML,
            self::OID_TSVECTOR,
            self::OID_TSQUERY,
            self::OID_TIMETZ => self::stripLengthPrefixed($data),
            self::OID_JSONB => new Jsonb(self::stripLengthPrefixed($data)),
            self::OID_BYTEA => self::stripLengthPrefixed($data),
            self::OID_DATE => self::decodeDate($data),
            self::OID_TIME => self::decodeTime($data),
            self::OID_TIMESTAMP => self::decodeTimestamp($data),
            self::OID_TIMESTAMPTZ => self::decodeTimestamp($data),
            self::OID_INTERVAL => self::decodeInterval($data),
            self::OID_UUID => self::decodeUuid($data),
            self::OID_INET,
            self::OID_CIDR,
            self::OID_MACADDR,
            self::OID_MACADDR8 => self::stripLengthPrefixed($data),
            self::OID_INT4RANGE,
            self::OID_INT8RANGE,
            self::OID_NUMRANGE,
            self::OID_TSRANGE,
            self::OID_TSTZRANGE,
            self::OID_DATERANGE => self::decodeRange($typeOid, $data),
            self::OID_SB_VECTOR => self::parseVectorLiteral(self::stripLengthPrefixed($data)),
            self::OID_POINT,
            self::OID_LSEG,
            self::OID_PATH,
            self::OID_BOX,
            self::OID_POLYGON,
            self::OID_LINE,
            self::OID_CIRCLE => new Geometry(self::stripLengthPrefixed($data)),
            self::OID_RECORD => self::decodeComposite($data),
            default => $data,
        };
    }

    private static function maybeDecodeBinaryTextValue(int $typeOid, string $data): mixed
    {
        $textLikeOids = [
            self::OID_TEXT,
            self::OID_VARCHAR,
            self::OID_CHAR,
            self::OID_BPCHAR,
            self::OID_JSON,
            self::OID_JSONB,
            self::OID_XML,
            self::OID_BYTEA,
            self::OID_INET,
            self::OID_CIDR,
            self::OID_MACADDR,
            self::OID_MACADDR8,
            self::OID_TIMETZ,
            self::OID_NUMERIC,
            self::OID_TSVECTOR,
            self::OID_TSQUERY,
            self::OID_SB_VECTOR,
        ];
        if (!in_array($typeOid, $textLikeOids, true)) {
            return null;
        }
        $candidates = [];
        $trimmed = self::stripTrailingNulls($data);
        if ($trimmed !== '' && self::looksLikeText($trimmed)) {
            $candidates[] = $trimmed;
        }
        if (strlen($data) >= 4) {
            $stripped = self::stripLengthPrefixed($data);
            if ($stripped !== $data && $stripped !== '' && self::looksLikeText($stripped)) {
                $candidates[] = $stripped;
            }
        }
        foreach ($candidates as $candidate) {
            try {
                return self::decodeTextTypedValue($typeOid, $candidate);
            } catch (\Throwable) {
                continue;
            }
        }
        return null;
    }

    private static function decodeTextValue(string $data): string
    {
        if (strlen($data) >= 4) {
            $length = self::readUInt32($data);
            if ($length <= strlen($data) - 4) {
                return substr($data, 4, $length);
            }
        }
        return $data;
    }

    private static function decodeTextTypedValue(int $typeOid, string $data): mixed
    {
        $text = self::decodeTextValue($data);
        $stripped = trim($text);
        return match ($typeOid) {
            self::OID_BOOL => self::parseBoolText($stripped),
            self::OID_INT2, self::OID_INT4, self::OID_INT8 => self::parseIntegerText($stripped),
            self::OID_FLOAT4, self::OID_FLOAT8 => self::parseFloatText($stripped),
            self::OID_NUMERIC, self::OID_MONEY => $stripped,
            self::OID_JSONB => new Jsonb($stripped),
            self::OID_BYTEA => self::decodeByteaText($stripped),
            self::OID_SB_VECTOR => self::parseVectorLiteral($stripped),
            default => $text,
        };
    }

    private static function decodeUnknownBinary(string $data): mixed
    {
        $trimmed = self::stripTrailingNulls($data);
        if ($trimmed !== '' && self::looksLikeText($trimmed)) {
            return self::parseUnknownText($trimmed);
        }
        $len = strlen($data);
        return match ($len) {
            1 => ord($data[0]),
            2 => self::readInt16($data),
            4 => self::readInt32($data),
            8 => self::readInt64($data),
            16 => self::decodeUuid($data),
            default => $data,
        };
    }

    private static function parseUnknownText(string $text): mixed
    {
        $trimmed = trim($text);
        if ($trimmed === '') {
            return $text;
        }
        $lowered = strtolower($trimmed);
        if ($lowered === 'true') {
            return true;
        }
        if ($lowered === 'false') {
            return false;
        }
        if (preg_match('/^[+-]?\d+$/', $trimmed)) {
            return (int)$trimmed;
        }
        if (preg_match('/^[+-]?(?:\d+\.?\d*|\d*\.?\d+)(?:[eE][+-]?\d+)?$/', $trimmed)) {
            return (float)$trimmed;
        }
        return $text;
    }

    private static function stripTrailingNulls(string $data): string
    {
        $end = strlen($data);
        while ($end > 0 && ord($data[$end - 1]) === 0) {
            $end -= 1;
        }
        return substr($data, 0, $end);
    }

    private static function looksLikeText(string $data): bool
    {
        $len = strlen($data);
        for ($i = 0; $i < $len; $i++) {
            $byte = ord($data[$i]);
            if ($byte === 0x09 || $byte === 0x0a || $byte === 0x0d) {
                continue;
            }
            if ($byte < 0x20 || $byte > 0x7e) {
                return false;
            }
        }
        return true;
    }

    private static function decodeByteaText(string $text): string
    {
        if (preg_match('/^(\\\\x|0x)/i', $text) === 1) {
            $hex = substr($text, 2);
            if ($hex !== '' && preg_match('/^[0-9a-f]+$/i', $hex) === 1) {
                $decoded = hex2bin($hex);
                if ($decoded !== false) {
                    return $decoded;
                }
            }
        }
        if ($text !== '' && (strlen($text) % 2) === 0 && preg_match('/^[0-9a-f]+$/i', $text) === 1) {
            $decoded = hex2bin($text);
            if ($decoded !== false) {
                return $decoded;
            }
        }
        return $text;
    }

    private static function parseBoolText(string $text): bool
    {
        $normalized = strtolower($text);
        return match ($normalized) {
            't', 'true', '1' => true,
            'f', 'false', '0' => false,
            default => throw new \InvalidArgumentException('invalid boolean text payload'),
        };
    }

    private static function parseIntegerText(string $text): int
    {
        if (preg_match('/^[+-]?\d+$/', $text) !== 1) {
            throw new \InvalidArgumentException('invalid integer text payload');
        }
        return (int)$text;
    }

    private static function parseFloatText(string $text): float
    {
        if (preg_match('/^[+-]?(?:\d+\.?\d*|\d*\.?\d+)(?:[eE][+-]?\d+)?$/', $text) !== 1) {
            throw new \InvalidArgumentException('invalid floating text payload');
        }
        return (float)$text;
    }

    private static function encodeLengthPrefixed(string $data): string
    {
        return pack('V', strlen($data)) . $data;
    }

    private static function encodeComposite(Composite $value): array
    {
        $fields = $value->fields ?? [];
        $typeOid = $value->typeOid ?: self::OID_RECORD;
        $buffer = self::packInt32(count($fields));
        foreach ($fields as $field) {
            $fieldOid = $field->oid ?? 0;
            $data = null;
            if ($field->raw !== null) {
                $data = $field->raw;
            } elseif ($field->value !== null) {
                $encoded = self::encodeParam($field->value);
                if ($fieldOid === 0) {
                    $fieldOid = $encoded['oid'];
                }
                $data = $encoded['param']['isNull'] ?? false ? null : ($encoded['param']['data'] ?? '');
            }
            if ($fieldOid === 0) {
                throw new \InvalidArgumentException('composite field OID is required');
            }
            $buffer .= pack('V', $fieldOid);
            if ($data === null) {
                $buffer .= self::packInt32(-1);
                continue;
            }
            $buffer .= self::packInt32(strlen($data));
            $buffer .= $data;
        }
        return ['data' => $buffer, 'oid' => $typeOid];
    }

    private static function decodeComposite(string $data): Composite
    {
        if (strlen($data) < 4) {
            return new Composite([]);
        }
        $count = self::readInt32(substr($data, 0, 4));
        $offset = 4;
        $fields = [];
        for ($i = 0; $i < $count; $i++) {
            if ($offset + 8 > strlen($data)) {
                break;
            }
            $oid = unpack('V', substr($data, $offset, 4))[1];
            $offset += 4;
            $length = self::readInt32(substr($data, $offset, 4));
            $offset += 4;
            if ($length < 0) {
                $fields[] = new CompositeField($oid, null, null);
                continue;
            }
            if ($offset + $length > strlen($data)) {
                break;
            }
            $raw = substr($data, $offset, $length);
            $offset += $length;
            $value = self::decodeBinaryValue($oid, $raw);
            $fields[] = new CompositeField($oid, $value, $raw);
        }
        return new Composite($fields, self::OID_RECORD);
    }

    private static function stripLengthPrefixed(string $data): string
    {
        if (strlen($data) < 4) {
            return $data;
        }
        $length = self::readUInt32($data);
        if ($length <= strlen($data) - 4) {
            return substr($data, 4, $length);
        }
        return $data;
    }

    private static function readUInt32(string $data): int
    {
        return unpack('V', substr($data, 0, 4))[1];
    }

    private static function readInt16(string $data): int
    {
        $value = unpack('v', substr($data, 0, 2))[1];
        return $value >= 0x8000 ? $value - 0x10000 : $value;
    }

    private static function readInt32(string $data): int
    {
        $value = unpack('V', substr($data, 0, 4))[1];
        return $value >= 0x80000000 ? $value - 0x100000000 : $value;
    }

    private static function readInt64(string $data): int
    {
        $parts = unpack('V2', $data);
        $value = $parts[1] + ($parts[2] << 32);
        if ($value >= 0x8000000000000000) {
            $value -= 0x10000000000000000;
        }
        return $value;
    }

    private static function packInt32(int $value): string
    {
        if ($value < 0) {
            $value = 0x100000000 + $value;
        }
        return pack('V', $value);
    }

    private static function packInt64(int $value): string
    {
        if ($value < 0) {
            $value = 0x10000000000000000 + $value;
        }
        $low = $value & 0xFFFFFFFF;
        $high = ($value >> 32) & 0xFFFFFFFF;
        return pack('V2', $low, $high);
    }

    private static function decodeDate(string $data): \DateTimeImmutable
    {
        $days = self::readInt32($data);
        $base = new \DateTimeImmutable('2000-01-01 00:00:00', new \DateTimeZone('UTC'));
        return $base->modify(sprintf('%+d days', $days));
    }

    private static function decodeTime(string $data): \DateTimeImmutable
    {
        $micros = self::readInt64($data);
        $seconds = intdiv($micros, 1000000);
        $microPart = $micros % 1000000;
        $time = new \DateTimeImmutable('@' . $seconds);
        $time = $time->setTimezone(new \DateTimeZone('UTC'));
        return $time->setTime(
            (int)$time->format('H'),
            (int)$time->format('i'),
            (int)$time->format('s'),
            (int)$microPart
        );
    }

    private static function decodeTimestamp(string $data): \DateTimeImmutable
    {
        $micros = self::readInt64($data);
        $seconds = intdiv($micros, 1000000);
        $microPart = $micros % 1000000;
        $base = new \DateTimeImmutable('2000-01-01 00:00:00', new \DateTimeZone('UTC'));
        $dt = $base->modify(sprintf('%+d seconds', $seconds));
        return $dt->setTime(
            (int)$dt->format('H'),
            (int)$dt->format('i'),
            (int)$dt->format('s'),
            (int)$microPart
        );
    }

    private static function decodeInterval(string $data): array
    {
        $micros = self::readInt64(substr($data, 0, 8));
        $days = self::readInt32(substr($data, 8, 4));
        $months = self::readInt32(substr($data, 12, 4));
        return ['micros' => $micros, 'days' => $days, 'months' => $months];
    }

    private static function decodeUuid(string $data): string
    {
        $hex = bin2hex($data);
        if (strlen($hex) !== 32) {
            return $hex;
        }
        return substr($hex, 0, 8) . '-' . substr($hex, 8, 4) . '-' . substr($hex, 12, 4) . '-' . substr($hex, 16, 4) . '-' . substr($hex, 20);
    }

    private static function decodeMoney(int $cents): string
    {
        $negative = $cents < 0;
        $abs = $negative ? -$cents : $cents;
        $units = intdiv($abs, 100);
        $fraction = $abs % 100;
        $value = sprintf('%d.%02d', $units, $fraction);
        return $negative ? '-' . $value : $value;
    }

    private static function encodeTimestamp(\DateTimeInterface $value): string
    {
        $utc = (new \DateTimeImmutable($value->format('Y-m-d H:i:s.u'), $value->getTimezone()))->setTimezone(new \DateTimeZone('UTC'));
        $seconds = (int)$utc->format('U');
        $micros = (int)$utc->format('u');
        $base = new \DateTimeImmutable('2000-01-01 00:00:00', new \DateTimeZone('UTC'));
        $baseSeconds = (int)$base->format('U');
        $deltaMicros = ($seconds - $baseSeconds) * 1000000 + $micros;
        return self::packInt64($deltaMicros);
    }

    private static function encodeRange(Range $range): array
    {
        $oid = self::resolveRangeOid($range);
        $flags = ($range->empty ? self::RANGE_EMPTY : 0)
            | ($range->lowerInclusive ? self::RANGE_LB_INC : 0)
            | ($range->upperInclusive ? self::RANGE_UB_INC : 0)
            | ($range->lowerInfinite ? self::RANGE_LB_INF : 0)
            | ($range->upperInfinite ? self::RANGE_UB_INF : 0);
        $parts = [pack('C4', $flags, 0, 0, 0)];
        if (!$range->empty && !$range->lowerInfinite) {
            $bound = self::encodeRangeBound($oid, $range->lower);
            $parts[] = pack('V', strlen($bound));
            $parts[] = $bound;
        }
        if (!$range->empty && !$range->upperInfinite) {
            $bound = self::encodeRangeBound($oid, $range->upper);
            $parts[] = pack('V', strlen($bound));
            $parts[] = $bound;
        }
        return ['data' => implode('', $parts), 'oid' => $oid];
    }

    private static function resolveRangeOid(Range $range): int
    {
        if ($range->rangeOid !== null) {
            return $range->rangeOid;
        }
        $sample = $range->lower ?? $range->upper;
        if ($sample === null) {
            throw new \InvalidArgumentException('range type cannot be inferred from empty bounds');
        }
        if ($sample instanceof \DateTimeInterface) {
            return self::OID_TSTZRANGE;
        }
        if (is_int($sample)) {
            return ($sample >= -2147483648 && $sample <= 2147483647) ? self::OID_INT4RANGE : self::OID_INT8RANGE;
        }
        if (is_float($sample) || is_string($sample)) {
            return self::OID_NUMRANGE;
        }
        return self::OID_NUMRANGE;
    }

    private static function encodeRangeBound(int $oid, mixed $value): string
    {
        return match ($oid) {
            self::OID_INT4RANGE => self::packInt32((int)$value),
            self::OID_INT8RANGE => self::packInt64((int)$value),
            self::OID_NUMRANGE => self::encodeLengthPrefixed((string)$value),
            self::OID_DATERANGE => self::encodeDate($value),
            self::OID_TSRANGE, self::OID_TSTZRANGE => self::encodeTimestamp($value instanceof \DateTimeInterface ? $value : new \DateTimeImmutable((string)$value)),
            default => self::encodeLengthPrefixed((string)$value),
        };
    }

    private static function decodeRange(int $oid, string $data): Range
    {
        if (strlen($data) < 4) {
            return new Range(['rangeOid' => $oid]);
        }
        $flags = ord($data[0]);
        $offset = 4;
        $range = new Range([
            'empty' => ($flags & self::RANGE_EMPTY) !== 0,
            'lowerInclusive' => ($flags & self::RANGE_LB_INC) !== 0,
            'upperInclusive' => ($flags & self::RANGE_UB_INC) !== 0,
            'lowerInfinite' => ($flags & self::RANGE_LB_INF) !== 0,
            'upperInfinite' => ($flags & self::RANGE_UB_INF) !== 0,
            'rangeOid' => $oid,
        ]);
        if ($range->empty) {
            return $range;
        }
        if (!$range->lowerInfinite) {
            $length = self::readInt32(substr($data, $offset, 4));
            $offset += 4;
            $bound = substr($data, $offset, $length);
            $offset += $length;
            $range->lower = self::decodeRangeBound($oid, $bound);
        }
        if (!$range->upperInfinite) {
            $length = self::readInt32(substr($data, $offset, 4));
            $offset += 4;
            $bound = substr($data, $offset, $length);
            $range->upper = self::decodeRangeBound($oid, $bound);
        }
        return $range;
    }

    private static function decodeRangeBound(int $oid, string $data): mixed
    {
        return match ($oid) {
            self::OID_INT4RANGE => self::readInt32($data),
            self::OID_INT8RANGE => self::readInt64($data),
            self::OID_NUMRANGE => self::stripLengthPrefixed($data),
            self::OID_DATERANGE => self::decodeDate($data),
            self::OID_TSRANGE, self::OID_TSTZRANGE => self::decodeTimestamp($data),
            default => null,
        };
    }

    private static function encodeDate(mixed $value): string
    {
        if ($value instanceof \DateTimeInterface) {
            $date = $value;
        } else {
            $date = new \DateTimeImmutable((string)$value, new \DateTimeZone('UTC'));
        }
        $base = new \DateTimeImmutable('2000-01-01', new \DateTimeZone('UTC'));
        $days = (int)$base->diff($date)->format('%r%a');
        return self::packInt32($days);
    }

    private static function isNumericArray(array $values): bool
    {
        foreach ($values as $value) {
            if (!is_int($value) && !is_float($value)) {
                return false;
            }
        }
        return true;
    }

    private static function formatArrayLiteral(array $values): string
    {
        $items = array_map([self::class, 'formatArrayItem'], $values);
        return '{' . implode(',', $items) . '}';
    }

    private static function formatArrayItem(mixed $value): string
    {
        if ($value === null) {
            return 'NULL';
        }
        if (is_array($value)) {
            return self::formatArrayLiteral($value);
        }
        if (is_string($value)) {
            return '"' . str_replace('"', '\\"', $value) . '"';
        }
        if (is_bool($value)) {
            return $value ? 'true' : 'false';
        }
        return (string)$value;
    }

    private static function parseVectorLiteral(string $text): array
    {
        $trimmed = trim($text);
        if ($trimmed !== '' && $trimmed[0] === '[' && $trimmed[strlen($trimmed) - 1] === ']') {
            $trimmed = substr($trimmed, 1, -1);
        }
        if ($trimmed === '') {
            return [];
        }
        $parts = array_map('trim', explode(',', $trimmed));
        $values = [];
        foreach ($parts as $part) {
            if ($part === '') {
                continue;
            }
            $values[] = (float)$part;
        }
        return $values;
    }

    private static function formatVectorLiteral(array $values): string
    {
        $parts = array_map(static fn($value) => is_finite($value) ? (string)$value : '0', $values);
        return '[' . implode(',', $parts) . ']';
    }
}
