// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

program ConnectionDirectAuthMatrixTests;

{$mode delphi}
{$APPTYPE CONSOLE}

uses
  {$IFDEF UNIX}
  cthreads,
  {$ENDIF}
  SysUtils,
  ScratchBird.Client, ScratchBird.Config, ScratchBird.Protocol, ScratchBird.Transport;

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

function ReadUInt64LEAt(const Buffer: TBytes; Offset: Integer): UInt64;
begin
  Result := UInt64(Buffer[Offset]) or
    (UInt64(Buffer[Offset + 1]) shl 8) or
    (UInt64(Buffer[Offset + 2]) shl 16) or
    (UInt64(Buffer[Offset + 3]) shl 24) or
    (UInt64(Buffer[Offset + 4]) shl 32) or
    (UInt64(Buffer[Offset + 5]) shl 40) or
    (UInt64(Buffer[Offset + 6]) shl 48) or
    (UInt64(Buffer[Offset + 7]) shl 56);
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

procedure TestDirectPasswordAuthConnect;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  MsgType: TScratchBirdMessageType;
  PayloadLength: Integer;
  Payload: TBytes;
  Dsn: string;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport);
  try
    Transport.QueueInbound(EncodeMessage(MSG_AUTH_REQUEST, BuildAuthRequestPayload(AUTH_PASSWORD), 0, 1, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_AUTH_OK, BuildAuthOkPayload, 0, 2, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 0, 0), 0, 3, nil, 0));

    Dsn := 'host=127.0.0.1;port=3092;database=testdb;user=alice;password=secret;front_door_mode=direct';
    Client.Connect(Dsn);
    AssertTrue(Client.Connected, 'direct password auth should connect');
    AssertEqualInt(2, Transport.WriteCount, 'direct password write count');
    DecodeProtocolFrame(Transport.WriteAt(0), MsgType, PayloadLength, Payload);
    AssertTrue(MsgType = MSG_STARTUP, 'direct password first write should be STARTUP');
    DecodeProtocolFrame(Transport.WriteAt(1), MsgType, PayloadLength, Payload);
    AssertTrue(MsgType = MSG_AUTH_RESPONSE, 'direct password second write should be AUTH_RESPONSE');
  finally
    Client.Free;
  end;
end;

procedure TestDirectScramAuthConnect;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  MsgType: TScratchBirdMessageType;
  PayloadLength: Integer;
  Payload: TBytes;
  Dsn: string;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport);
  try
    Transport.QueueInbound(EncodeMessage(MSG_AUTH_REQUEST, BuildAuthRequestPayload(AUTH_SCRAM_SHA256), 0, 1, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_AUTH_OK, BuildAuthOkPayload, 0, 2, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 0, 0), 0, 3, nil, 0));

    Dsn := 'host=127.0.0.1;port=3092;database=testdb;user=alice;password=secret;front_door_mode=direct';
    Client.Connect(Dsn);
    AssertTrue(Client.Connected, 'direct scram auth should connect');
    AssertEqualInt(2, Transport.WriteCount, 'direct scram write count');
    DecodeProtocolFrame(Transport.WriteAt(0), MsgType, PayloadLength, Payload);
    AssertTrue(MsgType = MSG_STARTUP, 'direct scram first write should be STARTUP');
    DecodeProtocolFrame(Transport.WriteAt(1), MsgType, PayloadLength, Payload);
    AssertTrue(MsgType = MSG_AUTH_RESPONSE, 'direct scram second write should be AUTH_RESPONSE');
  finally
    Client.Free;
  end;
end;

procedure TestDirectConnectAllowsBinaryTransferFalseAndCompressionZstd;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  MsgType: TScratchBirdMessageType;
  PayloadLength: Integer;
  Payload: TBytes;
  Features: UInt64;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport);
  try
    Transport.QueueInbound(EncodeMessage(MSG_AUTH_REQUEST, BuildAuthRequestPayload(AUTH_PASSWORD), 0, 1, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_AUTH_OK, BuildAuthOkPayload, 0, 2, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 0, 0), 0, 3, nil, 0));

    Client.Connect('scratchbird://alice:secret@127.0.0.1:3092/testdb?front_door_mode=direct&sslmode=disable&binary_transfer=false&compression=zstd');
    AssertTrue(Client.Connected, 'compatibility mode connect should succeed');
    AssertEqualInt(2, Transport.WriteCount, 'compatibility mode write count');
    DecodeProtocolFrame(Transport.WriteAt(0), MsgType, PayloadLength, Payload);
    AssertTrue(MsgType = MSG_STARTUP, 'compatibility mode first write should be STARTUP');
    AssertTrue(Length(Payload) >= 12, 'startup payload should include feature flags');
    Features := ReadUInt64LEAt(Payload, 4);
    AssertTrue((Features and FEATURE_COMPRESSION) = FEATURE_COMPRESSION, 'compression feature should be enabled');
    AssertTrue((Features and FEATURE_STREAMING) = 0, 'streaming feature should be disabled when binary_transfer=false');
  finally
    Client.Free;
  end;
end;

begin
  try
    TestDirectPasswordAuthConnect;
    TestDirectScramAuthConnect;
    TestDirectConnectAllowsBinaryTransferFalseAndCompressionZstd;
    Writeln('ConnectionDirectAuthMatrixTests: OK');
  except
    on E: Exception do
    begin
      Writeln('ConnectionDirectAuthMatrixTests: FAILED - ' + E.Message);
      Halt(1);
    end;
  end;
end.
