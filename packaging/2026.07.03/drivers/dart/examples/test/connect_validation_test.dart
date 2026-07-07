// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import 'dart:io';

import 'package:scratchbird/scratchbird.dart';
import 'package:test/test.dart';

ScratchBirdConfig _baseConfig({
  String sslmode = 'require',
  bool binaryTransfer = true,
  String compression = 'off',
}) {
  return ScratchBirdConfig(
    host: 'localhost',
    port: 3092,
    database: 'db',
    user: 'user',
    sslmode: sslmode,
    binaryTransfer: binaryTransfer,
    compression: compression,
  );
}

void main() {
  test('connect allows sslmode=disable and reaches the socket layer', () async {
    final cfg =
        _baseConfig(sslmode: 'disable').copyWith(host: '127.0.0.1', port: 1);
    await expectLater(
      ScratchBirdClient.connect(cfg),
      throwsA(
        anyOf(
          isA<SocketException>(),
          isA<ScratchBirdConnectionException>().having(
            (e) => e.toString(),
            'message',
            isNot(contains('TLS is required')),
          ),
        ),
      ),
    );
  });

  test('connect allows binary_transfer=false and reaches the socket layer',
      () async {
    final cfg = _baseConfig(binaryTransfer: false);
    await expectLater(
      ScratchBirdClient.connect(cfg.copyWith(host: '127.0.0.1', port: 1)),
      throwsA(
        anyOf(
          isA<SocketException>(),
          isA<ScratchBirdConnectionException>().having(
            (e) => e.toString(),
            'message',
            isNot(contains('binary_transfer=false is not supported')),
          ),
        ),
      ),
    );
  });

  test('connect allows compression=zstd and reaches the socket layer',
      () async {
    final cfg = _baseConfig(compression: 'zstd');
    await expectLater(
      ScratchBirdClient.connect(cfg.copyWith(host: '127.0.0.1', port: 1)),
      throwsA(
        anyOf(
          isA<SocketException>(),
          isA<ScratchBirdConnectionException>().having(
            (e) => e.toString(),
            'message',
            isNot(contains('compression=zstd is not supported')),
          ),
        ),
      ),
    );
  });
}
