// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import 'dart:convert';
import 'dart:math';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';

import 'errors.dart';

enum ScramAlgorithm {
  sha256,
  sha512,
}

class ScramClient {
  final String username;
  final ScramAlgorithm algorithm;
  late String clientNonce;
  String? clientFirstBare;
  Uint8List? serverSignature;

  ScramClient(this.username, {this.algorithm = ScramAlgorithm.sha256}) {
    final rand = Random.secure();
    final bytes = List<int>.generate(18, (_) => rand.nextInt(256));
    clientNonce = base64.encode(bytes);
  }

  String clientFirstMessage() {
    clientFirstBare = 'n=${_escape(username)},r=$clientNonce';
    return 'n,,' + clientFirstBare!;
  }

  String handleServerFirst(String password, String serverFirst) {
    final attrs = _parseAttrs(serverFirst);
    final nonce = attrs['r'] ?? '';
    final saltB64 = attrs['s'] ?? '';
    final iterStr = attrs['i'] ?? '0';
    if (!nonce.startsWith(clientNonce)) {
      throw const ScratchBirdAuthException('SCRAM server nonce mismatch');
    }
    final iterations = int.parse(iterStr);
    final salt = base64.decode(saltB64);
    final keyLen = algorithm == ScramAlgorithm.sha512 ? 64 : 32;
    final salted = _pbkdf2(utf8.encode(password), salt, iterations, keyLen);
    final clientKey = _hmac(salted, utf8.encode('Client Key'));
    final storedKey = _hash(clientKey);
    final clientFinalWithoutProof = 'c=biws,r=$nonce';
    final authMessage =
        '${clientFirstBare!},$serverFirst,$clientFinalWithoutProof';
    final clientSignature = _hmac(storedKey, utf8.encode(authMessage));
    final clientProof = _xor(clientKey, clientSignature);
    final serverKey = _hmac(salted, utf8.encode('Server Key'));
    serverSignature = _hmac(serverKey, utf8.encode(authMessage));
    return '$clientFinalWithoutProof,p=${base64.encode(clientProof)}';
  }

  void verifyServerFinal(String serverFinal) {
    final attrs = _parseAttrs(serverFinal);
    final verifier = attrs['v'] ?? '';
    final expected = base64.encode(serverSignature ?? Uint8List(0));
    if (verifier != expected) {
      throw const ScratchBirdAuthException('SCRAM server signature mismatch');
    }
  }

  String _escape(String input) =>
      input.replaceAll('=', '=3D').replaceAll(',', '=2C');

  Map<String, String> _parseAttrs(String message) {
    final out = <String, String>{};
    for (final part in message.split(',')) {
      final idx = part.indexOf('=');
      if (idx > 0) {
        out[part.substring(0, idx)] = part.substring(idx + 1);
      }
    }
    return out;
  }

  Uint8List _hmac(List<int> key, List<int> data) {
    final hmacSha = Hmac(
      algorithm == ScramAlgorithm.sha512 ? sha512 : sha256,
      key,
    );
    return Uint8List.fromList(hmacSha.convert(data).bytes);
  }

  Uint8List _hash(List<int> data) {
    final digest = algorithm == ScramAlgorithm.sha512 ? sha512 : sha256;
    return Uint8List.fromList(digest.convert(data).bytes);
  }

  Uint8List _xor(List<int> left, List<int> right) {
    final out = Uint8List(left.length);
    for (var i = 0; i < left.length; i++) {
      out[i] = left[i] ^ right[i];
    }
    return out;
  }

  Uint8List _pbkdf2(
    List<int> password,
    List<int> salt,
    int iterations,
    int keyLen,
  ) {
    final blockSize = algorithm == ScramAlgorithm.sha512 ? 64 : 32;
    final blocks = (keyLen / blockSize).ceil();
    final out = BytesBuilder();
    for (var i = 1; i <= blocks; i++) {
      out.add(_pbkdf2F(password, salt, iterations, i));
    }
    final bytes = out.toBytes();
    return Uint8List.sublistView(bytes, 0, keyLen);
  }

  List<int> _pbkdf2F(
      List<int> password, List<int> salt, int iterations, int blockIndex) {
    final block = Uint8List(salt.length + 4);
    block.setAll(0, salt);
    block[block.length - 4] = (blockIndex >> 24) & 0xff;
    block[block.length - 3] = (blockIndex >> 16) & 0xff;
    block[block.length - 2] = (blockIndex >> 8) & 0xff;
    block[block.length - 1] = blockIndex & 0xff;
    var u = _hmac(password, block);
    final out = List<int>.from(u);
    for (var i = 1; i < iterations; i++) {
      u = _hmac(password, u);
      for (var j = 0; j < out.length; j++) {
        out[j] ^= u[j];
      }
    }
    return out;
  }
}
