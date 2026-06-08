// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

unit ScratchBird.Config;

{$mode delphi}
{$H+}

interface

uses
  SysUtils, Classes;

type
  TScratchBirdConfig = record
    Host: string;
    Port: Integer;
    Protocol: string;
    FrontDoorMode: string;
    Database: string;
    UserName: string;
    Password: string;
    Schema: string;
    Role: string;
    SSLMode: string;
    SSLRootCert: string;
    SSLCert: string;
    SSLKey: string;
    SSLPassword: string;
    ConnectTimeoutMs: Integer;
    SocketTimeoutMs: Integer;
    ApplicationName: string;
    BinaryTransfer: Boolean;
    Compression: string;
    FetchSize: Integer;
    ManagerAuthToken: string;
    ManagerUsername: string;
    ManagerDatabase: string;
    ManagerConnectionProfile: string;
    ManagerClientIntent: string;
    ManagerClientFlags: Integer;
    ManagerAuthFastPath: Boolean;
    ConnectClientFlags: Integer;
    AuthToken: string;
    AuthMethodId: string;
    AuthMethodPayload: string;
    AuthPayloadJson: string;
    AuthPayloadB64: string;
    AuthProviderProfile: string;
    AuthRequiredMethods: string;
    AuthForbiddenMethods: string;
    AuthRequireChannelBinding: Boolean;
    WorkloadIdentityToken: string;
    ProxyPrincipalAssertion: string;
    DormantId: string;
    DormantReattachToken: string;
  end;

function DefaultConfig: TScratchBirdConfig;
function ParseConfig(const Dsn: string): TScratchBirdConfig;

implementation

function DefaultConfig: TScratchBirdConfig;
begin
  Result.Host := 'localhost';
  Result.Port := 3092;
  Result.Protocol := 'native';
  Result.FrontDoorMode := 'direct';
  Result.Database := '';
  Result.UserName := '';
  Result.Password := '';
  Result.Schema := '';
  Result.Role := '';
  Result.SSLMode := 'require';
  Result.SSLRootCert := '';
  Result.SSLCert := '';
  Result.SSLKey := '';
  Result.SSLPassword := '';
  Result.ConnectTimeoutMs := 30000;
  Result.SocketTimeoutMs := 0;
  Result.ApplicationName := 'scratchbird_pascal';
  Result.BinaryTransfer := True;
  Result.Compression := 'off';
  Result.FetchSize := 0;
  Result.ManagerAuthToken := '';
  Result.ManagerUsername := '';
  Result.ManagerDatabase := '';
  Result.ManagerConnectionProfile := 'SBsql';
  Result.ManagerClientIntent := 'SBsql';
  Result.ManagerClientFlags := 0;
  Result.ManagerAuthFastPath := True;
  Result.ConnectClientFlags := $0100;
  Result.AuthToken := '';
  Result.AuthMethodId := '';
  Result.AuthMethodPayload := '';
  Result.AuthPayloadJson := '';
  Result.AuthPayloadB64 := '';
  Result.AuthProviderProfile := '';
  Result.AuthRequiredMethods := '';
  Result.AuthForbiddenMethods := '';
  Result.AuthRequireChannelBinding := False;
  Result.WorkloadIdentityToken := '';
  Result.ProxyPrincipalAssertion := '';
  Result.DormantId := '';
  Result.DormantReattachToken := '';
end;

function NormalizeCompression(const Value: string): string;
var
  Normalized: string;
begin
  Normalized := LowerCase(Trim(Value));
  if (Normalized = '') or (Normalized = 'off') or (Normalized = 'none') then
    Exit('off');
  if Normalized = 'zstd' then
    Exit('zstd');
  raise Exception.Create('compression must be off or zstd.');
end;

function NormalizeFrontDoorMode(const Value: string): string;
var
  Normalized: string;
begin
  Normalized := LowerCase(Trim(Value));
  if (Normalized = '') or (Normalized = 'direct') then
    Exit('direct');
  if (Normalized = 'manager_proxy') or (Normalized = 'manager-proxy') or (Normalized = 'managed') then
    Exit('manager_proxy');
  raise Exception.Create('front_door_mode must be direct or manager_proxy.');
end;

function ParseBoolean(const Value: string): Boolean;
var
  Normalized: string;
begin
  Normalized := LowerCase(Trim(Value));
  Result := (Normalized = '1') or (Normalized = 'true') or (Normalized = 'yes') or (Normalized = 'on');
end;

procedure ApplyParam(var Config: TScratchBirdConfig; const Key, Value: string);
var
  KeyLower: string;
  Normalized: string;
begin
  KeyLower := LowerCase(Key);
  if (KeyLower = 'host') or (KeyLower = 'server') or (KeyLower = 'data source') or (KeyLower = 'datasource') then
    Config.Host := Value
  else if KeyLower = 'port' then
    Config.Port := StrToIntDef(Value, Config.Port)
  else if (KeyLower = 'database') or (KeyLower = 'dbname') or (KeyLower = 'initial catalog') then
    Config.Database := Value
  else if (KeyLower = 'protocol') or (KeyLower = 'parser') or (KeyLower = 'dialect') then
  begin
    Normalized := LowerCase(Trim(Value));
    if (Normalized = '') or (Normalized = 'native') or (Normalized = 'scratchbird') or
       (Normalized = 'scratchbird-native') or (Normalized = 'scratchbird_native') then
      Config.Protocol := 'native'
    else
      raise Exception.Create('Only protocol=native is supported; connect to the native parser listener/port.');
  end
  else if (KeyLower = 'front_door_mode') or (KeyLower = 'frontdoormode') or
          (KeyLower = 'connection_mode') or (KeyLower = 'ingress_mode') then
    Config.FrontDoorMode := NormalizeFrontDoorMode(Value)
  else if (KeyLower = 'user') or (KeyLower = 'username') or (KeyLower = 'user id') or (KeyLower = 'uid') then
    Config.UserName := Value
  else if (KeyLower = 'password') or (KeyLower = 'pwd') then
    Config.Password := Value
  else if (KeyLower = 'schema') or (KeyLower = 'search_path') or (KeyLower = 'searchpath') or (KeyLower = 'currentschema') then
    Config.Schema := Value
  else if KeyLower = 'role' then
    Config.Role := Value
  else if (KeyLower = 'sslmode') or (KeyLower = 'ssl mode') then
    Config.SSLMode := Value
  else if KeyLower = 'sslrootcert' then
    Config.SSLRootCert := Value
  else if KeyLower = 'sslcert' then
    Config.SSLCert := Value
  else if KeyLower = 'sslkey' then
    Config.SSLKey := Value
  else if KeyLower = 'sslpassword' then
    Config.SSLPassword := Value
  else if (KeyLower = 'connect_timeout') or (KeyLower = 'connecttimeout') or (KeyLower = 'timeout') then
    Config.ConnectTimeoutMs := StrToIntDef(Value, Config.ConnectTimeoutMs div 1000) * 1000
  else if (KeyLower = 'socket_timeout') or (KeyLower = 'sockettimeout') then
    Config.SocketTimeoutMs := StrToIntDef(Value, Config.SocketTimeoutMs div 1000) * 1000
  else if (KeyLower = 'application_name') or (KeyLower = 'applicationname') then
    Config.ApplicationName := Value
  else if (KeyLower = 'binary_transfer') or (KeyLower = 'binarytransfer') then
    Config.BinaryTransfer := ParseBoolean(Value)
  else if (KeyLower = 'fetch_size') or (KeyLower = 'fetchsize') or (KeyLower = 'default_fetch_size') then
    Config.FetchSize := StrToIntDef(Value, 0)
  else if KeyLower = 'compression' then
    Config.Compression := NormalizeCompression(Value)
  else if (KeyLower = 'manager_auth_token') or (KeyLower = 'mcp_auth_token') then
    Config.ManagerAuthToken := Value
  else if (KeyLower = 'manager_username') or (KeyLower = 'mcp_username') then
    Config.ManagerUsername := Value
  else if (KeyLower = 'manager_database') or (KeyLower = 'mcp_database') then
    Config.ManagerDatabase := Value
  else if (KeyLower = 'manager_connection_profile') or (KeyLower = 'mcp_connection_profile') then
    Config.ManagerConnectionProfile := Value
  else if (KeyLower = 'manager_client_intent') or (KeyLower = 'mcp_client_intent') then
    Config.ManagerClientIntent := Value
  else if (KeyLower = 'manager_client_flags') or (KeyLower = 'mcp_client_flags') then
    Config.ManagerClientFlags := StrToIntDef(Value, 0)
  else if (KeyLower = 'manager_auth_fast_path') or (KeyLower = 'mcp_auth_fast_path') then
    Config.ManagerAuthFastPath := ParseBoolean(Value)
  else if (KeyLower = 'client_flags') or (KeyLower = 'connect_client_flags') then
    Config.ConnectClientFlags := StrToIntDef(Value, Config.ConnectClientFlags)
  else if (KeyLower = 'auth_token') or (KeyLower = 'authtoken') then
    Config.AuthToken := Value
  else if (KeyLower = 'auth_method_id') or (KeyLower = 'authmethodid') then
    Config.AuthMethodId := Trim(Value)
  else if (KeyLower = 'auth_method_payload') or (KeyLower = 'authmethodpayload') then
    Config.AuthMethodPayload := Value
  else if (KeyLower = 'auth_payload_json') or (KeyLower = 'authpayloadjson') then
    Config.AuthPayloadJson := Value
  else if (KeyLower = 'auth_payload_b64') or (KeyLower = 'authpayloadb64') then
    Config.AuthPayloadB64 := Value
  else if (KeyLower = 'auth_provider_profile') or (KeyLower = 'authproviderprofile') then
    Config.AuthProviderProfile := Trim(Value)
  else if (KeyLower = 'auth_required_methods') or (KeyLower = 'authrequiredmethods') then
    Config.AuthRequiredMethods := Trim(Value)
  else if (KeyLower = 'auth_forbidden_methods') or (KeyLower = 'authforbiddenmethods') then
    Config.AuthForbiddenMethods := Trim(Value)
  else if (KeyLower = 'auth_require_channel_binding') or (KeyLower = 'authrequirechannelbinding') then
    Config.AuthRequireChannelBinding := ParseBoolean(Value)
  else if (KeyLower = 'workload_identity_token') or (KeyLower = 'workloadidentitytoken') then
    Config.WorkloadIdentityToken := Value
  else if (KeyLower = 'proxy_principal_assertion') or (KeyLower = 'proxyprincipalassertion') or
          (KeyLower = 'proxy_assertion') then
    Config.ProxyPrincipalAssertion := Value
  else if (KeyLower = 'dormant_id') or (KeyLower = 'dormantid') then
    Config.DormantId := Trim(Value)
  else if (KeyLower = 'dormant_reattach_token') or (KeyLower = 'dormantreattachtoken') then
    Config.DormantReattachToken := Value;
end;

function UrlDecode(const Value: string): string;
var
  I: Integer;
  Hex: string;
begin
  Result := '';
  I := 1;
  while I <= Length(Value) do
  begin
    if Value[I] = '%' then
    begin
      if I + 2 <= Length(Value) then
      begin
        Hex := Copy(Value, I + 1, 2);
        Result := Result + Chr(StrToIntDef('$' + Hex, Ord('?')));
        Inc(I, 3);
        Continue;
      end;
    end;
    if Value[I] = '+' then
      Result := Result + ' '
    else
      Result := Result + Value[I];
    Inc(I);
  end;
end;

function ParseUri(const Dsn: string): TScratchBirdConfig;
var
  Work, Auth, Path, Query: string;
  AtPos, SlashPos, QueryPos, ColonPos: Integer;
  UserInfo, HostPort: string;
  Key, Value: string;
  QueryList: TStringList;
  Pair: string;
begin
  Result := DefaultConfig;
  Work := Dsn;
  if Copy(LowerCase(Work), 1, 14) <> 'scratchbird://' then
    raise Exception.Create('Unsupported DSN scheme');
  Work := Copy(Work, 15, MaxInt);
  QueryPos := Pos('?', Work);
  if QueryPos > 0 then
  begin
    Query := Copy(Work, QueryPos + 1, MaxInt);
    Work := Copy(Work, 1, QueryPos - 1);
  end
  else
    Query := '';
  SlashPos := Pos('/', Work);
  if SlashPos > 0 then
  begin
    Auth := Copy(Work, 1, SlashPos - 1);
    Path := Copy(Work, SlashPos + 1, MaxInt);
  end
  else
  begin
    Auth := Work;
    Path := '';
  end;
  AtPos := Pos('@', Auth);
  if AtPos > 0 then
  begin
    UserInfo := Copy(Auth, 1, AtPos - 1);
    HostPort := Copy(Auth, AtPos + 1, MaxInt);
    ColonPos := Pos(':', UserInfo);
    if ColonPos > 0 then
    begin
      Result.UserName := UrlDecode(Copy(UserInfo, 1, ColonPos - 1));
      Result.Password := UrlDecode(Copy(UserInfo, ColonPos + 1, MaxInt));
    end
    else
      Result.UserName := UrlDecode(UserInfo);
  end
  else
    HostPort := Auth;
  ColonPos := LastDelimiter(':', HostPort);
  if ColonPos > 0 then
  begin
    Result.Host := Copy(HostPort, 1, ColonPos - 1);
    Result.Port := StrToIntDef(Copy(HostPort, ColonPos + 1, MaxInt), Result.Port);
  end
  else if HostPort <> '' then
    Result.Host := HostPort;
  if Path <> '' then
    Result.Database := Path;
  if Query <> '' then
  begin
    QueryList := TStringList.Create;
    try
      ExtractStrings(['&'], [], PChar(Query), QueryList);
      for Pair in QueryList do
      begin
        ColonPos := Pos('=', Pair);
        if ColonPos <= 0 then
          Continue;
        Key := Copy(Pair, 1, ColonPos - 1);
        Value := UrlDecode(Copy(Pair, ColonPos + 1, MaxInt));
        ApplyParam(Result, Key, Value);
      end;
    finally
      QueryList.Free;
    end;
  end;
end;


function ParseKeyValue(const Dsn: string): TScratchBirdConfig;
var
  Separator: Char;
  Parts: TStringList;
  Pair: string;
  Token: string;
  SepPos: Integer;
  Key, Value: string;
begin
  Result := DefaultConfig;
  if Pos(';', Dsn) > 0 then
    Separator := ';'
  else
    Separator := ' ';
  Parts := TStringList.Create;
  try
    ExtractStrings([Separator], [], PChar(Dsn), Parts);
    for Pair in Parts do
    begin
      Token := Trim(Pair);
      if Token = '' then
        Continue;
      SepPos := Pos('=', Token);
      if SepPos <= 0 then
        Continue;
      Key := Trim(Copy(Token, 1, SepPos - 1));
      Value := Trim(Copy(Token, SepPos + 1, MaxInt));
      if (Length(Value) >= 2) and (Value[1] = '"') and (Value[Length(Value)] = '"') then
        Value := Copy(Value, 2, Length(Value) - 2);
      ApplyParam(Result, Key, Value);
    end;
  finally
    Parts.Free;
  end;
end;

function ParseConfig(const Dsn: string): TScratchBirdConfig;
begin
  if Trim(Dsn) = '' then
    Exit(DefaultConfig);
  if Pos('://', Dsn) > 0 then
    Result := ParseUri(Dsn)
  else
    Result := ParseKeyValue(Dsn);
end;

end.
