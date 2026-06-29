// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

program ConnectionManagerProxyTests;

{$mode delphi}
{$APPTYPE CONSOLE}

uses
  {$IFDEF UNIX}
  cthreads,
  {$ENDIF}
  SysUtils,
  ScratchBird.Client, ScratchBird.Config, ScratchBird.Errors, ScratchBird.Protocol, ScratchBird.Transport;

const
  MANAGER_PROTOCOL_MAGIC = $42444253;
  MANAGER_PROTOCOL_VERSION = $0101;
  MANAGER_HEADER_SIZE = 12;
  MCP_MSG_CONNECT_RESPONSE = $02;
  MCP_MSG_AUTH_RESPONSE = $11;
  MCP_MSG_STATUS_RESPONSE = $64;

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

function ReadUInt16LEAt(const Buffer: TBytes; Offset: Integer): Word;
begin
  Result := Word(Buffer[Offset]) or (Word(Buffer[Offset + 1]) shl 8);
end;

function ReadUInt32LEAt(const Buffer: TBytes; Offset: Integer): Cardinal;
begin
  Result := Cardinal(Buffer[Offset]) or (Cardinal(Buffer[Offset + 1]) shl 8) or
    (Cardinal(Buffer[Offset + 2]) shl 16) or (Cardinal(Buffer[Offset + 3]) shl 24);
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
  Result[0] := Method;
  Result[1] := 0;
  Result[2] := 0;
  Result[3] := 0;
end;

function BuildAuthOkPayload: TBytes;
begin
  SetLength(Result, 20);
  FillChar(Result[0], Length(Result), 0);
end;

function BuildManagerAuthResponsePayloadSuccess: TBytes;
begin
  SetLength(Result, 1 + 4 + 256);
  FillChar(Result[0], Length(Result), 0);
end;

function BuildManagerAuthResponsePayloadFailure(const MessageText: string): TBytes;
var
  MsgBytes: TBytes;
  CopyLen: Integer;
begin
  SetLength(Result, 1 + 4 + 256);
  FillChar(Result[0], Length(Result), 0);
  Result[0] := 1;
  MsgBytes := TEncoding.UTF8.GetBytes(MessageText);
  CopyLen := Length(MsgBytes);
  if CopyLen > 256 then
    CopyLen := 256;
  if CopyLen > 0 then
    Move(MsgBytes[0], Result[5], CopyLen);
end;

function BuildManagerConnectResponsePayloadSuccess: TBytes;
begin
  SetLength(Result, 1 + 2 + 2 + 16 + 64 + 32);
  FillChar(Result[0], Length(Result), 0);
end;

procedure DecodeManagerFrame(const Frame: TBytes; out MsgType: Byte; out PayloadLength: Integer);
begin
  AssertTrue(Length(Frame) >= MANAGER_HEADER_SIZE, 'manager frame header length');
  AssertTrue(ReadUInt32LEAt(Frame, 0) = MANAGER_PROTOCOL_MAGIC, 'manager frame magic');
  AssertTrue(ReadUInt16LEAt(Frame, 4) = MANAGER_PROTOCOL_VERSION, 'manager frame version');
  MsgType := Frame[6];
  PayloadLength := Integer(ReadUInt32LEAt(Frame, 8));
  AssertEqualInt(MANAGER_HEADER_SIZE + PayloadLength, Length(Frame), 'manager frame total length');
end;

procedure DecodeProtocolFrame(const Frame: TBytes; out MsgType: TScratchBirdMessageType; out PayloadLength: Integer);
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
end;

procedure TFakeTransport.Configure(const Config: TScratchBirdConfig);
begin
  // no-op for deterministic unit tests
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
  if Length < 0 then
    raise Exception.Create('read length must be non-negative');
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

procedure TestManagerProxyConnectSuccessWithPasswordAuth;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  ManagerMsgType: Byte;
  ProtocolMsgType: TScratchBirdMessageType;
  PayloadLength: Integer;
  Dsn: string;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport);
  try
    Transport.QueueInbound(BuildManagerFrame(MCP_MSG_STATUS_RESPONSE, nil));
    Transport.QueueInbound(BuildManagerFrame(MCP_MSG_AUTH_RESPONSE, BuildManagerAuthResponsePayloadSuccess));
    Transport.QueueInbound(BuildManagerFrame(MCP_MSG_CONNECT_RESPONSE, BuildManagerConnectResponsePayloadSuccess));
    Transport.QueueInbound(EncodeMessage(MSG_AUTH_REQUEST, BuildAuthRequestPayload(AUTH_PASSWORD), 0, 1, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_AUTH_OK, BuildAuthOkPayload, 0, 2, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 0, 0), 0, 3, nil, 0));

    Dsn := 'host=127.0.0.1;port=3092;database=testdb;user=alice;password=secret;front_door_mode=manager_proxy;manager_auth_token=token123';
    Client.Connect(Dsn);
    AssertTrue(Client.Connected, 'manager proxy connect should set connected');
    AssertEqualInt(5, Transport.WriteCount, 'expected manager + protocol write count');

    DecodeManagerFrame(Transport.WriteAt(0), ManagerMsgType, PayloadLength);
    AssertTrue(ManagerMsgType = $65, 'first manager frame should be HELLO');
    DecodeManagerFrame(Transport.WriteAt(1), ManagerMsgType, PayloadLength);
    AssertTrue(ManagerMsgType = $66, 'second manager frame should be AUTH_START');
    DecodeManagerFrame(Transport.WriteAt(2), ManagerMsgType, PayloadLength);
    AssertTrue(ManagerMsgType = $69, 'third manager frame should be DB_CONNECT');

    DecodeProtocolFrame(Transport.WriteAt(3), ProtocolMsgType, PayloadLength);
    AssertTrue(ProtocolMsgType = MSG_STARTUP, 'fourth frame should be STARTUP');
    DecodeProtocolFrame(Transport.WriteAt(4), ProtocolMsgType, PayloadLength);
    AssertTrue(ProtocolMsgType = MSG_AUTH_RESPONSE, 'fifth frame should be AUTH_RESPONSE');
  finally
    Client.Free;
  end;
end;

procedure TestManagerProxyConnectAuthFailureReturns28000;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  Dsn: string;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport);
  try
    Transport.QueueInbound(BuildManagerFrame(MCP_MSG_STATUS_RESPONSE, nil));
    Transport.QueueInbound(BuildManagerFrame(MCP_MSG_AUTH_RESPONSE, BuildManagerAuthResponsePayloadFailure('mcp auth denied')));

    Dsn := 'host=127.0.0.1;port=3092;database=testdb;user=alice;password=secret;front_door_mode=manager_proxy;manager_auth_token=token123';
    try
      Client.Connect(Dsn);
      Fail('expected manager proxy auth failure');
    except
      on E: EScratchbirdAuthError do
      begin
        AssertEqualString('28000', E.SQLState, 'manager proxy auth failure SQLSTATE');
        AssertTrue(Pos('mcp auth denied', LowerCase(E.Message)) > 0, 'manager proxy auth failure message');
      end;
    end;
    AssertTrue(not Client.Connected, 'client should remain disconnected on auth failure');
  finally
    Client.Free;
  end;
end;

begin
  try
    TestManagerProxyConnectSuccessWithPasswordAuth;
    TestManagerProxyConnectAuthFailureReturns28000;
    Writeln('ConnectionManagerProxyTests: OK');
  except
    on E: Exception do
    begin
      Writeln('ConnectionManagerProxyTests: FAILED - ' + E.Message);
      Halt(1);
    end;
  end;
end.
