// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:scratchbird/scratchbird.dart';
import 'package:test/test.dart';

final String? _testDsn = Platform.environment['SCRATCHBIRD_TEST_DSN'];
final String? _integrationSkipReason =
    (_testDsn == null || _testDsn!.trim().isEmpty)
        ? 'SCRATCHBIRD_TEST_DSN not set'
        : null;
final String? _managerDsn =
    Platform.environment['SCRATCHBIRD_TEST_MANAGER_DSN'];
final String? _managerIntegrationSkipReason =
    (_managerDsn == null || _managerDsn!.trim().isEmpty)
        ? 'SCRATCHBIRD_TEST_MANAGER_DSN not set'
        : null;

ScratchBirdConfig _integrationConfig() {
  final dsn = _testDsn;
  if (dsn == null || dsn.trim().isEmpty) {
    throw StateError('SCRATCHBIRD_TEST_DSN not set');
  }
  return ScratchBirdConfig.fromDsn(dsn);
}

Future<ScratchBirdClient> _connectClient() {
  return ScratchBirdClient.connect(_integrationConfig());
}

ScratchBirdConfig _managerIntegrationConfig() {
  final dsn = _managerDsn;
  if (dsn == null || dsn.trim().isEmpty) {
    throw StateError('SCRATCHBIRD_TEST_MANAGER_DSN not set');
  }
  final config = ScratchBirdConfig.fromDsn(dsn);
  if (config.frontDoorMode != 'manager_proxy') {
    throw StateError(
      'SCRATCHBIRD_TEST_MANAGER_DSN must include front_door_mode=manager_proxy',
    );
  }
  return config;
}

Future<ScratchBirdClient> _connectManagerClient() {
  return ScratchBirdClient.connect(_managerIntegrationConfig());
}

void main() {
  test(
    'integration connect and basic query',
    () async {
      final client = await _connectClient();
      try {
        final result = await client.query('SELECT 1');
        expect(result.rows, isNotEmpty);
        expect(result.rows.first, isNotEmpty);
        expect(result.rows.first.first, equals(1));
      } finally {
        await client.close();
      }
    },
    skip: _integrationSkipReason,
  );

  test(
    'integration manager-proxy connect and basic query',
    () async {
      final client = await _connectManagerClient();
      try {
        final result = await client.query('SELECT 1');
        expect(result.rows, isNotEmpty);
        expect(result.rows.first, isNotEmpty);
        expect(result.rows.first.first, equals(1));
      } finally {
        await client.close();
      }
    },
    skip: _managerIntegrationSkipReason,
  );

  test(
    'integration parameterized query',
    () async {
      final client = await _connectClient();
      try {
        final result = await client.query('SELECT ?::INTEGER', <dynamic>[42]);
        expect(result.rows, isNotEmpty);
        expect(result.rows.first, isNotEmpty);
        expect(result.rows.first.first, equals(42));
      } finally {
        await client.close();
      }
    },
    skip: _integrationSkipReason,
  );

  test(
    'integration type roundtrip',
    () async {
      final client = await _connectClient();
      try {
        final result = await client.query(
          'SELECT ?::INTEGER, ?::DOUBLE, ?::VARCHAR, ?::BOOLEAN',
          <dynamic>[42, 3.5, 'scratchbird-dart', true],
        );
        expect(result.rows, isNotEmpty);
        expect(result.rows.first,
            equals(<dynamic>[42, 3.5, 'scratchbird-dart', true]));
      } finally {
        await client.close();
      }
    },
    skip: _integrationSkipReason,
  );

  test(
    'integration transaction stays active across connect commit and rollback',
    () async {
      final client = await _connectClient();
      try {
        await client.savepoint('sp_dart_bootstrap');
        await client.releaseSavepoint('sp_dart_bootstrap');
        await client.begin();
        await client.commit();
        await client.savepoint('sp_dart_after_commit');
        await client.releaseSavepoint('sp_dart_after_commit');
        await client.begin();
        await client.rollback();
        await client.savepoint('sp_dart_after_rollback');
        await client.releaseSavepoint('sp_dart_after_rollback');
      } finally {
        await client.close();
      }
    },
    skip: _integrationSkipReason,
  );

  test(
    'integration transaction savepoint lifecycle',
    () async {
      final client = await _connectClient();
      try {
        await client.savepoint('sp_dart_live');
        await client.rollbackToSavepoint('sp_dart_live');
        await client.releaseSavepoint('sp_dart_live');
        await client.commit();
      } finally {
        await client.close();
      }
    },
    skip: _integrationSkipReason,
  );

  test(
    'integration nested begin is rejected while active',
    () async {
      final client = await _connectClient();
      try {
        await client.begin();
        await expectLater(
          client.begin(),
          throwsA(isA<ScratchBirdTransactionException>()),
        );
      } finally {
        await client.close();
      }
    },
    skip: _integrationSkipReason,
  );

  test(
    'integration metadata wrappers',
    () async {
      final client = await _connectClient();
      try {
        final result = await client.queryMetadata('schemas');
        expect(result.columns, isNotEmpty);

        final schemaRows = await client.getSchema(collectionName: 'schemas');
        expect(schemaRows, isA<List<Map<String, dynamic>>>());

        final tree = await client.getSchemaTree(expandParents: true);
        expect(tree.database, equals(_integrationConfig().database));
      } finally {
        await client.close();
      }
    },
    skip: _integrationSkipReason,
  );

  test(
    'integration json and jsonb roundtrip',
    () async {
      final client = await _connectClient();
      try {
        final result = await client.query(
          'SELECT ?::JSON, ?::JSONB',
          <dynamic>[
            <String, dynamic>{'driver': 'dart', 'version': 1},
            ScratchBirdJsonb(
              Uint8List.fromList(
                utf8.encode('{"kind":"jsonb","enabled":true}'),
              ),
            ),
          ],
        );
        expect(result.rows, isNotEmpty);
        final row = result.rows.first;
        expect(row, hasLength(2));
        expect(jsonDecode(row[0] as String),
            equals(<String, dynamic>{'driver': 'dart', 'version': 1}));
        expect(row[1], isA<ScratchBirdJsonb>());
        final jsonb = row[1] as ScratchBirdJsonb;
        expect(
          jsonDecode(utf8.decode(jsonb.raw)),
          equals(<String, dynamic>{'kind': 'jsonb', 'enabled': true}),
        );
      } finally {
        await client.close();
      }
    },
    skip: _integrationSkipReason,
  );
}
