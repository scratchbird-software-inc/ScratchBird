// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import 'dart:typed_data';

import 'package:scratchbird/src/protocol.dart';
import 'package:test/test.dart';

void main() {
  test('standard READY payload exposes status transaction and visibility', () {
    final payload = Uint8List(20);
    final data = ByteData.sublistView(payload);
    payload[0] = 1;
    data.setUint64(4, 42, Endian.little);
    data.setUint64(12, 40, Endian.little);

    final ready = parseReady(payload);

    expect(ready.status, equals(1));
    expect(ready.txnId, equals(42));
    expect(ready.visibility, equals(40));
  });

  test('P1 READY payload maps T/E status bytes to active transaction', () {
    final payload = Uint8List(76);
    final data = ByteData.sublistView(payload);
    data.setUint64(48, 1234, Endian.little);
    payload[56] = 'T'.codeUnitAt(0);

    final ready = parseReady(payload);

    expect(ready.status, equals(1));
    expect(ready.txnId, equals(1234));
    expect(ready.visibility, equals(1234));
  });

  test('P1 READY payload maps idle status byte to inactive transaction', () {
    final payload = Uint8List(76);
    final data = ByteData.sublistView(payload);
    data.setUint64(48, 99, Endian.little);
    payload[56] = 'I'.codeUnitAt(0);

    final ready = parseReady(payload);

    expect(ready.status, equals(0));
    expect(ready.txnId, equals(99));
    expect(ready.visibility, equals(99));
  });

  test('transaction status payload exposes status and transaction id', () {
    final payload = Uint8List(12);
    final data = ByteData.sublistView(payload);
    payload[0] = 'T'.codeUnitAt(0);
    data.setUint64(4, 777, Endian.little);

    final status = parseTxnStatus(payload);

    expect(status.status, equals('T'));
    expect(status.txnId, equals(777));
  });
}
