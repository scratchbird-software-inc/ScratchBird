// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

program ConfigTests;

{$mode delphi}
{$APPTYPE CONSOLE}

uses
  SysUtils, ScratchBird.Config;

procedure AssertEqual(const Expected, Actual, MessageText: string);
begin
  if Expected <> Actual then
    raise Exception.Create(MessageText + ': expected=' + Expected + ' actual=' + Actual);
end;

procedure AssertEqualInt(Expected, Actual: Integer; const MessageText: string);
begin
  if Expected <> Actual then
    raise Exception.Create(MessageText + ': expected=' + IntToStr(Expected) + ' actual=' + IntToStr(Actual));
end;

procedure AssertTrue(Value: Boolean; const MessageText: string);
begin
  if not Value then
    raise Exception.Create(MessageText + ': expected true');
end;

var
  Config: TScratchBirdConfig;
begin
  try
    Config := ParseConfig('scratchbird://user:pass@localhost:3092/mydb?sslmode=require&connect_timeout=3&application_name=app&binary_transfer=false&compression=zstd');
    AssertEqual('localhost', Config.Host, 'host');
    AssertEqualInt(3092, Config.Port, 'port');
    AssertEqual('mydb', Config.Database, 'database');
    AssertEqual('user', Config.UserName, 'user');
    AssertEqual('pass', Config.Password, 'password');
    AssertEqual('require', Config.SSLMode, 'sslmode');
    AssertEqualInt(3000, Config.ConnectTimeoutMs, 'connect_timeout');
    AssertEqual('app', Config.ApplicationName, 'application_name');
    AssertTrue(not Config.BinaryTransfer, 'binary_transfer false parse');
    AssertEqual('zstd', Config.Compression, 'compression zstd parse');

    Config := ParseConfig('Host=server;Port=4000;Database=db;Username=me;Password=secret;SSL Mode=prefer;Timeout=5;Socket_Timeout=7;Compression=none');
    AssertEqual('server', Config.Host, 'host kv');
    AssertEqualInt(4000, Config.Port, 'port kv');
    AssertEqual('db', Config.Database, 'database kv');
    AssertEqual('me', Config.UserName, 'user kv');
    AssertEqual('secret', Config.Password, 'password kv');
    AssertEqual('off', Config.Compression, 'compression none alias');

    Config := ParseConfig('scratchbird://admin:secret@localhost:3092/mydb?front_door_mode=manager_proxy&manager_auth_token=token&manager_client_flags=7');
    AssertEqual('manager_proxy', Config.FrontDoorMode, 'front_door_mode');
    AssertEqual('token', Config.ManagerAuthToken, 'manager_auth_token');
    AssertEqualInt(7, Config.ManagerClientFlags, 'manager_client_flags');

    Config := ParseConfig(
      'scratchbird://user:pass@localhost:3092/mydb' +
      '?connect_client_flags=257' +
      '&auth_token=session-token' +
      '&auth_method_id=scratchbird.auth.proxy_assertion' +
      '&auth_method_payload=opaque' +
      '&auth_payload_json=%7B%22subject%22%3A%22alice%22%7D' +
      '&auth_payload_b64=YWJj' +
      '&auth_provider_profile=corp_primary' +
      '&auth_required_methods=SCRAM_SHA_256%2CTOKEN' +
      '&auth_forbidden_methods=MD5' +
      '&auth_require_channel_binding=true' +
      '&workload_identity_token=jwt-token' +
      '&proxy_principal_assertion=signed-assertion' +
      '&dormant_id=dorm-1' +
      '&dormant_reattach_token=dorm-token'
    );
    AssertEqualInt(257, Config.ConnectClientFlags, 'connect_client_flags');
    AssertEqual('session-token', Config.AuthToken, 'auth_token');
    AssertEqual('scratchbird.auth.proxy_assertion', Config.AuthMethodId, 'auth_method_id');
    AssertEqual('opaque', Config.AuthMethodPayload, 'auth_method_payload');
    AssertEqual('{"subject":"alice"}', Config.AuthPayloadJson, 'auth_payload_json');
    AssertEqual('YWJj', Config.AuthPayloadB64, 'auth_payload_b64');
    AssertEqual('corp_primary', Config.AuthProviderProfile, 'auth_provider_profile');
    AssertEqual('SCRAM_SHA_256,TOKEN', Config.AuthRequiredMethods, 'auth_required_methods');
    AssertEqual('MD5', Config.AuthForbiddenMethods, 'auth_forbidden_methods');
    AssertTrue(Config.AuthRequireChannelBinding, 'auth_require_channel_binding');
    AssertEqual('jwt-token', Config.WorkloadIdentityToken, 'workload_identity_token');
    AssertEqual('signed-assertion', Config.ProxyPrincipalAssertion, 'proxy_principal_assertion');
    AssertEqual('dorm-1', Config.DormantId, 'dormant_id');
    AssertEqual('dorm-token', Config.DormantReattachToken, 'dormant_reattach_token');

    try
      ParseConfig('scratchbird://localhost:3092/db?front_door_mode=invalid');
      raise Exception.Create('expected invalid front_door_mode parse failure');
    except
      on E: Exception do
        AssertTrue(Pos('front_door_mode must be direct or manager_proxy', E.Message) > 0, 'invalid front door error');
    end;

    try
      ParseConfig('scratchbird://localhost:3092/db?compression=gzip');
      raise Exception.Create('expected invalid compression parse failure');
    except
      on E: Exception do
        AssertTrue(Pos('compression must be off or zstd', E.Message) > 0, 'invalid compression error');
    end;

    Writeln('ConfigTests: OK');
  except
    on E: Exception do
    begin
      Writeln('ConfigTests: FAILED - ' + E.Message);
      Halt(1);
    end;
  end;
end.
