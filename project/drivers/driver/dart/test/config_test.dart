// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import 'package:test/test.dart';
import 'package:scratchbird/scratchbird.dart';

void main() {
  test('parses dsn', () {
    final cfg =
        ScratchBirdConfig.fromDsn('scratchbird://user:pass@localhost:3092/db');
    expect(cfg.user, 'user');
    expect(cfg.password, 'pass');
    expect(cfg.database, 'db');
  });

  test('parses manager proxy params', () {
    final cfg = ScratchBirdConfig.fromDsn(
      'scratchbird://admin:secret@localhost:3090/mydb?front_door_mode=manager_proxy&manager_auth_token=token&manager_client_flags=7',
    );
    expect(cfg.frontDoorMode, 'manager_proxy');
    expect(cfg.managerAuthToken, 'token');
    expect(cfg.managerClientFlags, 7);
  });

  test('rejects invalid front door mode', () {
    expect(
      () => ScratchBirdConfig.fromDsn(
        'scratchbird://localhost:3092/db?front_door_mode=invalid',
      ),
      throwsArgumentError,
    );
  });

  test('parses metadata parent expansion aliases', () {
    final cfg = ScratchBirdConfig.fromDsn(
      'scratchbird://user:pass@localhost:3092/db?metadata_expand_schema_parents=true',
    );
    expect(cfg.metadataExpandSchemaParents, isTrue);

    final kv = ScratchBirdConfig.fromDsn(
      'host=localhost port=3092 database=db user=user expand_schema_parents=1',
    );
    expect(kv.metadataExpandSchemaParents, isTrue);
  });

  test('parses bootstrap auth params', () {
    final cfg = ScratchBirdConfig.fromDsn(
      'scratchbird://user:pass@localhost:3092/db?connect_client_flags=12&auth_token=token123&auth_method_id=scratchbird.auth.scram_sha_512&auth_payload_json=%7B%22sub%22%3A%22user%22%7D&auth_required_methods=TOKEN&auth_forbidden_methods=PEER&auth_require_channel_binding=true&workload_identity_token=wid&proxy_principal_assertion=proxy&dormant_id=dormant-1&dormant_reattach_token=reattach-1',
    );
    expect(cfg.connectClientFlags, 12);
    expect(cfg.authToken, 'token123');
    expect(cfg.authMethodId, 'scratchbird.auth.scram_sha_512');
    expect(cfg.authPayloadJson, '{"sub":"user"}');
    expect(cfg.authRequiredMethods, 'TOKEN');
    expect(cfg.authForbiddenMethods, 'PEER');
    expect(cfg.authRequireChannelBinding, isTrue);
    expect(cfg.workloadIdentityToken, 'wid');
    expect(cfg.proxyPrincipalAssertion, 'proxy');
    expect(cfg.dormantId, 'dormant-1');
    expect(cfg.dormantReattachToken, 'reattach-1');
  });
}
