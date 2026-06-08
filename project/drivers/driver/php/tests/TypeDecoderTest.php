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
use ScratchBird\PDO\Composite;
use ScratchBird\PDO\CompositeField;
use ScratchBird\PDO\Geometry;
use ScratchBird\PDO\Jsonb;
use ScratchBird\PDO\Range;
use ScratchBird\PDO\TypeDecoder;

final class TypeDecoderTest extends TestCase
{
    public function testEncodeParamCoversRepresentativeScalarAndStructuredInputs(): void
    {
        $bool = TypeDecoder::encodeParam(true);
        $this->assertSame(TypeDecoder::OID_BOOL, $bool['oid']);
        $this->assertSame("\1", $bool['param']['data']);

        $int4 = TypeDecoder::encodeParam(42);
        $this->assertSame(TypeDecoder::OID_INT4, $int4['oid']);
        $this->assertSame(42, unpack('V', $int4['param']['data'])[1]);

        $int8 = TypeDecoder::encodeParam(2147483648);
        $this->assertSame(TypeDecoder::OID_INT8, $int8['oid']);
        $this->assertSame(8, strlen($int8['param']['data']));

        $json = TypeDecoder::encodeParam((object)['role' => 'admin', 'active' => true]);
        $this->assertSame(TypeDecoder::OID_JSON, $json['oid']);
        $this->assertGreaterThan(4, strlen($json['param']['data']));

        $vector = TypeDecoder::encodeParam([1, 2, 3]);
        $this->assertSame(TypeDecoder::OID_SB_VECTOR, $vector['oid']);
        $this->assertSame('[1,2,3]', substr($vector['param']['data'], 4));
    }

    public function testEncodeParamRejectsInvalidInputs(): void
    {
        $this->expectException(\InvalidArgumentException::class);
        $this->expectExceptionMessage('JSONB requires raw payload');
        TypeDecoder::encodeParam(new Jsonb(''));
    }

    public function testEncodeParamRejectsInvalidGeometryAndUnsupportedType(): void
    {
        try {
            TypeDecoder::encodeParam(new Geometry(''));
            $this->fail('Expected geometry encode to fail');
        } catch (\InvalidArgumentException $ex) {
            $this->assertStringContainsString('geometry requires WKB payload', $ex->getMessage());
        }

        $resource = tmpfile();
        try {
            TypeDecoder::encodeParam($resource);
            $this->fail('Expected resource encode to fail');
        } catch (\InvalidArgumentException $ex) {
            $this->assertStringContainsString('Unsupported parameter type', $ex->getMessage());
        } finally {
            if (is_resource($resource)) {
                fclose($resource);
            }
        }
    }

    public function testDecodeRepresentativeBinaryValues(): void
    {
        $numeric = TypeDecoder::decode(TypeDecoder::OID_NUMERIC, $this->lenPrefixed('12345.678'), TypeDecoder::FORMAT_BINARY);
        $this->assertSame('12345.678', $numeric);

        $money = TypeDecoder::decode(TypeDecoder::OID_MONEY, pack('V2', 12345, 0), TypeDecoder::FORMAT_BINARY);
        $this->assertSame('123.45', $money);

        $uuid = TypeDecoder::decode(TypeDecoder::OID_UUID, hex2bin('00112233445566778899aabbccddeeff'), TypeDecoder::FORMAT_BINARY);
        $this->assertSame('00112233-4455-6677-8899-aabbccddeeff', $uuid);

        $jsonb = TypeDecoder::decode(TypeDecoder::OID_JSONB, $this->lenPrefixed('{"k":1}'), TypeDecoder::FORMAT_BINARY);
        $this->assertInstanceOf(Jsonb::class, $jsonb);
        $this->assertSame('{"k":1}', $jsonb->raw);

        $vector = TypeDecoder::decode(TypeDecoder::OID_SB_VECTOR, $this->lenPrefixed('[0.5,1.5,2.5]'), TypeDecoder::FORMAT_BINARY);
        $this->assertSame([0.5, 1.5, 2.5], $vector);
    }

    public function testDecodeRangeAndCompositePayloads(): void
    {
        $rangePayload = pack('C4', 0, 0, 0, 0)
            . pack('V', 8) . pack('V2', 10, 0)
            . pack('V', 8) . pack('V2', 20, 0);
        $range = TypeDecoder::decode(TypeDecoder::OID_INT8RANGE, $rangePayload, TypeDecoder::FORMAT_BINARY);
        $this->assertInstanceOf(Range::class, $range);
        $this->assertSame(10, $range->lower);
        $this->assertSame(20, $range->upper);
        $this->assertFalse($range->empty);

        $compositePayload = pack('V', 1)
            . pack('V', TypeDecoder::OID_INT4)
            . pack('V', 4)
            . pack('V', 77);
        $composite = TypeDecoder::decode(TypeDecoder::OID_RECORD, $compositePayload, TypeDecoder::FORMAT_BINARY);
        $this->assertInstanceOf(Composite::class, $composite);
        $this->assertCount(1, $composite->fields);
        $this->assertInstanceOf(CompositeField::class, $composite->fields[0]);
        $this->assertSame(TypeDecoder::OID_INT4, $composite->fields[0]->oid);
        $this->assertSame(77, $composite->fields[0]->value);
    }

    public function testDecodeUnknownTypeHeuristicsForTextAndBinary(): void
    {
        $this->assertTrue(TypeDecoder::decode(0, 'true', TypeDecoder::FORMAT_TEXT));
        $this->assertSame(42, TypeDecoder::decode(0, '42', TypeDecoder::FORMAT_TEXT));
        $this->assertSame(12.5, TypeDecoder::decode(0, '12.5', TypeDecoder::FORMAT_TEXT));

        $binaryInt = pack('V', 321);
        $this->assertSame(321, TypeDecoder::decode(0, $binaryInt, TypeDecoder::FORMAT_BINARY));
    }

    public function testDecodeBinaryScalarOidMatrix(): void
    {
        $this->assertTrue(TypeDecoder::decode(TypeDecoder::OID_BOOL, "\1", TypeDecoder::FORMAT_BINARY));
        $this->assertSame(123, TypeDecoder::decode(TypeDecoder::OID_INT2, pack('v', 123), TypeDecoder::FORMAT_BINARY));
        $this->assertSame(12345, TypeDecoder::decode(TypeDecoder::OID_INT4, pack('V', 12345), TypeDecoder::FORMAT_BINARY));
        $this->assertSame(9876543210, TypeDecoder::decode(TypeDecoder::OID_INT8, $this->uint64Le(9876543210), TypeDecoder::FORMAT_BINARY));
        $this->assertEqualsWithDelta(1.25, TypeDecoder::decode(TypeDecoder::OID_FLOAT4, pack('g', 1.25), TypeDecoder::FORMAT_BINARY), 0.000001);
        $this->assertEqualsWithDelta(2.5, TypeDecoder::decode(TypeDecoder::OID_FLOAT8, pack('e', 2.5), TypeDecoder::FORMAT_BINARY), 0.000001);
        $this->assertSame('hello', TypeDecoder::decode(TypeDecoder::OID_TEXT, $this->lenPrefixed('hello'), TypeDecoder::FORMAT_BINARY));
        $this->assertSame('{"a":1}', TypeDecoder::decode(TypeDecoder::OID_JSON, $this->lenPrefixed('{"a":1}'), TypeDecoder::FORMAT_BINARY));
        $this->assertSame('127.0.0.1', TypeDecoder::decode(TypeDecoder::OID_INET, $this->lenPrefixed('127.0.0.1'), TypeDecoder::FORMAT_BINARY));
        $this->assertSame('00:11:22:33:44:55', TypeDecoder::decode(TypeDecoder::OID_MACADDR, $this->lenPrefixed('00:11:22:33:44:55'), TypeDecoder::FORMAT_BINARY));
        $this->assertSame('12:34:56+00', TypeDecoder::decode(TypeDecoder::OID_TIMETZ, $this->lenPrefixed('12:34:56+00'), TypeDecoder::FORMAT_BINARY));
        $this->assertSame('0.99', TypeDecoder::decode(TypeDecoder::OID_MONEY, $this->uint64Le(99), TypeDecoder::FORMAT_BINARY));
    }

    public function testDecodeTemporalAndIntervalFamilies(): void
    {
        $date = TypeDecoder::decode(TypeDecoder::OID_DATE, pack('V', 0), TypeDecoder::FORMAT_BINARY);
        $this->assertInstanceOf(\DateTimeImmutable::class, $date);
        $this->assertSame('2000-01-01', $date->format('Y-m-d'));

        $time = TypeDecoder::decode(TypeDecoder::OID_TIME, $this->uint64Le(3723000000), TypeDecoder::FORMAT_BINARY);
        $this->assertInstanceOf(\DateTimeImmutable::class, $time);
        $this->assertSame('01:02:03', $time->format('H:i:s'));

        $timestamp = TypeDecoder::decode(TypeDecoder::OID_TIMESTAMP, $this->uint64Le(0), TypeDecoder::FORMAT_BINARY);
        $this->assertInstanceOf(\DateTimeImmutable::class, $timestamp);
        $this->assertSame('2000-01-01 00:00:00', $timestamp->format('Y-m-d H:i:s'));

        $intervalPayload = $this->uint64Le(15_000_000) . pack('V', 2) . pack('V', 1);
        $interval = TypeDecoder::decode(TypeDecoder::OID_INTERVAL, $intervalPayload, TypeDecoder::FORMAT_BINARY);
        $this->assertSame(['micros' => 15_000_000, 'days' => 2, 'months' => 1], $interval);
    }

    public function testEncodeDecodeExtendedRangeFamilies(): void
    {
        $intRange = new Range([
            'lower' => 5,
            'upper' => 15,
            'lowerInclusive' => true,
            'upperInclusive' => false,
        ]);
        $encodedIntRange = TypeDecoder::encodeParam($intRange);
        $decodedIntRange = TypeDecoder::decode($encodedIntRange['oid'], $encodedIntRange['param']['data'], TypeDecoder::FORMAT_BINARY);
        $this->assertInstanceOf(Range::class, $decodedIntRange);
        $this->assertSame(5, $decodedIntRange->lower);
        $this->assertSame(15, $decodedIntRange->upper);

        $dateRange = new Range([
            'lower' => new \DateTimeImmutable('2026-03-06 00:00:00', new \DateTimeZone('UTC')),
            'upper' => new \DateTimeImmutable('2026-03-08 00:00:00', new \DateTimeZone('UTC')),
            'rangeOid' => TypeDecoder::OID_DATERANGE,
        ]);
        $encodedDateRange = TypeDecoder::encodeParam($dateRange);
        $decodedDateRange = TypeDecoder::decode($encodedDateRange['oid'], $encodedDateRange['param']['data'], TypeDecoder::FORMAT_BINARY);
        $this->assertInstanceOf(Range::class, $decodedDateRange);
        $this->assertInstanceOf(\DateTimeImmutable::class, $decodedDateRange->lower);
        $this->assertSame('2026-03-06', $decodedDateRange->lower->format('Y-m-d'));
        $this->assertSame('2026-03-08', $decodedDateRange->upper->format('Y-m-d'));
    }

    public function testEncodeDecodeJsonbGeometryCompositeAndDateTimeRoundTrip(): void
    {
        $jsonb = TypeDecoder::encodeParam(new Jsonb('', ['name' => 'scratchbird']));
        $decodedJsonb = TypeDecoder::decode($jsonb['oid'], $jsonb['param']['data'], TypeDecoder::FORMAT_BINARY);
        $this->assertInstanceOf(Jsonb::class, $decodedJsonb);
        $this->assertSame('{"name":"scratchbird"}', $decodedJsonb->raw);

        $geometry = TypeDecoder::encodeParam(new Geometry('0101000000000000000000F03F0000000000000040'));
        $decodedGeometry = TypeDecoder::decode($geometry['oid'], $geometry['param']['data'], TypeDecoder::FORMAT_BINARY);
        $this->assertInstanceOf(Geometry::class, $decodedGeometry);
        $this->assertSame('0101000000000000000000F03F0000000000000040', $decodedGeometry->wkb);

        $composite = new Composite(
            [
                new CompositeField(TypeDecoder::OID_INT4, 7),
                new CompositeField(TypeDecoder::OID_TEXT, 'bird'),
            ],
            TypeDecoder::OID_RECORD
        );
        $encodedComposite = TypeDecoder::encodeParam($composite);
        $decodedComposite = TypeDecoder::decode($encodedComposite['oid'], $encodedComposite['param']['data'], TypeDecoder::FORMAT_BINARY);
        $this->assertInstanceOf(Composite::class, $decodedComposite);
        $this->assertCount(2, $decodedComposite->fields);
        $this->assertSame(7, $decodedComposite->fields[0]->value);
        $this->assertSame('bird', $decodedComposite->fields[1]->value);

        $encodedDateTime = TypeDecoder::encodeParam(new \DateTimeImmutable('2026-03-06T12:34:56+00:00'));
        $decodedDateTime = TypeDecoder::decode($encodedDateTime['oid'], $encodedDateTime['param']['data'], TypeDecoder::FORMAT_BINARY);
        $this->assertInstanceOf(\DateTimeImmutable::class, $decodedDateTime);
        $this->assertSame('2026-03-06 12:34:56', $decodedDateTime->format('Y-m-d H:i:s'));
    }

    public function testOidNamesCoverExtendedFamilies(): void
    {
        $this->assertSame('timetz', TypeDecoder::oidName(TypeDecoder::OID_TIMETZ));
        $this->assertSame('vector', TypeDecoder::oidName(TypeDecoder::OID_SB_VECTOR));
        $this->assertSame('unknown', TypeDecoder::oidName(999999));
    }

    private function lenPrefixed(string $value): string
    {
        return pack('V', strlen($value)) . $value;
    }

    private function uint64Le(int $value): string
    {
        $lo = $value & 0xFFFFFFFF;
        $hi = ($value >> 32) & 0xFFFFFFFF;
        return pack('V2', $lo, $hi);
    }
}
