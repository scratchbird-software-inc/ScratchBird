// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import 'package:scratchbird/scratchbird.dart';
import 'package:test/test.dart';

class _FakeMetadataClient extends ScratchBirdClient {
  final List<Map<String, dynamic>> _responseRows;
  final List<String> executedSql = <String>[];

  _FakeMetadataClient(super.config, this._responseRows);

  @override
  Future<ScratchBirdResult> query(String sql,
      [List<dynamic> params = const []]) async {
    executedSql.add(sql);
    final keys = _responseRows.isEmpty
        ? const <String>[]
        : _responseRows.first.keys.toList(growable: false);
    final columns = keys
        .map((key) => ScratchBirdColumn(key, 0, 1))
        .toList(growable: false);
    final rows = _responseRows
        .map((row) => keys.map((key) => row[key]).toList(growable: false))
        .toList(growable: false);
    return ScratchBirdResult(rows, columns);
  }
}

void main() {
  test('queryMetadata resolves aliases to metadata queries', () async {
    final client = _FakeMetadataClient(
      const ScratchBirdConfig(
        host: 'localhost',
        port: 3092,
        database: 'db',
        user: 'user',
      ),
      const <Map<String, dynamic>>[],
    );

    await client.queryMetadata('schema');
    expect(client.executedSql, equals(<String>[metadataSchemasQuery]));
  });

  test('queryMetadata resolves extended metadata aliases', () async {
    final client = _FakeMetadataClient(
      const ScratchBirdConfig(
        host: 'localhost',
        port: 3092,
        database: 'db',
        user: 'user',
      ),
      const <Map<String, dynamic>>[],
    );

    await client.queryMetadata('catalog');
    await client.queryMetadata('pk');
    await client.queryMetadata('foreignkey');
    await client.queryMetadata('tableprivileges');
    await client.queryMetadata('columnprivilege');
    await client.queryMetadata('typeinfo');
    await client.queryMetadata('routine');

    expect(
      client.executedSql,
      equals(<String>[
        metadataCatalogsQuery,
        metadataPrimaryKeysQuery,
        metadataForeignKeysQuery,
        metadataTablePrivilegesQuery,
        metadataColumnPrivilegesQuery,
        metadataTypeInfoQuery,
        metadataRoutinesQuery,
      ]),
    );
  });

  test('queryMetadata rejects unsupported collections', () async {
    final client = _FakeMetadataClient(
      const ScratchBirdConfig(
        host: 'localhost',
        port: 3092,
        database: 'db',
        user: 'user',
      ),
      const <Map<String, dynamic>>[],
    );

    expect(
      () => client.queryMetadata('unsupported_family'),
      throwsArgumentError,
    );
  });

  test('getSchema expands parent schema rows from runtime config', () async {
    final client = _FakeMetadataClient(
      const ScratchBirdConfig(
        host: 'localhost',
        port: 3092,
        database: 'db',
        user: 'user',
        metadataExpandSchemaParents: true,
      ),
      <Map<String, dynamic>>[
        <String, dynamic>{
          'TABLE_SCHEM': 'users.alice.dev',
          'schema_id': 7,
        },
      ],
    );

    final rows = await client.getSchema(collectionName: 'schemas');
    expect(
      rows.map((row) => row['TABLE_SCHEM']),
      equals(<String>['users', 'users.alice', 'users.alice.dev']),
    );
    expect(rows[0]['schema_id'], isNull);
    expect(rows[2]['schema_id'], equals(7));
  });

  test('getSchemaTree returns recursive schema tree from metadata rows', () async {
    final client = _FakeMetadataClient(
      const ScratchBirdConfig(
        host: 'localhost',
        port: 3092,
        database: 'main',
        user: 'user',
      ),
      <Map<String, dynamic>>[
        <String, dynamic>{'schema_name': 'users.alice.dev'},
        <String, dynamic>{'schema_name': 'users.bob.dev'},
      ],
    );

    final tree = await client.getSchemaTree(expandParents: true);
    expect(tree.database, equals('main'));
    expect(tree.schemas.map((node) => node.path), equals(<String>['users']));
    expect(
      tree.schemas.first.children.map((node) => node.path),
      equals(<String>['users.alice', 'users.bob']),
    );
  });

  test('typed metadata wrapper methods route through the richer families',
      () async {
    final client = _FakeMetadataClient(
      const ScratchBirdConfig(
        host: 'localhost',
        port: 3092,
        database: 'db',
        user: 'user',
      ),
      const <Map<String, dynamic>>[],
    );

    await client.metadataRoutines();
    await client.metadataCatalogs();
    await client.metadataPrimaryKeys();
    await client.metadataForeignKeys();
    await client.metadataTablePrivileges();
    await client.metadataColumnPrivileges();
    await client.metadataTypeInfo();

    expect(
      client.executedSql,
      equals(<String>[
        metadataRoutinesQuery,
        metadataCatalogsQuery,
        metadataPrimaryKeysQuery,
        metadataForeignKeysQuery,
        metadataTablePrivilegesQuery,
        metadataColumnPrivilegesQuery,
        metadataTypeInfoQuery,
      ]),
    );
  });
}
