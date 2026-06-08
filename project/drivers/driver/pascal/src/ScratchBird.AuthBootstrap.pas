// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

unit ScratchBird.AuthBootstrap;

{$mode delphi}
{$H+}

interface

uses
  SysUtils, Classes,
  {$IFNDEF FPC}
  System.NetEncoding,
  {$ENDIF}
  ScratchBird.Config, ScratchBird.Protocol;

type
  TScratchBirdAuthMethodSurface = record
    MethodCode: Byte;
    MethodName: string;
    PluginMethodId: string;
    ExecutableLocally: Boolean;
    BrokerRequired: Boolean;
  end;

  TScratchBirdAuthMethodSurfaces = array of TScratchBirdAuthMethodSurface;

  TScratchBirdAuthProbeResult = record
    Reachable: Boolean;
    FrontDoorMode: string;
    RequiredMethodCode: Byte;
    RequiredMethodName: string;
    RequiredPluginMethodId: string;
    RequiredMethodBrokerRequired: Boolean;
    AdditionalContinuationPossible: Boolean;
    AdmittedMethods: TScratchBirdAuthMethodSurfaces;
  end;

  TScratchBirdResolvedAuthContext = record
    FrontDoorMode: string;
    Attached: Boolean;
    ManagerAuthenticated: Boolean;
    ResolvedMethodCode: Byte;
    ResolvedMethodName: string;
    ResolvedAuthPluginId: string;
  end;

function AuthMethodName(Method: Byte): string;
function AuthPluginIdForMethod(Method: Byte; const ConfiguredMethodId: string = ''): string;
function AuthMethodExecutableLocally(Method: Byte): Boolean;
function AuthMethodBrokerRequired(Method: Byte): Boolean;
function AdditionalContinuationPossible(Method: Byte): Boolean;
function TryDescribeAuthMethod(Method: Byte; const ConfiguredMethodId: string;
  out Surface: TScratchBirdAuthMethodSurface): Boolean;
function DefaultResolvedAuthContext(const FrontDoorMode: string = 'direct'): TScratchBirdResolvedAuthContext;
function ResolveTokenAuthPayload(const Config: TScratchBirdConfig): TBytes;
procedure ApplyAuthPluginSelection(const Config: TScratchBirdConfig; const Params: TStringList);

implementation

function NormalizeFrontDoorMode(const Value: string): string;
begin
  Result := Trim(Value);
  if Result = '' then
    Result := 'direct';
end;

function BytesFromString(const Value: string): TBytes;
begin
  if Value = '' then
    SetLength(Result, 0)
  else
    Result := TEncoding.UTF8.GetBytes(Value);
end;

function Base64Decode(const Value: string): TBytes;
{$IFDEF FPC}
const
  Alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
var
  Sanitized: string;
  I, Index, Count: Integer;
  Ch: Char;
  Block: array[0..3] of Integer;
  Triple: Cardinal;
begin
  Sanitized := '';
  for I := 1 to Length(Value) do
  begin
    Ch := Value[I];
    if not (Ch in [#9, #10, #13, ' ']) then
      Sanitized := Sanitized + Ch;
  end;

  if Sanitized = '' then
  begin
    SetLength(Result, 0);
    Exit;
  end;

  if (Length(Sanitized) mod 4) <> 0 then
    raise Exception.Create('invalid base64 payload');

  SetLength(Result, 0);
  I := 1;
  while I <= Length(Sanitized) do
  begin
    Count := 0;
    for Index := 0 to 3 do
    begin
      Ch := Sanitized[I + Index];
      if Ch = '=' then
        Block[Index] := -1
      else
      begin
        Block[Index] := Pos(Ch, Alphabet) - 1;
        if Block[Index] < 0 then
          raise Exception.Create('invalid base64 payload');
      end;
      if Block[Index] >= 0 then
        Inc(Count);
    end;

    Triple := 0;
    if Block[0] >= 0 then
      Triple := Triple or (Cardinal(Block[0]) shl 18);
    if Block[1] >= 0 then
      Triple := Triple or (Cardinal(Block[1]) shl 12);
    if Block[2] >= 0 then
      Triple := Triple or (Cardinal(Block[2]) shl 6);
    if Block[3] >= 0 then
      Triple := Triple or Cardinal(Block[3]);

    Index := Length(Result);
    if Count >= 2 then
    begin
      SetLength(Result, Index + 1);
      Result[Index] := Byte((Triple shr 16) and $FF);
      Inc(Index);
    end;
    if Count >= 3 then
    begin
      SetLength(Result, Index + 1);
      Result[Index] := Byte((Triple shr 8) and $FF);
      Inc(Index);
    end;
    if Count = 4 then
    begin
      SetLength(Result, Index + 1);
      Result[Index] := Byte(Triple and $FF);
    end;

    Inc(I, 4);
  end;
end;
{$ELSE}
begin
  Result := TNetEncoding.Base64.DecodeStringToBytes(Value);
end;
{$ENDIF}

procedure AddParamIfPresent(const Params: TStringList; const Name, Value: string);
begin
  if Trim(Value) <> '' then
    Params.Values[Name] := Value;
end;

function AuthMethodName(Method: Byte): string;
begin
  case Method of
    AUTH_OK: Result := 'OK';
    AUTH_PASSWORD: Result := 'PASSWORD';
    AUTH_MD5: Result := 'MD5';
    AUTH_SCRAM_SHA256: Result := 'SCRAM_SHA_256';
    AUTH_SCRAM_SHA512: Result := 'SCRAM_SHA_512';
    AUTH_TOKEN: Result := 'TOKEN';
    AUTH_PEER: Result := 'PEER';
    AUTH_REATTACH: Result := 'REATTACH';
  else
    Result := '';
  end;
end;

function AuthPluginIdForMethod(Method: Byte; const ConfiguredMethodId: string = ''): string;
begin
  if Trim(ConfiguredMethodId) <> '' then
    Exit(Trim(ConfiguredMethodId));

  case Method of
    AUTH_OK: Result := 'scratchbird.auth.none';
    AUTH_PASSWORD: Result := 'scratchbird.auth.password_compat';
    AUTH_MD5: Result := 'scratchbird.auth.md5_legacy';
    AUTH_SCRAM_SHA256: Result := 'scratchbird.auth.scram_sha_256';
    AUTH_SCRAM_SHA512: Result := 'scratchbird.auth.scram_sha_512';
    AUTH_TOKEN: Result := 'scratchbird.auth.authkey_token';
    AUTH_PEER: Result := 'scratchbird.auth.peer_uid';
    AUTH_REATTACH: Result := 'scratchbird.auth.reattach';
  else
    Result := '';
  end;
end;

function AuthMethodExecutableLocally(Method: Byte): Boolean;
begin
  Result := Method in [AUTH_PASSWORD, AUTH_SCRAM_SHA256, AUTH_SCRAM_SHA512, AUTH_TOKEN];
end;

function AuthMethodBrokerRequired(Method: Byte): Boolean;
begin
  Result := Method = AUTH_PEER;
end;

function AdditionalContinuationPossible(Method: Byte): Boolean;
begin
  Result := Method in [AUTH_SCRAM_SHA256, AUTH_SCRAM_SHA512, AUTH_TOKEN, AUTH_PEER];
end;

function TryDescribeAuthMethod(Method: Byte; const ConfiguredMethodId: string;
  out Surface: TScratchBirdAuthMethodSurface): Boolean;
begin
  FillChar(Surface, SizeOf(Surface), 0);
  Surface.MethodCode := Method;
  Surface.MethodName := AuthMethodName(Method);
  Result := Surface.MethodName <> '';
  if not Result then
    Exit;
  Surface.PluginMethodId := AuthPluginIdForMethod(Method, ConfiguredMethodId);
  Surface.ExecutableLocally := AuthMethodExecutableLocally(Method);
  Surface.BrokerRequired := AuthMethodBrokerRequired(Method);
end;

function DefaultResolvedAuthContext(const FrontDoorMode: string = 'direct'): TScratchBirdResolvedAuthContext;
begin
  Result.FrontDoorMode := NormalizeFrontDoorMode(FrontDoorMode);
  Result.Attached := False;
  Result.ManagerAuthenticated := False;
  Result.ResolvedMethodCode := AUTH_OK;
  Result.ResolvedMethodName := '';
  Result.ResolvedAuthPluginId := '';
end;

function ResolveTokenAuthPayload(const Config: TScratchBirdConfig): TBytes;
begin
  if Trim(Config.AuthToken) <> '' then
    Exit(BytesFromString(Config.AuthToken));
  if Trim(Config.AuthMethodPayload) <> '' then
    Exit(BytesFromString(Config.AuthMethodPayload));
  if Trim(Config.AuthPayloadB64) <> '' then
    Exit(Base64Decode(Config.AuthPayloadB64));
  if Trim(Config.AuthPayloadJson) <> '' then
    Exit(BytesFromString(Config.AuthPayloadJson));
  if Trim(Config.WorkloadIdentityToken) <> '' then
    Exit(BytesFromString(Config.WorkloadIdentityToken));
  if Trim(Config.ProxyPrincipalAssertion) <> '' then
    Exit(BytesFromString(Config.ProxyPrincipalAssertion));
  SetLength(Result, 0);
end;

procedure ApplyAuthPluginSelection(const Config: TScratchBirdConfig; const Params: TStringList);
begin
  AddParamIfPresent(Params, 'auth_method_id', Config.AuthMethodId);
  AddParamIfPresent(Params, 'auth_method_payload', Config.AuthMethodPayload);
  AddParamIfPresent(Params, 'auth_payload_json', Config.AuthPayloadJson);
  AddParamIfPresent(Params, 'auth_payload_b64', Config.AuthPayloadB64);
  AddParamIfPresent(Params, 'auth_provider_profile', Config.AuthProviderProfile);
  AddParamIfPresent(Params, 'auth_required_methods', Config.AuthRequiredMethods);
  AddParamIfPresent(Params, 'auth_forbidden_methods', Config.AuthForbiddenMethods);
  if Config.AuthRequireChannelBinding then
    Params.Values['auth_require_channel_binding'] := '1';
  AddParamIfPresent(Params, 'workload_identity_token', Config.WorkloadIdentityToken);
  AddParamIfPresent(Params, 'proxy_principal_assertion', Config.ProxyPrincipalAssertion);
  AddParamIfPresent(Params, 'dormant_id', Config.DormantId);
  AddParamIfPresent(Params, 'dormant_reattach_token', Config.DormantReattachToken);
end;

end.
