// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import 'dart:convert';
import 'dart:mirrors';
import 'dart:typed_data';

import 'package:scratchbird/scratchbird.dart';
import 'package:scratchbird/src/protocol.dart' as wire;
import 'package:test/test.dart';

ScratchBirdClient _newClient() {
  return ScratchBirdClient(
    const ScratchBirdConfig(
      host: 'localhost',
      port: 3092,
      database: 'db',
      user: 'user',
    ),
  );
}

class _PreparedTxnClient extends ScratchBirdClient {
  final List<String> recordedSql = [];

  _PreparedTxnClient(super.config);

  @override
  Future<ScratchBirdResult> query(String sql,
      [List<dynamic> params = const []]) async {
    recordedSql.add(sql);
    return ScratchBirdResult(const [], const []);
  }
}

void main() {
  group('txn guardrails', () {
    test('commit requires active transaction', () async {
      final client = _newClient();
      await expectLater(
        client.commit(),
        throwsA(
          isA<ScratchBirdTransactionException>().having(
            (e) => e.message,
            'message',
            contains('active transaction'),
          ),
        ),
      );
    });

    test('rollback requires active transaction', () async {
      final client = _newClient();
      await expectLater(
        client.rollback(),
        throwsA(
          isA<ScratchBirdTransactionException>().having(
            (e) => e.message,
            'message',
            contains('active transaction'),
          ),
        ),
      );
    });

    test('savepoint requires active transaction', () async {
      final client = _newClient();
      await expectLater(
        client.savepoint('sp1'),
        throwsA(
          isA<ScratchBirdTransactionException>().having(
            (e) => e.message,
            'message',
            contains('active transaction'),
          ),
        ),
      );
    });

    test('prepared transaction helpers emit canonical control SQL', () async {
      final client = _PreparedTxnClient(
        const ScratchBirdConfig(
          host: 'localhost',
          port: 3092,
          database: 'db',
          user: 'user',
        ),
      );

      expect(client.supportsPreparedTransactions(), isTrue);
      expect(client.supportsDormantReattach(), isFalse);

      await client.prepareTransaction('gid-1');
      await client.commitPrepared('gid-1');
      await client.rollbackPrepared("gid'2");

      expect(
          client.recordedSql,
          equals([
            "PREPARE TRANSACTION 'gid-1'",
            "COMMIT PREPARED 'gid-1'",
            "ROLLBACK PREPARED 'gid''2'",
          ]));
    });

    test('prepared transaction helpers reject blank gid', () async {
      final client = _newClient();
      await expectLater(
        client.prepareTransaction('   '),
        throwsA(
          isA<ScratchBirdProgrammingException>()
              .having((e) => e.sqlState, 'sqlState', equals('42601'))
              .having(
                (e) => e.message,
                'message',
                contains('Global transaction id must not be empty'),
              ),
        ),
      );
    });

    test('dormant helpers fail closed and remain explicit', () async {
      final client = _newClient();

      expect(client.supportsDormantReattach(), isFalse);

      await expectLater(
        client.detachToDormant(),
        throwsA(
          isA<ScratchBirdNotSupportedException>()
              .having((e) => e.sqlState, 'sqlState', equals('0A000'))
              .having(
                (e) => e.message,
                'message',
                contains('Dormant detach'),
              ),
        ),
      );

      await expectLater(
        client.reattachDormant('dormant-1', 'token'),
        throwsA(
          isA<ScratchBirdNotSupportedException>()
              .having((e) => e.sqlState, 'sqlState', equals('0A000'))
              .having(
                (e) => e.message,
                'message',
                contains('Dormant reattach'),
              ),
        ),
      );
    });
  });

  group('txn payload encoding', () {
    test('begin payload encodes flags and options', () {
      const flags = wire.txnFlagHasIsolation |
          wire.txnFlagHasAccess |
          wire.txnFlagHasDeferrable |
          wire.txnFlagHasWait |
          wire.txnFlagHasTimeout |
          wire.txnFlagHasAutocommit;
      final payload = wire.buildTxnBeginPayload(
        flags,
        2,
        1,
        wire.isolationSerializable,
        1,
        1,
        0,
        1500,
        wire.readCommittedModeDefault,
      );
      final data = ByteData.sublistView(payload);
      expect(data.getUint16(0, Endian.little), equals(flags));
      expect(data.getUint8(2), equals(2));
      expect(data.getUint8(3), equals(1));
      expect(data.getUint8(4), equals(wire.isolationSerializable));
      expect(data.getUint8(5), equals(1));
      expect(data.getUint8(6), equals(1));
      expect(data.getUint8(7), equals(0));
      expect(data.getUint32(8, Endian.little), equals(1500));
    });

    test('begin payload expands for read committed mode', () {
      final payload = wire.buildTxnBeginPayload(
        wire.txnFlagHasIsolation |
            wire.txnFlagHasTimeout |
            wire.txnFlagHasReadCommittedMode,
        0,
        0,
        wire.isolationReadCommitted,
        0,
        0,
        0,
        25,
        readCommittedModeReadConsistency,
      );
      final data = ByteData.sublistView(payload);
      expect(payload.length, equals(16));
      expect(
        data.getUint16(0, Endian.little),
        equals(
          wire.txnFlagHasIsolation |
              wire.txnFlagHasTimeout |
              wire.txnFlagHasReadCommittedMode,
        ),
      );
      expect(data.getUint8(12), equals(readCommittedModeReadConsistency));
    });

    test('canonical read committed mode label documents public selector', () {
      expect(
        canonicalReadCommittedModeLabel(readCommittedModeReadConsistency),
        equals('READ COMMITTED READ CONSISTENCY'),
      );
      expect(canonicalReadCommittedModeLabel(99), equals('UNKNOWN(99)'));
    });

    test('begin rejects read committed mode with snapshot aliases', () async {
      final client = _newClient();
      await expectLater(
        client.begin(
          isolationLevel: wire.isolationSerializable,
          readCommittedMode: readCommittedModeReadConsistency,
        ),
        throwsA(
          isA<ScratchBirdNotSupportedException>()
              .having((e) => e.sqlState, 'sqlState', equals('0A000'))
              .having(
                (e) => e.message,
                'message',
                contains('READ COMMITTED isolation alias'),
              ),
        ),
      );
    });

    test('savepoint payloads preserve savepoint name bytes', () {
      final savepoint = wire.buildTxnSavepointPayload('sp_1');
      final release = wire.buildTxnReleasePayload('sp_1');
      final rollbackTo = wire.buildTxnRollbackToPayload('sp_1');
      expect(release, equals(savepoint));
      expect(rollbackTo, equals(savepoint));
      final data = ByteData.sublistView(savepoint);
      expect(data.getUint32(0, Endian.little), equals(4));
      expect(String.fromCharCodes(savepoint.sublist(4)), equals('sp_1'));
    });
  });

  group('exec guardrails', () {
    test('query rejects empty SQL', () async {
      final client = _newClient();
      await expectLater(
        client.query('   '),
        throwsA(
          isA<ArgumentError>().having(
            (e) => e.message,
            'message',
            contains('SQL text must not be empty'),
          ),
        ),
      );
    });

    test('cancel rejects when no active query sequence', () async {
      final client = _newClient();
      await expectLater(
        client.cancel(),
        throwsA(
          isA<ScratchBirdExecutionException>().having(
            (e) => e.message,
            'message',
            contains('No active query to cancel'),
          ),
        ),
      );
    });

    test('portal resume helper requires explicit suspended state', () async {
      final client = _newClient();
      final library = reflectClass(ScratchBirdClient).owner as LibraryMirror;
      final symbol = MirrorSystem.getSymbol('_resumeSuspendedPortal', library);

      await expectLater(
        reflect(client).invoke(symbol, [32]).reflectee,
        throwsA(
          isA<ScratchBirdExecutionException>()
              .having((e) => e.sqlState, 'sqlState', equals('55000'))
              .having(
                (e) => e.message,
                'message',
                contains(
                    'Portal resume requires an explicit suspended result state'),
              ),
        ),
      );
    });
  });

  group('exec payload encoding', () {
    test('query payload encodes flags, limits, timeout, and SQL', () {
      final payload = wire.buildQueryPayload(
        'SELECT 42',
        wire.queryFlagBinaryResult | wire.queryFlagIncludePlan,
        25,
        900,
      );
      final data = ByteData.sublistView(payload);
      expect(
        data.getUint32(0, Endian.little),
        equals(wire.queryFlagBinaryResult | wire.queryFlagIncludePlan),
      );
      expect(data.getUint32(4, Endian.little), equals(25));
      expect(data.getUint32(8, Endian.little), equals(900));
      expect(String.fromCharCodes(payload.sublist(12)), equals('SELECT 42'));
    });

    test('query payload uses UTF-8 without NUL termination', () {
      final payload =
          wire.buildQueryPayload('-- box \u2500\nSELECT 42', 0, 0, 0);
      expect(
          payload.sublist(12), equals(utf8.encode('-- box \u2500\nSELECT 42')));
    });

    test('execute payload encodes portal and max rows', () {
      final payload = wire.buildExecutePayload('portal_1', 128);
      final data = ByteData.sublistView(payload);
      final portalLength = data.getUint32(0, Endian.little);
      expect(portalLength, equals(8));
      expect(String.fromCharCodes(payload.sublist(4, 12)), equals('portal_1'));
      expect(data.getUint32(12, Endian.little), equals(128));
    });

    test('cancel payload encodes type and target sequence', () {
      final payload = wire.buildCancelPayload(0, 77);
      final data = ByteData.sublistView(payload);
      expect(data.getUint32(0, Endian.little), equals(0));
      expect(data.getUint32(4, Endian.little), equals(77));
    });
  });
}
