// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

program StreamControlBackpressureTests;

{$mode delphi}
{$APPTYPE CONSOLE}

uses
  {$IFDEF UNIX}
  cthreads,
  {$ENDIF}
  SysUtils, Variants,
  ScratchBird.Client, ScratchBird.Config, ScratchBird.Protocol, ScratchBird.Sql, ScratchBird.Transport;

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

procedure AssertEqualInt64(Expected, Actual: Int64; const MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText + ': expected=' + IntToStr(Expected) + ' actual=' + IntToStr(Actual));
end;

procedure AssertEqualUInt64(Expected, Actual: UInt64; const MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText + ': expected=' + IntToStr(Int64(Expected)) + ' actual=' + IntToStr(Int64(Actual)));
end;

procedure AssertEqualString(const Expected, Actual, MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText + ': expected="' + Expected + '" actual="' + Actual + '"');
end;

procedure AssertEqualBytes(const Expected, Actual: TBytes; const MessageText: string);
var
  I: Integer;
begin
  if Length(Expected) <> Length(Actual) then
    Fail(MessageText + ': expected length=' + IntToStr(Length(Expected)) + ' actual length=' + IntToStr(Length(Actual)));
  for I := 0 to High(Expected) do
    if Expected[I] <> Actual[I] then
      Fail(MessageText + ': mismatch at index ' + IntToStr(I));
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

function BuildCommandCompletePayload(CommandType: Byte; Rows, LastId: UInt64; const Tag: string): TBytes;
var
  TagBytes: TBytes;
begin
  TagBytes := TEncoding.UTF8.GetBytes(Tag);
  SetLength(Result, 20 + Length(TagBytes) + 1);
  FillChar(Result[0], Length(Result), 0);
  Result[0] := CommandType;
  WriteUInt64LEAt(Result, 4, Rows);
  WriteUInt64LEAt(Result, 12, LastId);
  if Length(TagBytes) > 0 then
    Move(TagBytes[0], Result[20], Length(TagBytes));
  Result[20 + Length(TagBytes)] := 0;
end;

procedure DecodeOutboundFrame(const Frame: TBytes; out MsgType: TScratchBirdMessageType; out Payload: TBytes);
var
  Header: TBytes;
  Flags: Byte;
  PayloadLength: Integer;
  Sequence: Cardinal;
  AttachmentId: TBytes;
  TxnId: UInt64;
begin
  AssertTrue(Length(Frame) >= HEADER_SIZE, 'outbound frame must include header');
  Header := Copy(Frame, 0, HEADER_SIZE);
  AssertTrue(DecodeHeader(Header, MsgType, Flags, PayloadLength, Sequence, AttachmentId, TxnId), 'outbound header decode');
  AssertEqualInt(HEADER_SIZE + PayloadLength, Length(Frame), 'outbound frame length');
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

procedure TestStreamControlWritesEncodedWindowMessage;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  MsgType: TScratchBirdMessageType;
  Payload, Expected: TBytes;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport);
  try
    Client.StreamControl(STREAM_RESUME, 4096, 2500);
    AssertEqualInt(1, Transport.WriteCount, 'stream control write count');
    DecodeOutboundFrame(Transport.WriteAt(0), MsgType, Payload);
    AssertTrue(MsgType = MSG_STREAM_CONTROL, 'stream control message type');
    Expected := BuildStreamControlPayload(STREAM_RESUME, 4096, 2500);
    AssertEqualBytes(Expected, Payload, 'stream control payload');
  finally
    Client.Free;
  end;
end;

procedure TestPortalSuspendedTriggersExecuteResume;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  Stream: TScratchBirdResultStream;
  Row: TArray<Variant>;
  MsgType: TScratchBirdMessageType;
  Payload, Expected: TBytes;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport);
  try
    Transport.QueueInbound(EncodeMessage(MSG_PORTAL_SUSPENDED, nil, 0, 1, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_COMMAND_COMPLETE, BuildCommandCompletePayload(0, 0, 0, 'SELECT 0'), 0, 2, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 0, 0), 0, 3, nil, 0));

    Stream := TScratchBirdResultStream.Create(Client);
    try
      Row := Stream.ReadRow;
      AssertTrue(Row = nil, 'portal suspended resume should complete with no row');
      AssertEqualString('SELECT 0', Stream.CommandTag, 'portal suspended command tag');
      AssertEqualInt64(0, Stream.RowsAffected, 'portal suspended rows affected');
    finally
      Stream.Free;
    end;

    AssertEqualInt(1, Transport.WriteCount, 'portal suspended write count');
    DecodeOutboundFrame(Transport.WriteAt(0), MsgType, Payload);
    AssertTrue(MsgType = MSG_EXECUTE, 'portal suspended resume message type');
    Expected := BuildExecutePayload('', 0);
    AssertEqualBytes(Expected, Payload, 'portal suspended execute payload');
  finally
    Client.Free;
  end;
end;

procedure TestResultStreamCapturesLastInsertId;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  Stream: TScratchBirdResultStream;
  Row: TArray<Variant>;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport);
  try
    Transport.QueueInbound(EncodeMessage(MSG_COMMAND_COMPLETE, BuildCommandCompletePayload(0, 1, 4242, 'INSERT 0 1'), 0, 1, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 0, 0), 0, 2, nil, 0));

    Stream := TScratchBirdResultStream.Create(Client);
    try
      Row := Stream.ReadRow;
      AssertTrue(Row = nil, 'insert command stream should complete with no row');
      AssertEqualInt64(1, Stream.RowsAffected, 'insert rows affected');
      AssertEqualString('INSERT 0 1', Stream.CommandTag, 'insert command tag');
      AssertTrue(Stream.HasLastInsertId, 'insert stream should expose last insert id');
      AssertEqualUInt64(4242, Stream.LastInsertId, 'insert stream last insert id');
    finally
      Stream.Free;
    end;
  finally
    Client.Free;
  end;
end;

procedure TestResultStreamIgnoresNoticeMessages;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  Stream: TScratchBirdResultStream;
  Row: TArray<Variant>;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport);
  try
    Transport.QueueInbound(EncodeMessage(MSG_NOTICE, nil, 0, 1, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_COMMAND_COMPLETE, BuildCommandCompletePayload(0, 0, 0, 'SELECT 0'), 0, 2, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 0, 0), 0, 3, nil, 0));

    Stream := TScratchBirdResultStream.Create(Client);
    try
      Row := Stream.ReadRow;
      AssertTrue(Row = nil, 'notice-prefixed stream should complete with no row');
      AssertEqualInt64(0, Stream.RowsAffected, 'notice-prefixed rows affected');
      AssertEqualString('SELECT 0', Stream.CommandTag, 'notice-prefixed command tag');
    finally
      Stream.Free;
    end;
  finally
    Client.Free;
  end;
end;

begin
  try
    TestStreamControlWritesEncodedWindowMessage;
    TestPortalSuspendedTriggersExecuteResume;
    TestResultStreamCapturesLastInsertId;
    TestResultStreamIgnoresNoticeMessages;
    Writeln('StreamControlBackpressureTests: OK');
  except
    on E: Exception do
    begin
      Writeln('StreamControlBackpressureTests: FAILED - ' + E.Message);
      Halt(1);
    end;
  end;
end.
