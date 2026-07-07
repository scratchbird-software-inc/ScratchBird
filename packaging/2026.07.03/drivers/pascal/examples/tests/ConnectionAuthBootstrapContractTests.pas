// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

program ConnectionAuthBootstrapContractTests;

{$mode delphi}
{$APPTYPE CONSOLE}

uses
  {$IFDEF UNIX}
  cthreads,
  {$ENDIF}
  SysUtils,
  ScratchBird.Client, ScratchBird.Config, ScratchBird.Protocol, ScratchBird.Transport, ScratchBird.AuthBootstrap;

type
  TFakeTransport = class(TInterfacedObject, IScratchBirdTransport)
  private
    FReadBuffer: TBytes;
    FReadOffset: Integer;
    FWrites: array of TBytes;
    FConnected: Boolean;
  public
    procedure Configure(const Config: TScratchBirdConfig);
    procedure Connect;
    procedure Disconnect;
    function ReadExact(Length: Integer): TBytes;
    procedure Write(const Data: TBytes);
    function IsConnected: Boolean;
    procedure QueueInbound(const Frame: TBytes);
    function WriteCount: Integer;
    function WriteAt(Index: Integer): TBytes;
  end;

const
  MANAGER_PROTOCOL_MAGIC = $42444253;
  MANAGER_PROTOCOL_VERSION = $0101;
  MANAGER_HEADER_SIZE = 12;
  MCP_MSG_STATUS_RESPONSE = $64;
  MCP_MSG_HELLO = $65;

procedure Fail(const MessageText: string);
begin
  raise Exception.Create(MessageText);
end;

procedure AssertTrue(Value: Boolean; const MessageText: string);
begin
  if not Value then
    Fail(MessageText);
end;

procedure AssertEqualInt(Expected, Actual: Integer; const MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText + ': expected=' + IntToStr(Expected) + ' actual=' + IntToStr(Actual));
end;

procedure AssertEqualString(const Expected, Actual, MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText + ': expected="' + Expected + '" actual="' + Actual + '"');
end;

procedure WriteUInt64LEAt(var Buffer: TBytes; Offset: Integer; Value: UInt64);
begin
  Buffer[Offset] := Byte(Value and $FF);
  Buffer[Offset + 1] := Byte((Value shr 8) and $FF);
  Buffer[Offset + 2] := Byte((Value shr 16) and $FF);
  Buffer[Offset + 3] := Byte((Value shr 24) and $FF);
  Buffer[Offset + 4] := Byte((Value shr 32) and $FF);
  Buffer[Offset + 5] := Byte((Value shr 40) and $FF);
  Buffer[Offset + 6] := Byte((Value shr 48) and $FF);
  Buffer[Offset + 7] := Byte((Value shr 56) and $FF);
end;

function BuildReadyPayload(Status: Byte; TxnId, Visibility: UInt64): TBytes;
begin
  SetLength(Result, 20);
  FillChar(Result[0], Length(Result), 0);
  Result[0] := Status;
  WriteUInt64LEAt(Result, 4, TxnId);
  WriteUInt64LEAt(Result, 12, Visibility);
end;

function BuildAuthRequestPayload(Method: Byte): TBytes;
begin
  SetLength(Result, 4);
  FillChar(Result[0], Length(Result), 0);
  Result[0] := Method;
end;

function BuildAuthOkPayload: TBytes;
begin
  SetLength(Result, 20);
  FillChar(Result[0], Length(Result), 0);
end;

function BuildManagerFrame(MsgType: Byte; const Payload: TBytes): TBytes;
begin
  SetLength(Result, MANAGER_HEADER_SIZE + Length(Payload));
  Result[0] := Byte(MANAGER_PROTOCOL_MAGIC and $FF);
  Result[1] := Byte((MANAGER_PROTOCOL_MAGIC shr 8) and $FF);
  Result[2] := Byte((MANAGER_PROTOCOL_MAGIC shr 16) and $FF);
  Result[3] := Byte((MANAGER_PROTOCOL_MAGIC shr 24) and $FF);
  Result[4] := Byte(MANAGER_PROTOCOL_VERSION and $FF);
  Result[5] := Byte((MANAGER_PROTOCOL_VERSION shr 8) and $FF);
  Result[6] := MsgType;
  Result[7] := 0;
  Result[8] := Byte(Length(Payload) and $FF);
  Result[9] := Byte((Length(Payload) shr 8) and $FF);
  Result[10] := Byte((Length(Payload) shr 16) and $FF);
  Result[11] := Byte((Length(Payload) shr 24) and $FF);
  if Length(Payload) > 0 then
    Move(Payload[0], Result[MANAGER_HEADER_SIZE], Length(Payload));
end;

procedure DecodeProtocolFrame(const Frame: TBytes; out MsgType: TScratchBirdMessageType;
  out PayloadLength: Integer; out Payload: TBytes);
var
  Header: TBytes;
  Flags: Byte;
  Sequence: Cardinal;
  AttachmentId: TBytes;
  TxnId: UInt64;
begin
  Header := Copy(Frame, 0, HEADER_SIZE);
  AssertTrue(DecodeHeader(Header, MsgType, Flags, PayloadLength, Sequence, AttachmentId, TxnId), 'protocol frame header decode');
  AssertEqualInt(HEADER_SIZE + PayloadLength, Length(Frame), 'protocol frame total length');
  Payload := Copy(Frame, HEADER_SIZE, PayloadLength);
end;

procedure DecodeManagerFrame(const Frame: TBytes; out MsgType: Byte; out PayloadLength: Integer);
begin
  AssertTrue(Length(Frame) >= MANAGER_HEADER_SIZE, 'manager frame header length');
  AssertTrue(PCardinal(@Frame[0])^ = MANAGER_PROTOCOL_MAGIC, 'manager frame magic');
  MsgType := Frame[6];
  PayloadLength := Integer(Cardinal(Frame[8]) or (Cardinal(Frame[9]) shl 8) or (Cardinal(Frame[10]) shl 16) or (Cardinal(Frame[11]) shl 24));
end;

procedure TFakeTransport.Configure(const Config: TScratchBirdConfig);
begin
end;

procedure TFakeTransport.Connect;
begin
  FConnected := True;
end;

procedure TFakeTransport.Disconnect;
begin
  FConnected := False;
end;

function TFakeTransport.ReadExact(Length: Integer): TBytes;
begin
  if FReadOffset + Length > System.Length(FReadBuffer) then
    raise Exception.Create('fake transport read underflow');
  SetLength(Result, Length);
  if Length > 0 then
    Move(FReadBuffer[FReadOffset], Result[0], Length);
  Inc(FReadOffset, Length);
end;

procedure TFakeTransport.Write(const Data: TBytes);
var
  Index: Integer;
begin
  Index := Length(FWrites);
  SetLength(FWrites, Index + 1);
  FWrites[Index] := Copy(Data, 0, Length(Data));
end;

function TFakeTransport.IsConnected: Boolean;
begin
  Result := FConnected;
end;

procedure TFakeTransport.QueueInbound(const Frame: TBytes);
var
  Start, Count: Integer;
begin
  Count := Length(Frame);
  if Count = 0 then
    Exit;
  Start := Length(FReadBuffer);
  SetLength(FReadBuffer, Start + Count);
  Move(Frame[0], FReadBuffer[Start], Count);
end;

function TFakeTransport.WriteCount: Integer;
begin
  Result := Length(FWrites);
end;

function TFakeTransport.WriteAt(Index: Integer): TBytes;
begin
  Result := FWrites[Index];
end;

procedure TestProbeAuthSurfaceReportsDirectScramSha512;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  Probe: TScratchBirdAuthProbeResult;
  MsgType: TScratchBirdMessageType;
  PayloadLength: Integer;
  Payload: TBytes;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport);
  try
    Transport.QueueInbound(EncodeMessage(MSG_AUTH_REQUEST, BuildAuthRequestPayload(AUTH_SCRAM_SHA512), 0, 1, nil, 0));
    Probe := Client.ProbeAuthSurface('scratchbird://user:pass@localhost:3092/mydb?sslmode=disable');
    AssertTrue(Probe.Reachable, 'direct probe should be reachable');
    AssertEqualString('direct', Probe.FrontDoorMode, 'direct probe ingress mode');
    AssertTrue(Probe.RequiredMethodCode = AUTH_SCRAM_SHA512, 'direct probe method code');
    AssertEqualString('SCRAM_SHA_512', Probe.RequiredMethodName, 'direct probe method name');
    AssertEqualString('scratchbird.auth.scram_sha_512', Probe.RequiredPluginMethodId, 'direct probe plugin id');
    AssertTrue(Probe.AdditionalContinuationPossible, 'direct probe continuation flag');
    AssertEqualInt(1, Length(Probe.AdmittedMethods), 'direct probe admitted methods');
    AssertTrue(Probe.AdmittedMethods[0].ExecutableLocally, 'direct probe executable locally');
    DecodeProtocolFrame(Transport.WriteAt(0), MsgType, PayloadLength, Payload);
    AssertTrue(MsgType = MSG_STARTUP, 'probe first write should be startup');
  finally
    Client.Free;
  end;
end;

procedure TestProbeAuthSurfaceReportsManagerTokenIngress;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  Probe: TScratchBirdAuthProbeResult;
  MsgType: Byte;
  PayloadLength: Integer;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport);
  try
    Transport.QueueInbound(BuildManagerFrame(MCP_MSG_STATUS_RESPONSE, nil));
    Probe := Client.ProbeAuthSurface('scratchbird://admin:secret@localhost:3092/mydb?front_door_mode=manager_proxy&sslmode=disable');
    AssertTrue(Probe.Reachable, 'manager probe should be reachable');
    AssertEqualString('manager_proxy', Probe.FrontDoorMode, 'manager probe ingress mode');
    AssertTrue(Probe.RequiredMethodCode = AUTH_TOKEN, 'manager probe method code');
    AssertEqualString('TOKEN', Probe.RequiredMethodName, 'manager probe method name');
    AssertEqualString('scratchbird.auth.authkey_token', Probe.RequiredPluginMethodId, 'manager probe plugin id');
    AssertTrue(Probe.AdditionalContinuationPossible, 'manager probe continuation flag');
    AssertEqualInt(1, Length(Probe.AdmittedMethods), 'manager probe admitted methods');
    DecodeManagerFrame(Transport.WriteAt(0), MsgType, PayloadLength);
    AssertTrue(MsgType = MCP_MSG_HELLO, 'manager probe first write should be hello');
  finally
    Client.Free;
  end;
end;

procedure TestConnectTracksResolvedScramSha512Context;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  Context: TScratchBirdResolvedAuthContext;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport);
  try
    Transport.QueueInbound(EncodeMessage(MSG_AUTH_REQUEST, BuildAuthRequestPayload(AUTH_SCRAM_SHA512), 0, 1, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_AUTH_OK, BuildAuthOkPayload, 0, 2, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 0, 0), 0, 3, nil, 0));
    Client.Connect('scratchbird://user:secret@localhost:3092/mydb?sslmode=disable');
    Context := Client.GetResolvedAuthContext;
    AssertEqualString('direct', Context.FrontDoorMode, 'scram512 context ingress mode');
    AssertTrue(Context.ResolvedMethodCode = AUTH_SCRAM_SHA512, 'scram512 context method code');
    AssertEqualString('SCRAM_SHA_512', Context.ResolvedMethodName, 'scram512 context method');
    AssertEqualString('scratchbird.auth.scram_sha_512', Context.ResolvedAuthPluginId, 'scram512 context plugin id');
    AssertTrue(Context.Attached, 'scram512 context attached');
    AssertTrue(not Context.ManagerAuthenticated, 'scram512 manager flag');
  finally
    Client.Free;
  end;
end;

procedure TestConnectTracksResolvedTokenContext;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  Context: TScratchBirdResolvedAuthContext;
  MsgType: TScratchBirdMessageType;
  PayloadLength: Integer;
  Payload: TBytes;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport);
  try
    Transport.QueueInbound(EncodeMessage(MSG_AUTH_REQUEST, BuildAuthRequestPayload(AUTH_TOKEN), 0, 1, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_AUTH_OK, BuildAuthOkPayload, 0, 2, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 0, 0), 0, 3, nil, 0));
    Client.Connect('scratchbird://user:pass@localhost:3092/mydb?sslmode=disable&auth_token=token-123');
    Context := Client.GetResolvedAuthContext;
    AssertTrue(Context.ResolvedMethodCode = AUTH_TOKEN, 'token context method code');
    AssertEqualString('TOKEN', Context.ResolvedMethodName, 'token context method');
    AssertEqualString('scratchbird.auth.authkey_token', Context.ResolvedAuthPluginId, 'token context plugin id');
    AssertTrue(Context.Attached, 'token context attached');
    DecodeProtocolFrame(Transport.WriteAt(1), MsgType, PayloadLength, Payload);
    AssertTrue(MsgType = MSG_AUTH_RESPONSE, 'token auth response frame type');
    AssertEqualString('token-123', TEncoding.UTF8.GetString(Payload), 'token auth response payload');
  finally
    Client.Free;
  end;
end;

procedure TestConnectFailClosesPeerAuth;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  Context: TScratchBirdResolvedAuthContext;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport);
  try
    Transport.QueueInbound(EncodeMessage(MSG_AUTH_REQUEST, BuildAuthRequestPayload(AUTH_PEER), 0, 1, nil, 0));
    try
      Client.Connect('scratchbird://user:pass@localhost:3092/mydb?sslmode=disable');
      Fail('expected peer auth fail-closed');
    except
      on E: Exception do
        AssertTrue(Pos('requires external broker support', LowerCase(E.Message)) > 0, 'peer fail-closed message');
    end;
    Context := Client.GetResolvedAuthContext;
    AssertTrue(Context.ResolvedMethodCode = AUTH_PEER, 'peer context method code');
    AssertEqualString('PEER', Context.ResolvedMethodName, 'peer context method');
    AssertTrue(not Context.Attached, 'peer context attached false');
  finally
    Client.Free;
  end;
end;

begin
  try
    TestProbeAuthSurfaceReportsDirectScramSha512;
    TestProbeAuthSurfaceReportsManagerTokenIngress;
    TestConnectTracksResolvedScramSha512Context;
    TestConnectTracksResolvedTokenContext;
    TestConnectFailClosesPeerAuth;
    Writeln('ConnectionAuthBootstrapContractTests: OK');
  except
    on E: Exception do
    begin
      Writeln('ConnectionAuthBootstrapContractTests: FAILED - ' + E.Message);
      Halt(1);
    end;
  end;
end.
