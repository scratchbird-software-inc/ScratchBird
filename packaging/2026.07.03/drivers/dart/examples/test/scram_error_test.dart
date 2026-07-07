// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import 'package:scratchbird/scratchbird.dart';
import 'package:scratchbird/src/scram.dart';
import 'package:test/test.dart';

void main() {
  test('scram rejects server-first nonce mismatch with auth exception', () {
    final scram = ScramClient('user');
    expect(
      () => scram.handleServerFirst(
        'password',
        'r=server_nonce,s=c2FsdA==,i=4096',
      ),
      throwsA(
        isA<ScratchBirdAuthException>().having(
          (e) => e.message,
          'message',
          contains('nonce mismatch'),
        ),
      ),
    );
  });

  test('scram rejects server-final signature mismatch with auth exception', () {
    final scram = ScramClient('user');
    expect(
      () => scram.verifyServerFinal('v=invalid'),
      throwsA(
        isA<ScratchBirdAuthException>().having(
          (e) => e.message,
          'message',
          contains('signature mismatch'),
        ),
      ),
    );
  });
}
