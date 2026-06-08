// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import 'package:scratchbird/scratchbird.dart';
import 'package:test/test.dart';

void main() {
  test('sqlstate 23505 maps to integrity exception', () {
    final ex = mapSqlStateExecutionException(
      'duplicate key',
      sqlState: '23505',
      code: 321,
    );
    expect(ex, isA<ScratchBirdIntegrityException>());
    expect(ex.sqlState, equals('23505'));
    expect(ex.code, equals(321));
  });

  test('sqlstate class 22 maps to data exception', () {
    final ex = mapSqlStateExecutionException(
      'invalid text representation',
      sqlState: '22P02',
    );
    expect(ex, isA<ScratchBirdDataException>());
    expect(ex.sqlState, equals('22P02'));
  });

  test('sqlstate class 42 maps to programming exception', () {
    final ex = mapSqlStateExecutionException(
      'relation does not exist',
      sqlState: '42P01',
    );
    expect(ex, isA<ScratchBirdProgrammingException>());
    expect(ex.sqlState, equals('42P01'));
  });

  test('sqlstate class 08 maps to operational exception', () {
    final ex = mapSqlStateExecutionException(
      'connection failure',
      sqlState: '08006',
    );
    expect(ex, isA<ScratchBirdOperationalException>());
    expect(ex.sqlState, equals('08006'));
  });

  test('empty sqlstate falls back to generic execution exception', () {
    final ex = mapSqlStateExecutionException('query failed');
    expect(ex, isA<ScratchBirdExecutionException>());
    expect(ex, isNot(isA<ScratchBirdDataException>()));
    expect(ex.sqlState, isNull);
  });

  test('auth mapping keeps auth class for class 28 sqlstate', () {
    final ex = mapSqlStateAuthException(
      'invalid authorization contract',
      sqlState: '28000',
    );
    expect(ex, isA<ScratchBirdAuthException>());
    expect(ex.sqlState, equals('28000'));
  });

  test('auth mapping promotes class 08 sqlstate to connection exception', () {
    final ex = mapSqlStateAuthException(
      'connection exception',
      sqlState: '08006',
      code: 88,
    );
    expect(ex, isA<ScratchBirdConnectionException>());
    expect(ex.sqlState, equals('08006'));
    expect(ex.code, equals(88));
  });

  test('retry scope classifies statement and reconnect boundaries', () {
    expect(retryScopeForSqlState('40001'), equals(ScratchBirdRetryScope.statement));
    expect(retryScopeForSqlState('40P01'), equals(ScratchBirdRetryScope.statement));
    expect(retryScopeForSqlState('08006'), equals(ScratchBirdRetryScope.reconnect));
    expect(retryScopeForSqlState('57014'), equals(ScratchBirdRetryScope.none));
    expect(retryScopeForSqlState(null), equals(ScratchBirdRetryScope.none));
  });

  test('retryable sqlstate only covers fresh-boundary retries', () {
    expect(isRetryableSqlState('40001'), isTrue);
    expect(isRetryableSqlState('08003'), isTrue);
    expect(isRetryableSqlState('57014'), isFalse);
    expect(isRetryableSqlState(''), isFalse);
  });
}
