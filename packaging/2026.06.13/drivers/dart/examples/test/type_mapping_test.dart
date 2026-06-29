// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import 'dart:convert';
import 'dart:typed_data';

import 'package:test/test.dart';
import 'package:scratchbird/src/types.dart';

Uint8List _i16(int value) {
  final buf = ByteData(2);
  buf.setInt16(0, value, Endian.little);
  return buf.buffer.asUint8List();
}

Uint8List _i32(int value) {
  final buf = ByteData(4);
  buf.setInt32(0, value, Endian.little);
  return buf.buffer.asUint8List();
}

Uint8List _i64(int value) {
  final buf = ByteData(8);
  buf.setInt64(0, value, Endian.little);
  return buf.buffer.asUint8List();
}

Uint8List _lenPrefixed(Uint8List payload) {
  final out = Uint8List(4 + payload.length);
  final header = ByteData.sublistView(out);
  header.setUint32(0, payload.length, Endian.little);
  out.setRange(4, out.length, payload);
  return out;
}

Uint8List _textPayload(String value) {
  return _lenPrefixed(Uint8List.fromList(utf8.encode(value)));
}

void main() {
  test('array literal round-trip (binary unknown)', () {
    final encoded = encodeParam([1, 2, 3]);
    final decoded = decodeValue(0, encoded.param.data!, 1);
    expect(decoded, equals([1, 2, 3]));
  });

  test('vector literal round-trip', () {
    final encoded = encodeParam([1.0, 2.5, 3.25]);
    expect(encoded.oid, equals(oidVector));
    final decoded =
        decodeValue(oidVector, encoded.param.data!, 1) as List<dynamic>;
    expect(decoded.length, equals(3));
    expect(decoded[0], closeTo(1.0, 0.00001));
    expect(decoded[1], closeTo(2.5, 0.00001));
    expect(decoded[2], closeTo(3.25, 0.00001));
  });

  test('range round-trip (int4range)', () {
    final range = ScratchBirdRange<int>(
      lower: 1,
      upper: 10,
      lowerInclusive: true,
      upperInclusive: false,
      rangeOid: oidInt4Range,
    );
    final encoded = encodeParam(range);
    expect(encoded.oid, equals(oidInt4Range));
    final decoded =
        decodeValue(encoded.oid, encoded.param.data!, 1) as ScratchBirdRange;
    expect(decoded.lower, equals(1));
    expect(decoded.upper, equals(10));
    expect(decoded.lowerInclusive, isTrue);
    expect(decoded.upperInclusive, isFalse);
    expect(decoded.empty, isFalse);
  });

  test('composite round-trip', () {
    final comp = ScratchBirdComposite(fields: [
      ScratchBirdCompositeField(oid: oidInt4, value: 7),
      ScratchBirdCompositeField(oid: oidText, value: "hello"),
    ]);
    final encoded = encodeParam(comp);
    expect(encoded.oid, equals(oidRecord));
    final decoded =
        decodeValue(oidRecord, encoded.param.data!, 1) as ScratchBirdComposite;
    expect(decoded.fields.length, equals(2));
    expect(decoded.fields[0].value, equals(7));
    expect(decoded.fields[1].value, equals("hello"));
  });

  test('inet/cidr/macaddr round-trip', () {
    final inet = ScratchBirdInet("127.0.0.1");
    final cidr = ScratchBirdCidr("10.0.0.0/24");
    final mac = ScratchBirdMacaddr("aa:bb:cc:dd:ee:ff");

    final inetEnc = encodeParam(inet);
    final cidrEnc = encodeParam(cidr);
    final macEnc = encodeParam(mac);

    expect(decodeValue(oidInet, inetEnc.param.data!, 1), equals("127.0.0.1"));
    expect(decodeValue(oidCidr, cidrEnc.param.data!, 1), equals("10.0.0.0/24"));
    expect(decodeValue(oidMacaddr, macEnc.param.data!, 1),
        equals("aa:bb:cc:dd:ee:ff"));
  });

  test('scalar decode paths for core binary types', () {
    expect(decodeValue(oidBool, Uint8List.fromList([1]), 1), isTrue);
    expect(decodeValue(oidBool, Uint8List.fromList([0]), 1), isFalse);
    expect(decodeValue(oidInt2, _i16(-7), 1), equals(-7));
    expect(decodeValue(oidInt4, _i32(12345), 1), equals(12345));
    expect(decodeValue(oidInt8, _i64(-9876543210), 1), equals(-9876543210));

    final base = DateTime.utc(2000, 1, 1);
    expect(
      decodeValue(oidDate, _i32(5), 1),
      equals(base.add(const Duration(days: 5))),
    );
    expect(decodeValue(oidTime, _i64(123456), 1), equals(123456));
    expect(
      decodeValue(oidTimestamp, _i64(777000), 1),
      equals(base.add(const Duration(microseconds: 777000))),
    );
    expect(
      decodeValue(oidTimestamptz, _i64(-500000), 1),
      equals(base.add(const Duration(microseconds: -500000))),
    );
  });

  test('uuid/json/jsonb decode paths', () {
    final uuidBytes =
        Uint8List.fromList(hexToBytes('00112233445566778899aabbccddeeff'));
    expect(
      decodeValue(oidUuid, uuidBytes, 1),
      equals('00112233-4455-6677-8899-aabbccddeeff'),
    );

    expect(
      decodeValue(oidJson, _textPayload('{"k":1}'), 1),
      equals('{"k":1}'),
    );

    final jsonb =
        decodeValue(oidJsonb, _lenPrefixed(Uint8List.fromList([1, 2, 3])), 1)
            as ScratchBirdJsonb;
    expect(jsonb.raw, orderedEquals(<int>[1, 2, 3]));
  });

  test('text mode preserves typed values but parses unknown-oid text', () {
    expect(
      decodeValue(oidInt4, Uint8List.fromList(utf8.encode('42')), 0),
      equals('42'),
    );
    expect(
      decodeValue(oidBool, Uint8List.fromList(utf8.encode('true')), 0),
      equals('true'),
    );
    expect(
      decodeValue(0, Uint8List.fromList(utf8.encode('42')), 0),
      equals(42),
    );
    expect(
      decodeValue(0, Uint8List.fromList(utf8.encode('{1,2.5,true,NULL}')), 0),
      equals(<dynamic>[1, 2.5, true, null]),
    );
  });

  test('range encoding fails when oid cannot be inferred', () {
    expect(
      () => encodeParam(
        ScratchBirdRange<dynamic>(
          rangeOid: 0,
          lower: null,
          upper: null,
        ),
      ),
      throwsA(
        isA<Exception>().having(
          (e) => e.toString(),
          'message',
          contains('range type cannot be inferred'),
        ),
      ),
    );
  });

  test('range encoding rejects incompatible bound types', () {
    expect(
      () => encodeParam(
        ScratchBirdRange<int>(
          rangeOid: oidDateRange,
          lower: 1,
          upper: 2,
        ),
      ),
      throwsA(
        isA<Exception>().having(
          (e) => e.toString(),
          'message',
          contains('daterange requires DateTime bounds'),
        ),
      ),
    );
  });

  test('composite encoding requires field oid when value is null', () {
    expect(
      () => encodeParam(
        ScratchBirdComposite(fields: [
          ScratchBirdCompositeField(value: null),
        ]),
      ),
      throwsA(
        isA<Exception>().having(
          (e) => e.toString(),
          'message',
          contains('composite field OID is required'),
        ),
      ),
    );
  });

  test('unsupported parameter type throws', () {
    expect(
      () => encodeParam(Object()),
      throwsA(
        isA<Exception>().having(
          (e) => e.toString(),
          'message',
          contains('Unsupported parameter type'),
        ),
      ),
    );
  });
}
