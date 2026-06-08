// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import 'dart:typed_data';
import 'dart:convert';

import 'protocol.dart';

const int oidBool = 16;
const int oidBytea = 17;
const int oidChar = 18;
const int oidInt8 = 20;
const int oidInt2 = 21;
const int oidInt4 = 23;
const int oidText = 25;
const int oidJson = 114;
const int oidXml = 142;
const int oidPoint = 600;
const int oidFloat4 = 700;
const int oidFloat8 = 701;
const int oidMoney = 790;
const int oidMacaddr = 829;
const int oidCidr = 650;
const int oidInet = 869;
const int oidBpchar = 1042;
const int oidVarchar = 1043;
const int oidDate = 1082;
const int oidTime = 1083;
const int oidTimestamp = 1114;
const int oidTimestamptz = 1184;
const int oidInterval = 1186;
const int oidNumeric = 1700;
const int oidUuid = 2950;
const int oidJsonb = 3802;
const int oidRecord = 2249;
const int oidInt4Range = 3904;
const int oidNumRange = 3906;
const int oidTsRange = 3908;
const int oidTstzRange = 3910;
const int oidDateRange = 3912;
const int oidInt8Range = 3926;
const int oidTsVector = 3614;
const int oidTsQuery = 3615;
const int oidVector = 16386;

const int rangeEmpty = 0x01;
const int rangeLbInc = 0x02;
const int rangeUbInc = 0x04;
const int rangeLbInf = 0x08;
const int rangeUbInf = 0x10;

class ScratchBirdJsonb {
  final Uint8List raw;
  ScratchBirdJsonb(this.raw);
}

class ScratchBirdJson {
  final Uint8List raw;
  ScratchBirdJson(this.raw);
}

class ScratchBirdGeometry {
  final Uint8List wkb;
  ScratchBirdGeometry(this.wkb);
}

class ScratchBirdInet {
  final String value;
  ScratchBirdInet(this.value);
}

class ScratchBirdCidr {
  final String value;
  ScratchBirdCidr(this.value);
}

class ScratchBirdMacaddr {
  final String value;
  ScratchBirdMacaddr(this.value);
}

class ScratchBirdCompositeField {
  final int? oid;
  final dynamic value;
  final Uint8List? raw;

  ScratchBirdCompositeField({this.oid, this.value, this.raw});
}

class ScratchBirdComposite {
  final int typeOid;
  final List<ScratchBirdCompositeField> fields;

  ScratchBirdComposite({this.typeOid = oidRecord, required this.fields});
}

class ScratchBirdRange<T> {
  final T? lower;
  final T? upper;
  final bool lowerInclusive;
  final bool upperInclusive;
  final bool lowerInfinite;
  final bool upperInfinite;
  final bool empty;
  final int rangeOid;

  ScratchBirdRange({
    this.lower,
    this.upper,
    this.lowerInclusive = false,
    this.upperInclusive = false,
    this.lowerInfinite = false,
    this.upperInfinite = false,
    this.empty = false,
    required this.rangeOid,
  });
}

class ScratchBirdInterval {
  final int micros;
  final int days;
  final int months;
  ScratchBirdInterval(this.micros, this.days, this.months);
}

class RawValue {
  final int oid;
  final Uint8List data;
  RawValue(this.oid, this.data);
}

class ParamEncoding {
  final ParamValue param;
  final int oid;
  ParamEncoding(this.param, this.oid);
}

ParamEncoding encodeParam(dynamic value) {
  if (value == null) {
    return ParamEncoding(ParamValue(format: 1, isNull: true), 0);
  }
  if (value is RawValue) {
    return ParamEncoding(ParamValue(format: 1, data: value.data), value.oid);
  }
  if (value is ScratchBirdComposite) {
    final encoded = _encodeComposite(value);
    return ParamEncoding(
        ParamValue(format: 1, data: encoded.data), encoded.oid);
  }
  if (value is ScratchBirdRange) {
    final encoded = _encodeRange(value);
    return ParamEncoding(
        ParamValue(format: 1, data: encoded.data), encoded.oid);
  }
  if (value is ScratchBirdJsonb) {
    return ParamEncoding(
        ParamValue(format: 1, data: _lengthPrefixed(value.raw)), oidJsonb);
  }
  if (value is ScratchBirdJson) {
    return ParamEncoding(
        ParamValue(format: 1, data: _lengthPrefixed(value.raw)), oidJson);
  }
  if (value is ScratchBirdGeometry) {
    return ParamEncoding(
        ParamValue(format: 1, data: _lengthPrefixed(value.wkb)), oidPoint);
  }
  if (value is ScratchBirdInterval) {
    final buf = ByteData(16);
    buf.setInt64(0, value.micros, Endian.little);
    buf.setInt32(8, value.days, Endian.little);
    buf.setInt32(12, value.months, Endian.little);
    return ParamEncoding(
        ParamValue(format: 1, data: buf.buffer.asUint8List()), oidInterval);
  }
  if (value is ScratchBirdInet) {
    return ParamEncoding(
        ParamValue(
            format: 1,
            data:
                _lengthPrefixed(Uint8List.fromList(utf8.encode(value.value)))),
        oidInet);
  }
  if (value is ScratchBirdCidr) {
    return ParamEncoding(
        ParamValue(
            format: 1,
            data:
                _lengthPrefixed(Uint8List.fromList(utf8.encode(value.value)))),
        oidCidr);
  }
  if (value is ScratchBirdMacaddr) {
    return ParamEncoding(
        ParamValue(
            format: 1,
            data:
                _lengthPrefixed(Uint8List.fromList(utf8.encode(value.value)))),
        oidMacaddr);
  }
  if (value is bool) {
    return ParamEncoding(
        ParamValue(format: 1, data: Uint8List.fromList([value ? 1 : 0])),
        oidBool);
  }
  if (value is int) {
    if (value >= -32768 && value <= 32767) {
      final buf = ByteData(2);
      buf.setInt16(0, value, Endian.little);
      return ParamEncoding(
          ParamValue(format: 1, data: buf.buffer.asUint8List()), oidInt2);
    }
    if (value >= -2147483648 && value <= 2147483647) {
      final buf = ByteData(4);
      buf.setInt32(0, value, Endian.little);
      return ParamEncoding(
          ParamValue(format: 1, data: buf.buffer.asUint8List()), oidInt4);
    }
    final buf = ByteData(8);
    buf.setInt64(0, value, Endian.little);
    return ParamEncoding(
        ParamValue(format: 1, data: buf.buffer.asUint8List()), oidInt8);
  }
  if (value is double) {
    final buf = ByteData(8);
    buf.setFloat64(0, value, Endian.little);
    return ParamEncoding(
        ParamValue(format: 1, data: buf.buffer.asUint8List()), oidFloat8);
  }
  if (value is DateTime) {
    final base = DateTime.utc(2000, 1, 1);
    final micros = value.toUtc().difference(base).inMicroseconds;
    final buf = ByteData(8);
    buf.setInt64(0, micros, Endian.little);
    return ParamEncoding(
        ParamValue(format: 1, data: buf.buffer.asUint8List()), oidTimestamptz);
  }
  if (value is Uint8List) {
    return ParamEncoding(
        ParamValue(format: 1, data: _lengthPrefixed(value)), oidBytea);
  }
  if (value is List<double>) {
    return ParamEncoding(
        ParamValue(
            format: 1,
            data: _lengthPrefixed(
                Uint8List.fromList(utf8.encode(_formatVectorLiteral(value))))),
        oidVector);
  }
  if (value is List<int>) {
    final encodedValue = _formatArrayLiteral(value.toList());
    final payload = Utf8Encoder().convert(encodedValue);
    final prefixed = _lengthPrefixed(Uint8List.fromList(payload));
    return ParamEncoding(ParamValue(format: 1, data: prefixed), 0);
  }
  if (value is List<num>) {
    final allInt = value.every((v) => v is int);
    if (allInt) {
      final encodedValue =
          _formatArrayLiteral(value.map((v) => (v as int)).toList());
      final payload = Utf8Encoder().convert(encodedValue);
      final prefixed = _lengthPrefixed(Uint8List.fromList(payload));
      return ParamEncoding(ParamValue(format: 1, data: prefixed), 0);
    }
    final asDouble = value.map((v) => v.toDouble()).toList();
    final encodedValue = _formatVectorLiteral(asDouble);
    final payload = Utf8Encoder().convert(encodedValue);
    final prefixed = _lengthPrefixed(Uint8List.fromList(payload));
    return ParamEncoding(ParamValue(format: 1, data: prefixed), oidVector);
  }
  if (value is List) {
    return ParamEncoding(
        ParamValue(
            format: 1,
            data: _lengthPrefixed(
                Uint8List.fromList(utf8.encode(_formatArrayLiteral(value))))),
        0);
  }
  if (value is String) {
    final uuid = _uuidToBytes(value);
    if (uuid != null) {
      return ParamEncoding(ParamValue(format: 1, data: uuid), oidUuid);
    }
    return ParamEncoding(
        ParamValue(
            format: 1,
            data: _lengthPrefixed(Uint8List.fromList(utf8.encode(value)))),
        oidText);
  }
  if (value is Map || value is List) {
    final json = jsonEncode(value);
    return ParamEncoding(
        ParamValue(
            format: 1,
            data: _lengthPrefixed(Uint8List.fromList(utf8.encode(json)))),
        oidJson);
  }
  throw Exception('Unsupported parameter type');
}

dynamic decodeValue(int typeOid, Uint8List data, int format) {
  if (format == 0) {
    final text = utf8.decode(data);
    if (typeOid == 0) {
      return _parseUnknownText(text);
    }
    return text;
  }
  switch (typeOid) {
    case oidBool:
      return data.isNotEmpty && data[0] == 1;
    case oidInt2:
      return ByteData.sublistView(data).getInt16(0, Endian.little);
    case oidInt4:
      return ByteData.sublistView(data).getInt32(0, Endian.little);
    case oidInt8:
      return ByteData.sublistView(data).getInt64(0, Endian.little);
    case oidFloat4:
      return ByteData.sublistView(data).getFloat32(0, Endian.little);
    case oidFloat8:
      return ByteData.sublistView(data).getFloat64(0, Endian.little);
    case oidNumeric:
      return utf8.decode(_stripLength(data));
    case oidMoney:
      return ByteData.sublistView(data).getInt64(0, Endian.little);
    case oidText:
    case oidVarchar:
    case oidChar:
    case oidBpchar:
    case oidJson:
    case oidXml:
    case oidTsVector:
    case oidTsQuery:
      return utf8.decode(_stripLength(data));
    case oidJsonb:
      return ScratchBirdJsonb(_stripLength(data));
    case oidUuid:
      return _uuidFromBytes(data);
    case oidInet:
    case oidCidr:
    case oidMacaddr:
      return utf8.decode(_stripLength(data));
    case oidDate:
      final days = ByteData.sublistView(data).getInt32(0, Endian.little);
      return DateTime.utc(2000, 1, 1).add(Duration(days: days));
    case oidTime:
      return ByteData.sublistView(data).getInt64(0, Endian.little);
    case oidTimestamp:
    case oidTimestamptz:
      final micros = ByteData.sublistView(data).getInt64(0, Endian.little);
      return DateTime.utc(2000, 1, 1).add(Duration(microseconds: micros));
    case oidInterval:
      final bd = ByteData.sublistView(data);
      return ScratchBirdInterval(bd.getInt64(0, Endian.little),
          bd.getInt32(8, Endian.little), bd.getInt32(12, Endian.little));
    case oidInt4Range:
    case oidInt8Range:
    case oidNumRange:
    case oidTsRange:
    case oidTstzRange:
    case oidDateRange:
      return _decodeRange(typeOid, data);
    case oidPoint:
      return ScratchBirdGeometry(_stripLength(data));
    case oidVector:
      return _parseVectorLiteral(utf8.decode(_stripLength(data)));
    case oidRecord:
      return _decodeComposite(data);
    default:
      if (typeOid == 0) {
        return _decodeUnknownBinary(data);
      }
      return RawValue(typeOid, data);
  }
}

Uint8List _lengthPrefixed(Uint8List data) {
  final buf = ByteData(4 + data.length);
  buf.setUint32(0, data.length, Endian.little);
  buf.buffer.asUint8List().setAll(4, data);
  return buf.buffer.asUint8List();
}

Uint8List _stripLength(Uint8List data) {
  if (data.length < 4) return data;
  final len = ByteData.sublistView(data).getUint32(0, Endian.little);
  if (len > data.length - 4) return data;
  return data.sublist(4, 4 + len);
}

Uint8List? _uuidToBytes(String value) {
  final regex = RegExp(r'^[0-9a-fA-F-]{36}$');
  if (!regex.hasMatch(value)) return null;
  final hex = value.replaceAll('-', '');
  return Uint8List.fromList(hexToBytes(hex));
}

String _uuidFromBytes(Uint8List bytes) {
  final hex = bytes.map((b) => b.toRadixString(16).padLeft(2, '0')).join();
  return '${hex.substring(0, 8)}-${hex.substring(8, 12)}-${hex.substring(12, 16)}-${hex.substring(16, 20)}-${hex.substring(20)}';
}

List<int> hexToBytes(String hex) {
  final out = <int>[];
  for (var i = 0; i < hex.length; i += 2) {
    out.add(int.parse(hex.substring(i, i + 2), radix: 16));
  }
  return out;
}

ScratchBirdRange _decodeRange(int rangeOid, Uint8List data) {
  final flags = data[0];
  var offset = 4;

  ScratchBirdRange range = ScratchBirdRange(
    rangeOid: rangeOid,
    empty: (flags & rangeEmpty) != 0,
    lowerInclusive: (flags & rangeLbInc) != 0,
    upperInclusive: (flags & rangeUbInc) != 0,
    lowerInfinite: (flags & rangeLbInf) != 0,
    upperInfinite: (flags & rangeUbInf) != 0,
  );

  dynamic lower;
  dynamic upper;
  if ((flags & rangeLbInf) == 0) {
    final len = ByteData.sublistView(data, offset, offset + 4)
        .getUint32(0, Endian.little);
    offset += 4;
    lower = _decodeRangeBound(rangeOid, data.sublist(offset, offset + len));
    offset += len;
  }
  if ((flags & rangeUbInf) == 0) {
    final len = ByteData.sublistView(data, offset, offset + 4)
        .getUint32(0, Endian.little);
    offset += 4;
    upper = _decodeRangeBound(rangeOid, data.sublist(offset, offset + len));
  }

  return ScratchBirdRange(
    rangeOid: rangeOid,
    lower: lower,
    upper: upper,
    empty: range.empty,
    lowerInclusive: range.lowerInclusive,
    upperInclusive: range.upperInclusive,
    lowerInfinite: range.lowerInfinite,
    upperInfinite: range.upperInfinite,
  );
}

dynamic _decodeRangeBound(int rangeOid, Uint8List data) {
  switch (rangeOid) {
    case oidInt4Range:
      return ByteData.sublistView(data).getInt32(0, Endian.little);
    case oidInt8Range:
      return ByteData.sublistView(data).getInt64(0, Endian.little);
    case oidNumRange:
      return utf8.decode(_stripLength(data));
    case oidDateRange:
      final days = ByteData.sublistView(data).getInt32(0, Endian.little);
      return DateTime.utc(2000, 1, 1).add(Duration(days: days));
    case oidTsRange:
    case oidTstzRange:
      final micros = ByteData.sublistView(data).getInt64(0, Endian.little);
      return DateTime.utc(2000, 1, 1).add(Duration(microseconds: micros));
    default:
      return data;
  }
}

class _EncodedComposite {
  final Uint8List data;
  final int oid;
  _EncodedComposite(this.data, this.oid);
}

_EncodedComposite _encodeComposite(ScratchBirdComposite composite) {
  final typeOid = composite.typeOid == 0 ? oidRecord : composite.typeOid;
  final chunks = <int>[];
  final header = ByteData(4);
  header.setInt32(0, composite.fields.length, Endian.little);
  chunks.addAll(header.buffer.asUint8List());
  for (final field in composite.fields) {
    var fieldOid = field.oid ?? 0;
    Uint8List? fieldData;
    if (field.raw != null) {
      fieldData = field.raw;
    } else if (field.value != null) {
      final encoded = encodeParam(field.value);
      if (fieldOid == 0) {
        fieldOid = encoded.oid;
      } else if (field.value is int &&
          _isNumericOid(fieldOid) &&
          !encoded.param.isNull) {
        fieldData = _encodeIntForOid(field.value as int, fieldOid);
      }
      if (fieldData == null && !encoded.param.isNull) {
        fieldData = encoded.param.data ?? Uint8List(0);
      }
    }
    if (fieldOid == 0) {
      throw Exception('composite field OID is required');
    }
    final oidBuf = ByteData(4);
    oidBuf.setUint32(0, fieldOid, Endian.little);
    chunks.addAll(oidBuf.buffer.asUint8List());
    if (fieldData == null) {
      final lenBuf = ByteData(4);
      lenBuf.setInt32(0, -1, Endian.little);
      chunks.addAll(lenBuf.buffer.asUint8List());
      continue;
    }
    final lenBuf = ByteData(4);
    lenBuf.setInt32(0, fieldData.length, Endian.little);
    chunks.addAll(lenBuf.buffer.asUint8List());
    chunks.addAll(fieldData);
  }
  return _EncodedComposite(Uint8List.fromList(chunks), typeOid);
}

ScratchBirdComposite _decodeComposite(Uint8List data) {
  if (data.length < 4) {
    return ScratchBirdComposite(fields: []);
  }
  final count = ByteData.sublistView(data).getInt32(0, Endian.little);
  var offset = 4;
  final fields = <ScratchBirdCompositeField>[];
  for (var i = 0; i < count; i++) {
    if (offset + 8 > data.length) break;
    final oid = ByteData.sublistView(data, offset, offset + 4)
        .getUint32(0, Endian.little);
    offset += 4;
    final len = ByteData.sublistView(data, offset, offset + 4)
        .getInt32(0, Endian.little);
    offset += 4;
    if (len < 0) {
      fields.add(ScratchBirdCompositeField(oid: oid, value: null, raw: null));
      continue;
    }
    if (offset + len > data.length) break;
    final raw = data.sublist(offset, offset + len);
    offset += len;
    final value = decodeValue(oid, raw, 1);
    fields.add(ScratchBirdCompositeField(oid: oid, value: value, raw: raw));
  }
  return ScratchBirdComposite(fields: fields);
}

class _EncodedRange {
  final Uint8List data;
  final int oid;
  _EncodedRange(this.data, this.oid);
}

_EncodedRange _encodeRange(ScratchBirdRange range) {
  final oid = _resolveRangeOid(range);
  var flags = 0;
  if (range.empty) flags |= rangeEmpty;
  if (range.lowerInclusive) flags |= rangeLbInc;
  if (range.upperInclusive) flags |= rangeUbInc;
  if (range.lowerInfinite) flags |= rangeLbInf;
  if (range.upperInfinite) flags |= rangeUbInf;
  final chunks = <int>[flags, 0, 0, 0];
  if (!range.empty && !range.lowerInfinite) {
    final bound = _encodeRangeBound(oid, range.lower);
    final lenBuf = ByteData(4);
    lenBuf.setInt32(0, bound.length, Endian.little);
    chunks.addAll(lenBuf.buffer.asUint8List());
    chunks.addAll(bound);
  }
  if (!range.empty && !range.upperInfinite) {
    final bound = _encodeRangeBound(oid, range.upper);
    final lenBuf = ByteData(4);
    lenBuf.setInt32(0, bound.length, Endian.little);
    chunks.addAll(lenBuf.buffer.asUint8List());
    chunks.addAll(bound);
  }
  return _EncodedRange(Uint8List.fromList(chunks), oid);
}

int _resolveRangeOid(ScratchBirdRange range) {
  if (range.rangeOid != 0) {
    return range.rangeOid;
  }
  final sample = range.lower ?? range.upper;
  if (sample == null) {
    throw Exception('range type cannot be inferred from empty bounds');
  }
  if (sample is int) {
    if (sample >= -2147483648 && sample <= 2147483647) {
      return oidInt4Range;
    }
    return oidInt8Range;
  }
  if (sample is num) {
    return oidNumRange;
  }
  if (sample is DateTime) {
    return oidTstzRange;
  }
  throw Exception('unsupported range bound type');
}

Uint8List _encodeRangeBound(int oid, dynamic value) {
  switch (oid) {
    case oidInt4Range:
      if (value is int) {
        final buf = ByteData(4);
        buf.setInt32(0, value, Endian.little);
        return buf.buffer.asUint8List();
      }
      throw Exception('int4range requires int bounds');
    case oidInt8Range:
      if (value is int) {
        final buf = ByteData(8);
        buf.setInt64(0, value, Endian.little);
        return buf.buffer.asUint8List();
      }
      throw Exception('int8range requires int bounds');
    case oidNumRange:
      return _lengthPrefixed(Uint8List.fromList(utf8.encode(value.toString())));
    case oidDateRange:
      if (value is DateTime) {
        final base = DateTime.utc(2000, 1, 1);
        final days = value.toUtc().difference(base).inDays;
        final buf = ByteData(4);
        buf.setInt32(0, days, Endian.little);
        return buf.buffer.asUint8List();
      }
      throw Exception('daterange requires DateTime bounds');
    case oidTsRange:
    case oidTstzRange:
      if (value is DateTime) {
        final base = DateTime.utc(2000, 1, 1);
        final micros = value.toUtc().difference(base).inMicroseconds;
        final buf = ByteData(8);
        buf.setInt64(0, micros, Endian.little);
        return buf.buffer.asUint8List();
      }
      throw Exception('tsrange requires DateTime bounds');
    default:
      throw Exception('unsupported range type');
  }
}

bool _isNumericOid(int oid) {
  return oid == oidInt2 || oid == oidInt4 || oid == oidInt8;
}

Uint8List _encodeIntForOid(int value, int oid) {
  switch (oid) {
    case oidInt2:
      final buf = ByteData(2);
      buf.setInt16(0, value, Endian.little);
      return buf.buffer.asUint8List();
    case oidInt4:
      final buf = ByteData(4);
      buf.setInt32(0, value, Endian.little);
      return buf.buffer.asUint8List();
    case oidInt8:
      final buf = ByteData(8);
      buf.setInt64(0, value, Endian.little);
      return buf.buffer.asUint8List();
    default:
      return Uint8List(0);
  }
}

dynamic _decodeUnknownBinary(Uint8List data) {
  if (data.length >= 4) {
    final stripped = _stripLength(data);
    if (stripped.isNotEmpty && _looksLikeText(stripped)) {
      final text = utf8.decode(stripped);
      return _parseUnknownText(text);
    }
  }
  final trimmed = _stripTrailingNulls(data);
  if (trimmed.isNotEmpty && _looksLikeText(trimmed)) {
    final text = utf8.decode(trimmed);
    return _parseUnknownText(text);
  }
  switch (data.length) {
    case 1:
      return data[0];
    case 2:
      return ByteData.sublistView(data).getInt16(0, Endian.little);
    case 4:
      return ByteData.sublistView(data).getInt32(0, Endian.little);
    case 8:
      return ByteData.sublistView(data).getInt64(0, Endian.little);
    case 16:
      return _uuidFromBytes(data);
    default:
      return data;
  }
}

dynamic _parseUnknownText(String text) {
  final trimmed = text.trim();
  if (trimmed.isEmpty) {
    return text;
  }
  if (_looksLikeArrayLiteral(trimmed)) {
    return _parseArrayLiteral(trimmed);
  }
  if (trimmed == 'true' || trimmed == 'false') {
    return trimmed == 'true';
  }
  final asInt = int.tryParse(trimmed);
  if (asInt != null) {
    return asInt;
  }
  final asDouble = double.tryParse(trimmed);
  if (asDouble != null) {
    return asDouble;
  }
  return text;
}

bool _looksLikeText(Uint8List data) {
  for (final b in data) {
    if (b == 0x09 || b == 0x0A || b == 0x0D) {
      continue;
    }
    if (b < 0x20 || b > 0x7E) {
      return false;
    }
  }
  return true;
}

Uint8List _stripTrailingNulls(Uint8List data) {
  var end = data.length;
  while (end > 0 && data[end - 1] == 0) {
    end -= 1;
  }
  if (end == data.length) {
    return data;
  }
  return data.sublist(0, end);
}

bool _looksLikeArrayLiteral(String text) {
  return text.startsWith('{') && text.endsWith('}');
}

List<dynamic> _parseArrayLiteral(String text) {
  var trimmed = text.trim();
  if (trimmed.isEmpty || trimmed == '{}') {
    return [];
  }
  if (trimmed.startsWith('{') && trimmed.endsWith('}')) {
    trimmed = trimmed.substring(1, trimmed.length - 1);
  }
  return _splitArrayItems(trimmed);
}

List<dynamic> _splitArrayItems(String text) {
  final items = <dynamic>[];
  var depth = 0;
  final buffer = StringBuffer();
  for (var i = 0; i < text.length; i++) {
    final ch = text[i];
    if (ch == '{') {
      depth += 1;
      buffer.write(ch);
    } else if (ch == '}') {
      depth = depth > 0 ? depth - 1 : 0;
      buffer.write(ch);
    } else if (ch == ',' && depth == 0) {
      items.add(_parseArrayItem(buffer.toString()));
      buffer.clear();
    } else {
      buffer.write(ch);
    }
  }
  if (buffer.isNotEmpty || text.isNotEmpty) {
    items.add(_parseArrayItem(buffer.toString()));
  }
  return items;
}

dynamic _parseArrayItem(String raw) {
  final token = raw.trim();
  if (token.isEmpty) {
    return '';
  }
  if (token.toUpperCase() == 'NULL') {
    return null;
  }
  if (token.startsWith('{') && token.endsWith('}')) {
    return _parseArrayLiteral(token);
  }
  if (token.startsWith('[') && token.endsWith(']')) {
    return _parseVectorLiteral(token);
  }
  if (token == 'true' || token == 'false') {
    return token == 'true';
  }
  final asInt = int.tryParse(token);
  if (asInt != null) {
    return asInt;
  }
  final asDouble = double.tryParse(token);
  if (asDouble != null) {
    return asDouble;
  }
  return token;
}

String _formatArrayLiteral(List values) {
  final items = values.map((v) => _formatArrayItem(v)).toList();
  return '{${items.join(',')}}';
}

String _formatArrayItem(dynamic value) {
  if (value == null) {
    return 'NULL';
  }
  if (value is String) {
    final escaped = value.replaceAll('"', r'\"');
    return '"$escaped"';
  }
  if (value is List<double>) {
    return _formatVectorLiteral(value);
  }
  if (value is List<num>) {
    return _formatVectorLiteral(value.map((v) => v.toDouble()).toList());
  }
  if (value is List) {
    return _formatArrayLiteral(value);
  }
  return value.toString();
}

List<double> _parseVectorLiteral(String text) {
  var trimmed = text.trim();
  if (trimmed.startsWith('[') && trimmed.endsWith(']')) {
    trimmed = trimmed.substring(1, trimmed.length - 1);
  }
  if (trimmed.isEmpty) {
    return [];
  }
  return trimmed.split(',').map((part) {
    final val = double.tryParse(part.trim());
    return val ?? 0;
  }).toList();
}

String _formatVectorLiteral(List<double> values) {
  final parts = values.map((v) => v.isFinite ? v.toString() : '0').toList();
  return '[${parts.join(',')}]';
}
