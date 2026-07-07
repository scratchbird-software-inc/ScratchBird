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
  group('metadata schema shaping', () {
    test('expands database-default branch style metadata rows', () {
      final rows = <Map<String, dynamic>>[
        {
          'TABLE_CATALOG': 'demo_db',
          'TABLE_SCHEM': 'users.alice.dev',
          'schema_id': 1,
          'owner_id': 7,
        },
        {
          'TABLE_CATALOG': 'demo_db',
          'TABLE_SCHEM': 'sys',
          'schema_id': 2,
          'owner_id': 7,
        },
      ];

      final expanded = expandSchemaMetadataRows(rows, expandParents: true);

      expect(
        expanded.map((row) => row['TABLE_SCHEM']),
        equals(<String>['users', 'users.alice', 'users.alice.dev', 'sys']),
      );
      expect(expanded[0]['TABLE_CATALOG'], equals('demo_db'));
      expect(expanded[1]['TABLE_CATALOG'], equals('demo_db'));
      expect(expanded[0]['schema_id'], isNull);
      expect(expanded[2]['schema_id'], equals(1));
      expect(expanded[3]['schema_id'], equals(2));
    });

    test('supports dotted parent expansion for schema paths', () {
      final paths = listMetadataSchemaPaths(
        <Map<String, dynamic>>[
          {'schema_name': 'analytics.prod'},
          {'schema_name': 'users.alice.dev'},
          {'schema_name': 'users.alice.dev'},
          {'schema_name': 'users..bob.dev'},
        ],
        expandParents: true,
      );

      expect(
        paths,
        equals(<String>[
          'analytics',
          'analytics.prod',
          'users',
          'users.alice',
          'users.alice.dev',
          'users.bob',
          'users.bob.dev',
        ]),
      );
    });

    test('enforces uniqueness within same parent', () {
      final tree = buildMetadataSchemaTree(
        <Map<String, dynamic>>[
          {'schema_name': 'users.bob.dev'},
          {'schema_name': 'users.bob.dev'},
          {'schema_name': 'users.bob.qa'},
        ],
      );

      final users = _findNodeByPath(tree.schemas, 'users');
      final bob = _findNodeByPath(users!.children, 'users.bob');
      expect(bob, isNotNull);
      expect(
        bob!.children.map((child) => child.path).toList(),
        equals(<String>['users.bob.dev', 'users.bob.qa']),
      );
      expect(
          bob.children.where((child) => child.path == 'users.bob.dev').length,
          equals(1));
    });

    test('allows same leaf name under different parents', () {
      final tree = buildMetadataSchemaTree(
        <Map<String, dynamic>>[
          {'schema_name': 'users.alice.dev'},
          {'schema_name': 'users.bob.dev'},
        ],
        database: 'demo_db',
      );

      expect(tree.database, equals('demo_db'));
      expect(
        tree.schemas.map((node) => node.path).toList(),
        equals(<String>['users']),
      );

      final users = _findNodeByPath(tree.schemas, 'users');
      final alice = _findNodeByPath(users!.children, 'users.alice');
      final bob = _findNodeByPath(users.children, 'users.bob');
      final aliceDev = _findNodeByPath(alice!.children, 'users.alice.dev');
      final bobDev = _findNodeByPath(bob!.children, 'users.bob.dev');

      expect(aliceDev, isNotNull);
      expect(bobDev, isNotNull);
      expect(aliceDev, isNot(same(bobDev)));
      expect(aliceDev!.name, equals('dev'));
      expect(bobDev!.name, equals('dev'));
    });
  });
}

MetadataSchemaTreeNode? _findNodeByPath(
  List<MetadataSchemaTreeNode> nodes,
  String path,
) {
  for (final node in nodes) {
    if (node.path == path) {
      return node;
    }
  }
  return null;
}
