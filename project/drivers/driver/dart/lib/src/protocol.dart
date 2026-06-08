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
    final value =
        utf8.decode(payload.sublist(start, offset), allowMalformed: true);
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

({int method, int stage, Uint8List data}) parseAuthContinue(
  Uint8List payload,
) {
  if (payload.length < 8) {
    throw const ScratchBirdConnectionException(
      'Auth continue truncated',
      sqlState: '08P01',
    );
  }
  final dataLen =
      ByteData.sublistView(payload, 4, 8).getUint32(0, Endian.little);
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
  final infoLen =
      ByteData.sublistView(payload, 16, 20).getUint32(0, Endian.little);
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

Uint8List buildStartupPayload(int features, Map<String, String> params) {
  final paramBytes = buildParamList(params);
  final payload = ByteData(12 + paramBytes.length);
  payload.setUint8(0, protocolMajor);
  payload.setUint8(1, protocolMinor);
  payload.setUint16(2, 0, Endian.little);
  payload.setUint64(4, features, Endian.little);
  final bytes = payload.buffer.asUint8List();
  bytes.setAll(12, paramBytes);
  return bytes;
}

Uint8List buildQueryPayload(String sql, int flags, int maxRows, int timeoutMs) {
  final sqlBytes = Uint8List.fromList('${sql}\u0000'.codeUnits);
  final payload = ByteData(12 + sqlBytes.length);
  payload.setUint32(0, flags, Endian.little);
  payload.setUint32(4, maxRows, Endian.little);
  payload.setUint32(8, timeoutMs, Endian.little);
  final bytes = payload.buffer.asUint8List();
  bytes.setAll(12, sqlBytes);
  return bytes;
}

Uint8List buildParsePayload(
    String statementName, String sql, List<int> paramTypes) {
  final nameBytes = Uint8List.fromList(statementName.codeUnits);
  final sqlBytes = Uint8List.fromList(sql.codeUnits);
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

Uint8List buildBindPayload(String portalName, String statementName,
    List<ParamValue> params, List<int> resultFormats) {
  final portalBytes = Uint8List.fromList(portalName.codeUnits);
  final stmtBytes = Uint8List.fromList(statementName.codeUnits);
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
  final nameBytes = Uint8List.fromList(name.codeUnits);
  final payload = BytesBuilder();
  payload.add(Uint8List.fromList([kind, 0, 0, 0]));
  payload.add(_u32(nameBytes.length));
  payload.add(nameBytes);
  return payload.toBytes();
}

Uint8List buildExecutePayload(String portalName, int maxRows) {
  final portalBytes = Uint8List.fromList(portalName.codeUnits);
  final payload = BytesBuilder();
  payload.add(_u32(portalBytes.length));
  payload.add(portalBytes);
  payload.add(_u32(maxRows));
  return payload.toBytes();
}

Uint8List buildCancelPayload(int cancelType, int targetSequence) {
  final payload = ByteData(8);
  payload.setUint32(0, cancelType, Endian.little);
  payload.setUint32(4, targetSequence, Endian.little);
  return payload.buffer.asUint8List();
}

Uint8List buildSblrExecutePayload(
    int sblrHash, Uint8List? sblrBytecode, List<ParamValue> params) {
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
    int subscribeType, String channel, String filterExpr) {
  final channelBytes = Uint8List.fromList(channel.codeUnits);
  final filterBytes = Uint8List.fromList(filterExpr.codeUnits);
  final payload = BytesBuilder();
  payload.add(Uint8List.fromList([subscribeType, 0, 0, 0]));
  payload.add(_u32(channelBytes.length));
  payload.add(channelBytes);
  payload.add(_u32(filterBytes.length));
  payload.add(filterBytes);
  return payload.toBytes();
}

Uint8List buildUnsubscribePayload(String channel) {
  final channelBytes = Uint8List.fromList(channel.codeUnits);
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
  final nameBytes = Uint8List.fromList(name.codeUnits);
  final payload = BytesBuilder();
  payload.add(_u32(nameBytes.length));
  payload.add(nameBytes);
  return payload.toBytes();
}

Uint8List buildTxnReleasePayload(String name) => buildTxnSavepointPayload(name);

Uint8List buildTxnRollbackToPayload(String name) =>
    buildTxnSavepointPayload(name);

Uint8List buildSetOptionPayload(String name, String value) {
  final nameBytes = Uint8List.fromList(name.codeUnits);
  final valueBytes = Uint8List.fromList(value.codeUnits);
  final payload = BytesBuilder();
  payload.add(_u32(nameBytes.length));
  payload.add(nameBytes);
  payload.add(_u32(valueBytes.length));
  payload.add(valueBytes);
  return payload.toBytes();
}

Uint8List buildStreamControlPayload(
    int controlType, int windowSize, int timeoutMs) {
  final payload = ByteData(12);
  payload.setUint8(0, controlType);
  payload.setUint32(4, windowSize, Endian.little);
  payload.setUint32(8, timeoutMs, Endian.little);
  return payload.buffer.asUint8List();
}

Uint8List buildAttachCreatePayload(String emulationMode, String dbName) {
  final modeBytes = Uint8List.fromList(emulationMode.codeUnits);
  final dbBytes = Uint8List.fromList(dbName.codeUnits);
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
    payload.add(Uint8List.fromList(key.codeUnits));
    payload.add(Uint8List.fromList([0]));
    payload.add(Uint8List.fromList(value.codeUnits));
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

Uint8List _i32(int value) {
  final data = ByteData(4);
  data.setInt32(0, value, Endian.little);
  return data.buffer.asUint8List();
}
