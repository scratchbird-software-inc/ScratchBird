// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:scratchbird/scratchbird.dart';
import 'package:scratchbird/src/protocol.dart';
import 'package:test/test.dart';

const int _managerProtocolMagic = 0x42444253;
const int _managerProtocolVersion = 0x0101;
const int _managerHeaderSize = 12;
const int _mcpMsgConnectResponse = 0x02;
const int _mcpMsgStatusResponse = 0x64;
const int _mcpMsgHello = 0x65;

void main() {
  ScratchBirdConfig _baseConfig(int port) {
    return ScratchBirdConfig(
      host: '127.0.0.1',
      port: port,
      database: 'db',
      user: 'user',
      password: 'pass',
      sslmode: 'disable',
    );
  }

  test('probeAuthSurface reports direct SCRAM_SHA_512', () async {
    final server = await _LoopbackServer.start((socket, reader) async {
      final startup = await _readProtocolMessage(reader);
      expect(startup.header.type, MessageType.startup);
      await _writeProtocolMessage(
        socket,
        MessageType.authRequest,
        Uint8List.fromList([authScramSha512Method, 0, 0, 0]),
      );
    });

    final probe =
        await ScratchBirdClient.probeAuthSurface(_baseConfig(server.port));
    expect(probe.reachable, isTrue);
    expect(probe.ingressMode, 'direct');
    expect(probe.requiredMethod, 'SCRAM_SHA_512');
    expect(probe.requiredPluginMethodId, 'scratchbird.auth.scram_sha_512');
    expect(probe.admittedMethods.single.executableLocally, isTrue);
    expect(probe.additionalContinuationPossible, isTrue);
    await server.done;
  });

  test('probeAuthSurface reports manager TOKEN ingress', () async {
    final server = await _LoopbackServer.start((socket, reader) async {
      final frame = await _readManagerFrame(reader);
      expect(frame.type, _mcpMsgHello);
      await _writeManagerFrame(
        socket,
        _mcpMsgStatusResponse,
        Uint8List(0),
      );
    });

    final probe = await ScratchBirdClient.probeAuthSurface(
      _baseConfig(server.port).copyWith(frontDoorMode: 'manager_proxy'),
    );
    expect(probe.reachable, isTrue);
    expect(probe.ingressMode, 'manager_proxy');
    expect(probe.requiredMethod, 'TOKEN');
    expect(probe.requiredPluginMethodId, 'scratchbird.auth.authkey_token');
    expect(probe.admittedMethods.single.executableLocally, isTrue);
    await server.done;
  });

  test('connect tracks resolved SCRAM_SHA_512 auth context', () async {
    final server = await _LoopbackServer.start((socket, reader) async {
      final startup = await _readProtocolMessage(reader);
      expect(startup.header.type, MessageType.startup);
      await _writeProtocolMessage(
        socket,
        MessageType.authRequest,
        Uint8List.fromList([authScramSha512Method, 0, 0, 0]),
      );

      final clientFirstMsg = await _readProtocolMessage(reader);
      final clientFirst = utf8.decode(clientFirstMsg.payload);
      final clientNonce = _extractNonce(clientFirst);
      final salt = Uint8List.fromList([1, 2, 3, 4, 5, 6, 7, 8]);
      final serverFirst =
          'r=${clientNonce}server,s=${base64.encode(salt)},i=4096';
      final continuePayload = BytesBuilder()
        ..add([authScramSha512Method, 1, 0, 0])
        ..add(_u32(serverFirst.length))
        ..add(utf8.encode(serverFirst));
      await _writeProtocolMessage(
        socket,
        MessageType.authContinue,
        continuePayload.toBytes(),
      );

      final clientFinalMsg = await _readProtocolMessage(reader);
      final clientFinal = utf8.decode(clientFinalMsg.payload);
      final serverFinal = _buildScramServerFinalSha512(
        password: 'pass',
        clientFirst: clientFirst,
        serverFirst: serverFirst,
        clientFinal: clientFinal,
        salt: salt,
        iterations: 4096,
      );
      await _writeProtocolMessage(
        socket,
        MessageType.authOk,
        _makeAuthOkPayload(serverFinal),
        attachmentId: _sessionId(),
      );
      await _writeProtocolMessage(
        socket,
        MessageType.ready,
        Uint8List(0),
        attachmentId: _sessionId(),
      );
    });

    final client = await ScratchBirdClient.connect(_baseConfig(server.port));
    final context = client.getResolvedAuthContext();
    expect(context.ingressMode, 'direct');
    expect(context.resolvedAuthMethod, 'SCRAM_SHA_512');
    expect(context.resolvedAuthPluginId, 'scratchbird.auth.scram_sha_512');
    expect(context.managerAuthenticated, isFalse);
    expect(context.attached, isTrue);
    await client.close();
    await server.done;
  });

  test('connect tracks resolved TOKEN auth context', () async {
    final server = await _LoopbackServer.start((socket, reader) async {
      final startup = await _readProtocolMessage(reader);
      expect(startup.header.type, MessageType.startup);
      await _writeProtocolMessage(
        socket,
        MessageType.authRequest,
        Uint8List.fromList([authTokenMethod, 0, 0, 0]),
      );
      final authResponse = await _readProtocolMessage(reader);
      expect(utf8.decode(authResponse.payload), 'token-123');
      await _writeProtocolMessage(
        socket,
        MessageType.authOk,
        _makeAuthOkPayload(''),
        attachmentId: _sessionId(),
      );
      await _writeProtocolMessage(
        socket,
        MessageType.ready,
        Uint8List(0),
        attachmentId: _sessionId(),
      );
    });

    final client = await ScratchBirdClient.connect(
      _baseConfig(server.port).copyWith(authToken: 'token-123'),
    );
    final context = client.getResolvedAuthContext();
    expect(context.resolvedAuthMethod, 'TOKEN');
    expect(context.resolvedAuthPluginId, 'scratchbird.auth.authkey_token');
    expect(context.attached, isTrue);
    await client.close();
    await server.done;
  });

  test('connect fails closed for PEER auth', () async {
    final server = await _LoopbackServer.start((socket, reader) async {
      final startup = await _readProtocolMessage(reader);
      expect(startup.header.type, MessageType.startup);
      await _writeProtocolMessage(
        socket,
        MessageType.authRequest,
        Uint8List.fromList([authPeerMethod, 0, 0, 0]),
      );
    });

    await expectLater(
      ScratchBirdClient.connect(_baseConfig(server.port)),
      throwsA(
        isA<ScratchBirdAuthException>().having(
          (e) => e.message,
          'message',
          contains('requires external broker support'),
        ),
      ),
    );
    await server.done;
  });
}

class _LoopbackServer {
  final ServerSocket server;
  final Future<void> done;

  _LoopbackServer(this.server, this.done);

  int get port => server.port;

  static Future<_LoopbackServer> start(
    Future<void> Function(Socket socket, _TestSocketReader reader) handler,
  ) async {
    final server = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
    final completer = Completer<void>();
    server.listen((socket) async {
      final reader = _TestSocketReader(StreamIterator(socket));
      try {
        await handler(socket, reader);
        await socket.flush();
      } catch (e, st) {
        if (!completer.isCompleted) {
          completer.completeError(e, st);
        }
      } finally {
        await socket.close();
        await server.close();
        if (!completer.isCompleted) {
          completer.complete();
        }
      }
    }, onError: (Object error, StackTrace stackTrace) {
      if (!completer.isCompleted) {
        completer.completeError(error, stackTrace);
      }
    });
    return _LoopbackServer(server, completer.future);
  }
}

class _TestSocketReader {
  final StreamIterator<Uint8List> _iterator;
  final List<int> _buffer = <int>[];

  _TestSocketReader(this._iterator);

  Future<Uint8List> readExact(int length) async {
    while (_buffer.length < length) {
      if (!await _iterator.moveNext()) {
        throw StateError('socket closed before reading $length bytes');
      }
      _buffer.addAll(_iterator.current);
    }
    final bytes = Uint8List.fromList(_buffer.sublist(0, length));
    _buffer.removeRange(0, length);
    return bytes;
  }
}

Future<ScratchBirdMessage> _readProtocolMessage(
    _TestSocketReader reader) async {
  final headerBytes = await reader.readExact(headerSize);
  final header = decodeHeader(headerBytes);
  final payload =
      header.length == 0 ? Uint8List(0) : await reader.readExact(header.length);
  return ScratchBirdMessage(header, payload);
}

Future<void> _writeProtocolMessage(
  Socket socket,
  int type,
  Uint8List payload, {
  Uint8List? attachmentId,
  int txnId = 0,
  int sequence = 0,
}) async {
  socket.add(
    encodeMessage(
      MessageHeader(
        type: type,
        flags: 0,
        length: payload.length,
        sequence: sequence,
        attachmentId: attachmentId ?? Uint8List(16),
        txnId: txnId,
      ),
      payload,
    ),
  );
  await socket.flush();
}

Future<({int type, Uint8List payload})> _readManagerFrame(
  _TestSocketReader reader,
) async {
  final header = await reader.readExact(_managerHeaderSize);
  final data = ByteData.sublistView(header);
  expect(data.getUint32(0, Endian.little), _managerProtocolMagic);
  expect(data.getUint16(4, Endian.little), _managerProtocolVersion);
  final type = data.getUint8(6);
  final payloadLength = data.getUint32(8, Endian.little);
  final payload =
      payloadLength == 0 ? Uint8List(0) : await reader.readExact(payloadLength);
  return (type: type, payload: payload);
}

Future<void> _writeManagerFrame(
    Socket socket, int type, Uint8List payload) async {
  final header = ByteData(_managerHeaderSize);
  header.setUint32(0, _managerProtocolMagic, Endian.little);
  header.setUint16(4, _managerProtocolVersion, Endian.little);
  header.setUint8(6, type);
  header.setUint8(7, 0);
  header.setUint32(8, payload.length, Endian.little);
  socket.add(header.buffer.asUint8List());
  socket.add(payload);
  await socket.flush();
}

String _extractNonce(String clientFirst) {
  for (final part in clientFirst.split(',')) {
    if (part.startsWith('r=')) {
      return part.substring(2);
    }
  }
  throw StateError('missing SCRAM nonce');
}

Uint8List _sessionId() {
  return Uint8List.fromList(List<int>.generate(16, (index) => index + 1));
}

Uint8List _makeAuthOkPayload(String serverInfo) {
  final infoBytes = Uint8List.fromList(utf8.encode(serverInfo));
  final payload = BytesBuilder()
    ..add(_sessionId())
    ..add(_u32(infoBytes.length))
    ..add(infoBytes);
  return payload.toBytes();
}

String _buildScramServerFinalSha512({
  required String password,
  required String clientFirst,
  required String serverFirst,
  required String clientFinal,
  required Uint8List salt,
  required int iterations,
}) {
  final clientFirstBare = clientFirst.substring(3);
  final clientFinalWithoutProof = clientFinal.split(',p=').first;
  final authMessage = '$clientFirstBare,$serverFirst,$clientFinalWithoutProof';
  final salted = _pbkdf2Sha512(utf8.encode(password), salt, iterations, 64);
  final serverKey = _hmacSha512(salted, utf8.encode('Server Key'));
  final signature = _hmacSha512(serverKey, utf8.encode(authMessage));
  return 'v=${base64.encode(signature)}';
}

Uint8List _pbkdf2Sha512(
  List<int> password,
  List<int> salt,
  int iterations,
  int keyLen,
) {
  final blocks = (keyLen / 64).ceil();
  final out = BytesBuilder();
  for (var i = 1; i <= blocks; i++) {
    out.add(_pbkdf2BlockSha512(password, salt, iterations, i));
  }
  final bytes = out.toBytes();
  return Uint8List.sublistView(bytes, 0, keyLen);
}

List<int> _pbkdf2BlockSha512(
  List<int> password,
  List<int> salt,
  int iterations,
  int blockIndex,
) {
  final block = Uint8List(salt.length + 4);
  block.setAll(0, salt);
  block[block.length - 4] = (blockIndex >> 24) & 0xff;
  block[block.length - 3] = (blockIndex >> 16) & 0xff;
  block[block.length - 2] = (blockIndex >> 8) & 0xff;
  block[block.length - 1] = blockIndex & 0xff;
  var u = _hmacSha512(password, block);
  final out = List<int>.from(u);
  for (var i = 1; i < iterations; i++) {
    u = _hmacSha512(password, u);
    for (var j = 0; j < out.length; j++) {
      out[j] ^= u[j];
    }
  }
  return out;
}

Uint8List _hmacSha512(List<int> key, List<int> data) {
  return Uint8List.fromList(Hmac(sha512, key).convert(data).bytes);
}

Uint8List _u32(int value) {
  final data = ByteData(4)..setUint32(0, value, Endian.little);
  return data.buffer.asUint8List();
}
