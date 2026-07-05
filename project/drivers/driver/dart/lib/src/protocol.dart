// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import 'dart:convert';
import 'dart:typed_data';

import 'errors.dart';

const int protocolMagic = 0x50574253;
const int protocolMajor = 1;
const int protocolMinor = 1;
const int headerSize = 40;
const int maxMessageSize = 1024 * 1024 * 1024;

class MessageType {
  static const int startup = 0x01;
  static const int authResponse = 0x02;
  static const int query = 0x03;
  static const int parse = 0x04;
  static const int bind = 0x05;
  static const int describe = 0x06;
  static const int execute = 0x07;
  static const int close = 0x08;
  static const int sync = 0x09;
  static const int flush = 0x0a;
  static const int cancel = 0x0b;
  static const int terminate = 0x0c;
  static const int copyData = 0x0d;
  static const int copyDone = 0x0e;
  static const int copyFail = 0x0f;
  static const int sblrExecute = 0x10;
  static const int subscribe = 0x11;
  static const int unsubscribe = 0x12;
  static const int federatedQuery = 0x13;
  static const int streamControl = 0x14;
  static const int txnBegin = 0x15;
  static const int txnCommit = 0x16;
  static const int txnRollback = 0x17;
  static const int txnSavepoint = 0x18;
  static const int txnRelease = 0x19;
  static const int txnRollbackTo = 0x1a;
  static const int ping = 0x1b;
  static const int setOption = 0x1c;
  static const int clusterAuth = 0x1d;
  static const int attachCreate = 0x1e;
  static const int attachDetach = 0x1f;
  static const int attachList = 0x20;

  static const int authRequest = 0x40;
  static const int authOk = 0x41;
  static const int authContinue = 0x42;
  static const int ready = 0x43;
  static const int rowDescription = 0x44;
  static const int dataRow = 0x45;
  static const int commandComplete = 0x46;
  static const int emptyQuery = 0x47;
  static const int error = 0x48;
  static const int notice = 0x49;
  static const int parseComplete = 0x4a;
  static const int bindComplete = 0x4b;
  static const int closeComplete = 0x4c;
  static const int portalSuspended = 0x4d;
  static const int noData = 0x4e;
  static const int parameterStatus = 0x4f;
  static const int parameterDescription = 0x50;
  static const int copyInResponse = 0x51;
  static const int copyOutResponse = 0x52;
  static const int copyBothResponse = 0x53;
  static const int notification = 0x54;
  static const int functionResult = 0x55;
  static const int negotiateVersion = 0x56;
  static const int sblrCompiled = 0x57;
  static const int queryPlan = 0x58;
  static const int streamReady = 0x59;
  static const int streamData = 0x5a;
  static const int streamEnd = 0x5b;
  static const int txnStatus = 0x5c;
  static const int pong = 0x5d;
  static const int clusterAuthOk = 0x5e;
  static const int federatedResult = 0x5f;
  static const int heartbeat = 0x80;
  static const int extension = 0x81;
}

const int authOkMethod = 0;
const int authPasswordMethod = 1;
const int authMd5Method = 2;
const int authScramSha256Method = 3;
const int authScramSha512Method = 4;
const int authTokenMethod = 5;
const int authPeerMethod = 6;
const int authReattachMethod = 7;

const int featureCompression = 1 << 0;
const int featureStreaming = 1 << 1;
const int featureSblr = 1 << 2;
const int featureNotifications = 1 << 4;
const int featureQueryPlan = 1 << 5;
const int featureBatch = 1 << 6;
const int featurePipeline = 1 << 7;
const int featureBinaryCopy = 1 << 8;
const int featureSavepoints = 1 << 9;

const int nativeRowsetTypeText = 1;
const int nativeRowsetTypeInt64 = 2;
const int nativeRowsetTypeBoolean = 3;
const int nativeRowsetTypeInt32 = 4;
const int nativeRowsetTypeUint64 = 5;
const int nativeRowsetTypeReal64 = 6;
const int nativeRowsetTypeBinary = 7;

const int queryFlagDescribeOnly = 0x01;
const int queryFlagNoPortal = 0x02;
const int queryFlagBinaryResult = 0x04;
const int queryFlagIncludePlan = 0x08;
const int queryFlagReturnSblr = 0x10;
const int queryFlagNoCache = 0x20;

const int isolationReadUncommitted = 0;
const int isolationReadCommitted = 1;
const int isolationRepeatableRead = 2;
const int isolationSerializable = 3;

const int readCommittedModeDefault = 0;
const int readCommittedModeReadConsistency = 1;
const int readCommittedModeRecordVersion = 2;
const int readCommittedModeNoRecordVersion = 3;

const int txnFlagHasIsolation = 0x0001;
const int txnFlagHasAccess = 0x0002;
const int txnFlagHasDeferrable = 0x0004;
const int txnFlagHasWait = 0x0008;
const int txnFlagHasTimeout = 0x0010;
const int txnFlagHasAutocommit = 0x0020;
const int txnFlagHasReadCommittedMode = 0x0100;

const int streamStart = 0;
const int streamPause = 1;
const int streamResume = 2;
const int streamCancel = 3;
const int streamAck = 4;

const int subscribeTypeChannel = 0;
const int subscribeTypeTable = 1;
const int subscribeTypeQuery = 2;
const int subscribeTypeEvent = 3;

class MessageHeader {
  final int type;
  final int flags;
  final int length;
  final int sequence;
  final Uint8List attachmentId;
  final int txnId;

  MessageHeader({
    required this.type,
    required this.flags,
    required this.length,
    required this.sequence,
    required this.attachmentId,
    required this.txnId,
  });
}

class ScratchBirdMessage {
  final MessageHeader header;
  final Uint8List payload;

  ScratchBirdMessage(this.header, this.payload);
}

class ProtocolError {
  final String severity;
  final String sqlState;
  final String message;
  final String detail;
  final String hint;
  final int? code;

  const ProtocolError({
    required this.severity,
    required this.sqlState,
    required this.message,
    required this.detail,
    required this.hint,
    required this.code,
  });
}

ProtocolError parseErrorMessage(Uint8List payload) {
  var offset = 0;
  var severity = '';
  var sqlState = '';
  var message = '';
  var detail = '';
  var hint = '';
  int? code;

  while (offset < payload.length) {
    final field = payload[offset];
    offset += 1;
    if (field == 0) {
      break;
    }
    final start = offset;
    while (offset < payload.length && payload[offset] != 0) {
      offset += 1;
    }
    if (offset >= payload.length) {
      break;
    }
    final value = utf8.decode(
      payload.sublist(start, offset),
      allowMalformed: true,
    );
    offset += 1;
    switch (String.fromCharCode(field)) {
      case 'S':
        severity = value;
        break;
      case 'C':
        sqlState = value;
        break;
      case 'M':
        message = value;
        break;
      case 'D':
        detail = value;
        break;
      case 'H':
        hint = value;
        break;
      case 'N':
        code = int.tryParse(value.trim());
        break;
    }
  }

  return ProtocolError(
    severity: severity,
    sqlState: sqlState,
    message: message,
    detail: detail,
    hint: hint,
    code: code,
  );
}

String formatProtocolErrorMessage(
  ProtocolError error, {
  required String fallbackMessage,
}) {
  var text = error.message.isNotEmpty ? error.message : fallbackMessage;
  if (error.sqlState.isNotEmpty) {
    text = '[${error.sqlState}] $text';
  }
  if (error.detail.isNotEmpty) {
    text = '$text Detail: ${error.detail}';
  }
  if (error.hint.isNotEmpty) {
    text = '$text Hint: ${error.hint}';
  }
  return text;
}

Uint8List encodeMessage(MessageHeader header, Uint8List payload) {
  final buffer = BytesBuilder();
  final hdr = ByteData(headerSize);
  hdr.setUint32(0, protocolMagic, Endian.little);
  hdr.setUint8(4, protocolMajor);
  hdr.setUint8(5, protocolMinor);
  hdr.setUint8(6, header.type);
  hdr.setUint8(7, header.flags);
  hdr.setUint32(8, payload.length, Endian.little);
  hdr.setUint32(12, header.sequence, Endian.little);
  hdr.buffer.asUint8List(16, 16).setAll(0, header.attachmentId);
  hdr.setUint64(32, header.txnId, Endian.little);
  buffer.add(hdr.buffer.asUint8List());
  buffer.add(payload);
  return buffer.toBytes();
}

MessageHeader decodeHeader(Uint8List data) {
  if (data.length != headerSize) {
    throw const ScratchBirdProtocolException('Invalid header length');
  }
  final header = ByteData.sublistView(data);
  final magic = header.getUint32(0, Endian.little);
  if (magic != protocolMagic) {
    throw const ScratchBirdProtocolException('Invalid protocol magic');
  }
  final major = header.getUint8(4);
  final minor = header.getUint8(5);
  if (major != protocolMajor || minor != protocolMinor) {
    throw const ScratchBirdProtocolException('Unsupported protocol version');
  }
  final length = header.getUint32(8, Endian.little);
  if (length > maxMessageSize) {
    throw const ScratchBirdProtocolException('Payload too large');
  }
  return MessageHeader(
    type: header.getUint8(6),
    flags: header.getUint8(7),
    length: length,
    sequence: header.getUint32(12, Endian.little),
    attachmentId: data.sublist(16, 32),
    txnId: header.getUint64(32, Endian.little),
  );
}

({int method, Uint8List data}) parseAuthRequest(Uint8List payload) {
  if (payload.length < 4) {
    throw const ScratchBirdConnectionException(
      'Auth request truncated',
      sqlState: '08P01',
    );
  }
  return (method: payload[0], data: payload.sublist(4));
}

({int method, int stage, Uint8List data}) parseAuthContinue(Uint8List payload) {
  if (payload.length < 8) {
    throw const ScratchBirdConnectionException(
      'Auth continue truncated',
      sqlState: '08P01',
    );
  }
  final dataLen = ByteData.sublistView(
    payload,
    4,
    8,
  ).getUint32(0, Endian.little);
  if (8 + dataLen > payload.length) {
    throw const ScratchBirdConnectionException(
      'Auth continue truncated',
      sqlState: '08P01',
    );
  }
  return (
    method: payload[0],
    stage: payload[1],
    data: payload.sublist(8, 8 + dataLen),
  );
}

({Uint8List sessionId, Uint8List serverInfo}) parseAuthOk(Uint8List payload) {
  if (payload.length < 20) {
    throw const ScratchBirdConnectionException(
      'Auth ok truncated',
      sqlState: '08P01',
    );
  }
  final infoLen = ByteData.sublistView(
    payload,
    16,
    20,
  ).getUint32(0, Endian.little);
  if (20 + infoLen > payload.length) {
    throw const ScratchBirdConnectionException(
      'Auth ok truncated',
      sqlState: '08P01',
    );
  }
  return (
    sessionId: payload.sublist(0, 16),
    serverInfo: payload.sublist(20, 20 + infoLen),
  );
}

({int status, int txnId, int visibility}) parseReady(Uint8List payload) {
  if (payload.length >= 76) {
    final statusByte = payload[56];
    if (statusByte == 0x49 ||
        statusByte == 0x54 ||
        statusByte == 0x45 ||
        statusByte == 0x52 ||
        statusByte == 0x41) {
      final txnId = ByteData.sublistView(
        payload,
        48,
        56,
      ).getUint64(0, Endian.little);
      final status = statusByte == 0x54 || statusByte == 0x45 ? 1 : 0;
      return (status: status, txnId: txnId, visibility: txnId);
    }
  }
  if (payload.length < 20) {
    throw const ScratchBirdProtocolException('Ready truncated');
  }
  final status = payload[0];
  final txnId = ByteData.sublistView(
    payload,
    4,
    12,
  ).getUint64(0, Endian.little);
  final visibility = ByteData.sublistView(
    payload,
    12,
    20,
  ).getUint64(0, Endian.little);
  return (status: status, txnId: txnId, visibility: visibility);
}

({String status, int txnId}) parseTxnStatus(Uint8List payload) {
  if (payload.length < 12) {
    throw const ScratchBirdProtocolException('Txn status truncated');
  }
  final status = String.fromCharCode(payload[0]);
  final txnId = ByteData.sublistView(
    payload,
    4,
    12,
  ).getUint64(0, Endian.little);
  return (status: status, txnId: txnId);
}

Uint8List buildStartupPayload(int features, Map<String, String> params) {
  final paramBytes = buildP1ParamList(params);
  final payload = ByteData(88 + paramBytes.length);
  var offset = 0;
  payload.setUint16(
    offset,
    (protocolMajor << 8) | protocolMinor,
    Endian.little,
  );
  offset += 2;
  payload.setUint16(
    offset,
    (protocolMajor << 8) | protocolMinor,
    Endian.little,
  );
  offset += 2;
  payload.setUint32(offset, 0, Endian.little);
  offset += 4;
  payload.setUint64(offset, features, Endian.little);
  offset += 8;
  payload.setUint64(offset, 0, Endian.little);
  offset += 8;
  payload.setUint64(offset, 0, Endian.little);
  offset += 8;
  offset += 16 * 3;
  payload.setUint32(offset, params.length, Endian.little);
  offset += 4;
  final bytes = payload.buffer.asUint8List();
  bytes.setAll(offset, paramBytes);
  offset += paramBytes.length;
  ByteData.sublistView(bytes).setUint32(offset, 0, Endian.little);
  return bytes;
}

Uint8List buildP1ParamList(Map<String, String> params) {
  final bytes = BytesBuilder(copy: false);
  final keys = params.keys.toList()..sort();
  for (final key in keys) {
    _appendP1LengthPrefixedString(bytes, key);
    bytes.add(_u16(1));
    final valueBytes = utf8.encode(params[key] ?? '');
    bytes.add(_u32(valueBytes.length));
    bytes.add(valueBytes);
  }
  return bytes.toBytes();
}

void _appendP1LengthPrefixedString(BytesBuilder bytes, String value) {
  final valueBytes = utf8.encode(value);
  bytes.add(_u32(valueBytes.length));
  bytes.add(valueBytes);
}

Uint8List buildQueryPayload(String sql, int flags, int maxRows, int timeoutMs) {
  final sqlBytes = Uint8List.fromList(utf8.encode(sql));
  final payload = ByteData(12 + sqlBytes.length);
  payload.setUint32(0, flags, Endian.little);
  payload.setUint32(4, maxRows, Endian.little);
  payload.setUint32(8, timeoutMs, Endian.little);
  final bytes = payload.buffer.asUint8List();
  bytes.setAll(12, sqlBytes);
  return bytes;
}

Uint8List buildParsePayload(
  String statementName,
  String sql,
  List<int> paramTypes,
) {
  final nameBytes = Uint8List.fromList(utf8.encode(statementName));
  final sqlBytes = Uint8List.fromList(utf8.encode(sql));
  final payload = BytesBuilder();
  payload.add(_u32(nameBytes.length));
  payload.add(nameBytes);
  payload.add(_u32(sqlBytes.length));
  payload.add(sqlBytes);
  payload.add(_u16(paramTypes.length));
  payload.add(_u16(0));
  for (final oid in paramTypes) {
    payload.add(_u32(oid));
  }
  return payload.toBytes();
}

Uint8List buildBindPayload(
  String portalName,
  String statementName,
  List<ParamValue> params,
  List<int> resultFormats,
) {
  final portalBytes = Uint8List.fromList(utf8.encode(portalName));
  final stmtBytes = Uint8List.fromList(utf8.encode(statementName));
  final payload = BytesBuilder();
  payload.add(_u32(portalBytes.length));
  payload.add(portalBytes);
  payload.add(_u32(stmtBytes.length));
  payload.add(stmtBytes);
  payload.add(_u16(params.length));
  for (final param in params) {
    payload.add(_u16(param.format));
  }
  payload.add(_u16(params.length));
  payload.add(_u16(0));
  for (final param in params) {
    if (param.isNull) {
      payload.add(_u32(0xffffffff));
    } else {
      final data = param.data ?? Uint8List(0);
      payload.add(_u32(data.length));
      payload.add(data);
    }
  }
  payload.add(_u16(resultFormats.length));
  for (final fmt in resultFormats) {
    payload.add(_u16(fmt));
  }
  return payload.toBytes();
}

Uint8List buildDescribePayload(int kind, String name) {
  final nameBytes = Uint8List.fromList(utf8.encode(name));
  final payload = BytesBuilder();
  payload.add(Uint8List.fromList([kind, 0, 0, 0]));
  payload.add(_u32(nameBytes.length));
  payload.add(nameBytes);
  return payload.toBytes();
}

Uint8List buildExecutePayload(String portalName, int maxRows) {
  final portalBytes = Uint8List.fromList(utf8.encode(portalName));
  final payload = BytesBuilder();
  payload.add(_u32(portalBytes.length));
  payload.add(portalBytes);
  payload.add(_u32(maxRows));
  return payload.toBytes();
}

Uint8List copyTextRowsToNativeFrame(List<int> data, {List<int>? columnTypes}) {
  final bytes = Uint8List.fromList(data);
  if (bytes.length >= 4 &&
      bytes[0] == 0x53 &&
      bytes[1] == 0x42 &&
      bytes[2] == 0x4e &&
      bytes[3] == 0x52) {
    return bytes;
  }
  final text = utf8.decode(bytes);
  final lines = text
      .split('\n')
      .map(
        (line) =>
            line.endsWith('\r') ? line.substring(0, line.length - 1) : line,
      )
      .where((line) => line.trim().isNotEmpty)
      .toList();
  if (lines.isEmpty) {
    throw const ScratchBirdProtocolException('COPY input contains no rows');
  }

  final first = lines.first;
  if (first.contains(';') && first.contains('=')) {
    var columns = <String>[];
    final rows = <List<Object?>>[];
    for (final line in lines) {
      final fields = <({String name, Object? value})>[];
      for (final item in line.split(';')) {
        if (item.isEmpty) {
          continue;
        }
        final sep = item.indexOf('=');
        if (sep <= 0) {
          throw const ScratchBirdProtocolException(
            'malformed canonical COPY field',
          );
        }
        final name = item.substring(0, sep);
        final value = item.substring(sep + 1);
        fields.add((
          name: name,
          value: value.toUpperCase() == 'NULL' ? null : value,
        ));
      }
      if (fields.isEmpty) {
        continue;
      }
      final names = fields.map((field) => field.name).toList();
      if (columns.isEmpty) {
        columns = names;
      } else if (!_sameStringList(columns, names)) {
        throw const ScratchBirdProtocolException(
          'COPY input changed row shape mid-stream',
        );
      }
      rows.add(fields.map((field) => field.value).toList());
    }
    return buildNativeRowsetPayload(columns, rows, columnTypes: columnTypes);
  }

  final columns = _splitCopyCsvLine(
    first,
  ).map((value) => value.trim()).toList();
  if (columns.isEmpty || columns.any((value) => value.isEmpty)) {
    throw const ScratchBirdProtocolException(
      'CSV COPY input requires a non-empty header row',
    );
  }
  final rows = <List<Object?>>[];
  for (final line in lines.skip(1)) {
    final values = _splitCopyCsvLine(line);
    if (values.length != columns.length) {
      throw const ScratchBirdProtocolException('CSV COPY row shape mismatch');
    }
    rows.add(
      values
          .map<Object?>(
            (value) =>
                value.isEmpty || value.toUpperCase() == 'NULL' ? null : value,
          )
          .toList(),
    );
  }
  if (rows.isEmpty) {
    throw const ScratchBirdProtocolException(
      'CSV COPY input contains no data rows',
    );
  }
  return buildNativeRowsetPayload(columns, rows, columnTypes: columnTypes);
}

Uint8List buildNativeRowsetPayload(
  List<String> columns,
  List<List<Object?>> rows, {
  List<int>? columnTypes,
}) {
  if (rows.isEmpty) {
    throw const ScratchBirdProtocolException(
      'native rowset requires at least one row',
    );
  }
  if (columns.isEmpty || columns.any((column) => column.isEmpty)) {
    throw const ScratchBirdProtocolException(
      'native rowset requires non-empty column names',
    );
  }
  for (final row in rows) {
    if (row.length != columns.length) {
      throw const ScratchBirdProtocolException(
        'native rowset row shape mismatch',
      );
    }
  }
  final normalizedTypes = columnTypes == null
      ? inferNativeRowsetColumnTypes(rows)
      : List<int>.from(columnTypes);
  if (normalizedTypes.length != columns.length) {
    throw const ScratchBirdProtocolException(
      'native rowset column/type shape mismatch',
    );
  }

  final nullBitmapBytes = (columns.length + 7) ~/ 8;
  final payload = BytesBuilder(copy: false);
  payload.add(Uint8List.fromList([0x53, 0x42, 0x4e, 0x52]));
  payload.add(_u16(2));
  payload.add(_u16(0));
  payload.add(_u64(rows.length));
  payload.add(_u32(columns.length));
  payload.add(Uint8List.fromList(normalizedTypes));
  for (final column in columns) {
    final encoded = utf8.encode(column);
    payload.add(_u32(encoded.length));
    payload.add(Uint8List.fromList(encoded));
  }
  for (final row in rows) {
    final nullBitmap = Uint8List(nullBitmapBytes);
    final values = BytesBuilder(copy: false);
    for (var index = 0; index < row.length; index += 1) {
      final value = row[index];
      if (value == null) {
        nullBitmap[index ~/ 8] |= 1 << (index % 8);
        continue;
      }
      switch (normalizedTypes[index]) {
        case nativeRowsetTypeInt64:
          values.add(_i64(int.parse(value.toString())));
          break;
        case nativeRowsetTypeBoolean:
          values.add(
            Uint8List.fromList([_truthyNativeRowsetBoolean(value) ? 1 : 0]),
          );
          break;
        case nativeRowsetTypeInt32:
          values.add(_i32(int.parse(value.toString())));
          break;
        case nativeRowsetTypeUint64:
          values.add(_u64(int.parse(value.toString())));
          break;
        case nativeRowsetTypeReal64:
          values.add(_f64(double.parse(value.toString())));
          break;
        case nativeRowsetTypeBinary:
          final raw = value is Uint8List
              ? value
              : Uint8List.fromList(utf8.encode(value.toString()));
          values.add(_u32(raw.length));
          values.add(raw);
          break;
        case nativeRowsetTypeText:
          final encoded = utf8.encode(value.toString());
          values.add(_u32(encoded.length));
          values.add(Uint8List.fromList(encoded));
          break;
        default:
          throw ScratchBirdProtocolException(
            'unsupported native rowset type ${normalizedTypes[index]}',
          );
      }
    }
    payload.add(nullBitmap);
    payload.add(values.toBytes());
  }
  return payload.toBytes();
}

List<int> inferNativeRowsetColumnTypes(List<List<Object?>> rows) {
  if (rows.isEmpty) {
    return <int>[];
  }
  final columnCount = rows.first.length;
  final types = List<int>.filled(columnCount, nativeRowsetTypeText);
  for (var column = 0; column < columnCount; column += 1) {
    final values = rows
        .map((row) => row[column])
        .where((value) => value != null)
        .toList();
    if (values.isEmpty) {
      continue;
    }
    if (values.every((value) => value is Uint8List)) {
      types[column] = nativeRowsetTypeBinary;
      continue;
    }
    final textValues = values.map((value) => value.toString()).toList();
    final lowerValues = textValues
        .map((value) => value.trim().toLowerCase())
        .toList();
    if (lowerValues.every((value) => value == 'true' || value == 'false')) {
      types[column] = nativeRowsetTypeBoolean;
      continue;
    }
    if (textValues.every(
      (value) => _losslessInt(value, -2147483648, 2147483647),
    )) {
      types[column] = nativeRowsetTypeInt32;
      continue;
    }
    if (textValues.every(
      (value) => _losslessInt(value, -9223372036854775808, 9223372036854775807),
    )) {
      types[column] = nativeRowsetTypeInt64;
      continue;
    }
    if (textValues.every((value) => _losslessUint64(value))) {
      types[column] = nativeRowsetTypeUint64;
      continue;
    }
    if (textValues.every(_losslessReal64)) {
      types[column] = nativeRowsetTypeReal64;
      continue;
    }
  }
  return types;
}

Uint8List buildCancelPayload(int cancelType, int targetSequence) {
  final payload = ByteData(8);
  payload.setUint32(0, cancelType, Endian.little);
  payload.setUint32(4, targetSequence, Endian.little);
  return payload.buffer.asUint8List();
}

Uint8List buildSblrExecutePayload(
  int sblrHash,
  Uint8List? sblrBytecode,
  List<ParamValue> params,
) {
  final bytecode = sblrBytecode ?? Uint8List(0);
  final payload = BytesBuilder();
  final header = ByteData(16);
  header.setUint64(0, sblrHash, Endian.little);
  header.setUint32(8, bytecode.length, Endian.little);
  header.setUint16(12, params.length, Endian.little);
  header.setUint16(14, 0, Endian.little);
  payload.add(header.buffer.asUint8List());
  if (bytecode.isNotEmpty) {
    payload.add(bytecode);
  }
  for (final param in params) {
    if (param.isNull || param.data == null) {
      payload.add(_i32(-1));
    } else {
      payload.add(_i32(param.data!.length));
      payload.add(param.data!);
    }
  }
  return payload.toBytes();
}

Uint8List buildSubscribePayload(
  int subscribeType,
  String channel,
  String filterExpr,
) {
  final channelBytes = Uint8List.fromList(utf8.encode(channel));
  final filterBytes = Uint8List.fromList(utf8.encode(filterExpr));
  final payload = BytesBuilder();
  payload.add(Uint8List.fromList([subscribeType, 0, 0, 0]));
  payload.add(_u32(channelBytes.length));
  payload.add(channelBytes);
  payload.add(_u32(filterBytes.length));
  payload.add(filterBytes);
  return payload.toBytes();
}

Uint8List buildUnsubscribePayload(String channel) {
  final channelBytes = Uint8List.fromList(utf8.encode(channel));
  final payload = BytesBuilder();
  payload.add(_u32(channelBytes.length));
  payload.add(channelBytes);
  return payload.toBytes();
}

Uint8List buildTxnBeginPayload(
  int flags,
  int conflictAction,
  int autocommitMode,
  int isolationLevel,
  int accessMode,
  int deferrable,
  int waitMode,
  int timeoutMs,
  int readCommittedMode,
) {
  final payload = ByteData(
    (flags & txnFlagHasReadCommittedMode) != 0 ? 16 : 12,
  );
  payload.setUint16(0, flags, Endian.little);
  payload.setUint8(2, conflictAction);
  payload.setUint8(3, autocommitMode);
  payload.setUint8(4, isolationLevel);
  payload.setUint8(5, accessMode);
  payload.setUint8(6, deferrable);
  payload.setUint8(7, waitMode);
  payload.setUint32(8, timeoutMs, Endian.little);
  if ((flags & txnFlagHasReadCommittedMode) != 0) {
    payload.setUint8(12, readCommittedMode);
  }
  return payload.buffer.asUint8List();
}

Uint8List buildTxnCommitPayload(int flags) {
  return Uint8List.fromList([flags, 0, 0, 0]);
}

Uint8List buildTxnRollbackPayload(int flags) {
  return Uint8List.fromList([flags, 0, 0, 0]);
}

Uint8List buildTxnSavepointPayload(String name) {
  final nameBytes = Uint8List.fromList(utf8.encode(name));
  final payload = BytesBuilder();
  payload.add(_u32(nameBytes.length));
  payload.add(nameBytes);
  return payload.toBytes();
}

Uint8List buildTxnReleasePayload(String name) => buildTxnSavepointPayload(name);

Uint8List buildTxnRollbackToPayload(String name) =>
    buildTxnSavepointPayload(name);

Uint8List buildSetOptionPayload(String name, String value) {
  final nameBytes = Uint8List.fromList(utf8.encode(name));
  final valueBytes = Uint8List.fromList(utf8.encode(value));
  final payload = BytesBuilder();
  payload.add(_u32(nameBytes.length));
  payload.add(nameBytes);
  payload.add(_u32(valueBytes.length));
  payload.add(valueBytes);
  return payload.toBytes();
}

Uint8List buildStreamControlPayload(
  int controlType,
  int windowSize,
  int timeoutMs,
) {
  final payload = ByteData(12);
  payload.setUint8(0, controlType);
  payload.setUint32(4, windowSize, Endian.little);
  payload.setUint32(8, timeoutMs, Endian.little);
  return payload.buffer.asUint8List();
}

Uint8List buildAttachCreatePayload(String emulationMode, String dbName) {
  final modeBytes = Uint8List.fromList(utf8.encode(emulationMode));
  final dbBytes = Uint8List.fromList(utf8.encode(dbName));
  final payload = BytesBuilder();
  payload.add(_u32(modeBytes.length));
  payload.add(modeBytes);
  payload.add(_u32(dbBytes.length));
  payload.add(dbBytes);
  return payload.toBytes();
}

Uint8List buildParamList(Map<String, String> params) {
  final payload = BytesBuilder();
  params.forEach((key, value) {
    payload.add(Uint8List.fromList(utf8.encode(key)));
    payload.add(Uint8List.fromList([0]));
    payload.add(Uint8List.fromList(utf8.encode(value)));
    payload.add(Uint8List.fromList([0]));
  });
  payload.add(Uint8List.fromList([0]));
  return payload.toBytes();
}

class ParamValue {
  final int format;
  final Uint8List? data;
  final bool isNull;

  ParamValue({required this.format, this.data, this.isNull = false});
}

Uint8List _u16(int value) {
  final data = ByteData(2);
  data.setUint16(0, value, Endian.little);
  return data.buffer.asUint8List();
}

Uint8List _u32(int value) {
  final data = ByteData(4);
  data.setUint32(0, value, Endian.little);
  return data.buffer.asUint8List();
}

Uint8List _u64(int value) {
  final data = ByteData(8);
  data.setUint64(0, value, Endian.little);
  return data.buffer.asUint8List();
}

Uint8List _i32(int value) {
  final data = ByteData(4);
  data.setInt32(0, value, Endian.little);
  return data.buffer.asUint8List();
}

Uint8List _i64(int value) {
  final data = ByteData(8);
  data.setInt64(0, value, Endian.little);
  return data.buffer.asUint8List();
}

Uint8List _f64(double value) {
  final data = ByteData(8);
  data.setFloat64(0, value, Endian.little);
  return data.buffer.asUint8List();
}

bool _sameStringList(List<String> left, List<String> right) {
  if (left.length != right.length) {
    return false;
  }
  for (var index = 0; index < left.length; index += 1) {
    if (left[index] != right[index]) {
      return false;
    }
  }
  return true;
}

List<String> _splitCopyCsvLine(String line) {
  final values = <String>[];
  final current = StringBuffer();
  var inQuotes = false;
  for (var index = 0; index < line.length; index += 1) {
    final ch = line[index];
    if (ch == '"') {
      if (inQuotes && index + 1 < line.length && line[index + 1] == '"') {
        current.write('"');
        index += 1;
      } else {
        inQuotes = !inQuotes;
      }
      continue;
    }
    if (ch == ',' && !inQuotes) {
      values.add(current.toString());
      current.clear();
      continue;
    }
    current.write(ch);
  }
  values.add(current.toString());
  return values;
}

bool _losslessInt(String value, int minimum, int maximum) {
  if (value.isEmpty) {
    return false;
  }
  final parsed = int.tryParse(value);
  if (parsed == null || parsed < minimum || parsed > maximum) {
    return false;
  }
  return parsed.toString() == value;
}

bool _losslessUint64(String value) {
  if (value.isEmpty || value.startsWith('-')) {
    return false;
  }
  final parsed = BigInt.tryParse(value);
  if (parsed == null || parsed > BigInt.parse('18446744073709551615')) {
    return false;
  }
  return parsed.toString() == value;
}

bool _losslessReal64(String value) {
  final text = value.trim();
  if (text.isEmpty) {
    return false;
  }
  final lower = text.toLowerCase();
  if (lower == 'nan' ||
      lower == '+nan' ||
      lower == '-nan' ||
      lower == 'inf' ||
      lower == '+inf' ||
      lower == '-inf' ||
      lower == 'infinity' ||
      lower == '+infinity' ||
      lower == '-infinity') {
    return false;
  }
  final parsed = double.tryParse(text);
  if (parsed == null || parsed.isNaN || parsed.isInfinite) {
    return false;
  }
  return text.contains('.') || text.contains('e') || text.contains('E');
}

bool _truthyNativeRowsetBoolean(Object value) {
  if (value is bool) {
    return value;
  }
  final normalized = value.toString().trim().toLowerCase();
  return normalized == 'true' || normalized == '1';
}
