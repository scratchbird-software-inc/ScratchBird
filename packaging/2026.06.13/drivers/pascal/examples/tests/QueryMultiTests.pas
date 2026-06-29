// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

program QueryMultiTests;

{$mode delphi}
{$APPTYPE CONSOLE}

uses
  {$IFDEF UNIX}
  cthreads,
  {$ENDIF}
  SysUtils,
  ScratchBird.Client, ScratchBird.Config, ScratchBird.Errors, ScratchBird.Protocol, ScratchBird.Transport, ScratchBird.Types;

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

procedure AppendUInt16LE(var Buffer: TBytes; Value: Word);
var
  Start: Integer;
begin
  Start := Length(Buffer);
  SetLength(Buffer, Start + 2);
  Buffer[Start] := Byte(Value and $FF);
  Buffer[Start + 1] := Byte((Value shr 8) and $FF);
end;

procedure AppendUInt32LE(var Buffer: TBytes; Value: Cardinal);
var
  Start: Integer;
begin
  Start := Length(Buffer);
  SetLength(Buffer, Start + 4);
  Buffer[Start] := Byte(Value and $FF);
  Buffer[Start + 1] := Byte((Value shr 8) and $FF);
  Buffer[Start + 2] := Byte((Value shr 16) and $FF);
  Buffer[Start + 3] := Byte((Value shr 24) and $FF);
end;

procedure AppendBytes(var Buffer: TBytes; const Bytes: TBytes);
var
  Start, Count: Integer;
begin
  Count := Length(Bytes);
  if Count = 0 then
    Exit;
  Start := Length(Buffer);
  SetLength(Buffer, Start + Count);
  Move(Bytes[0], Buffer[Start], Count);
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

function BuildRowDescriptionPayloadOneIntColumn(const Name: string): TBytes;
var
  NameBytes: TBytes;
begin
  NameBytes := TEncoding.UTF8.GetBytes(Name);
  Result := nil;
  AppendUInt16LE(Result, 1);
  AppendUInt16LE(Result, 0);
  AppendUInt32LE(Result, Cardinal(Length(NameBytes)));
  AppendBytes(Result, NameBytes);
  AppendUInt32LE(Result, 0);
  AppendUInt16LE(Result, 1);
  AppendUInt32LE(Result, OID_INT4);
  AppendUInt16LE(Result, 4);
  AppendUInt32LE(Result, Cardinal($FFFFFFFF));
  AppendBytes(Result, TBytes.Create(FORMAT_BINARY, 0, 0, 0));
end;

function BuildDataRowPayloadOneInt(Value: Integer): TBytes;
var
  ValueBytes: TBytes;
begin
  SetLength(ValueBytes, 4);
  ValueBytes[0] := Byte(Cardinal(Value) and $FF);
  ValueBytes[1] := Byte((Cardinal(Value) shr 8) and $FF);
  ValueBytes[2] := Byte((Cardinal(Value) shr 16) and $FF);
  ValueBytes[3] := Byte((Cardinal(Value) shr 24) and $FF);
  Result := nil;
  AppendUInt16LE(Result, 1);
  AppendUInt16LE(Result, 1);
  AppendBytes(Result, TBytes.Create(0));
  AppendUInt32LE(Result, 4);
  AppendBytes(Result, ValueBytes);
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

procedure TestQueryMultiCollectsRowsetsAndSummaries;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  Rowsets: TScratchBirdRowsets;
  MsgType: TScratchBirdMessageType;
  Payload: TBytes;
  Statements: array[0..1] of string;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport, True);
  try
    Statements[0] := 'SELECT id FROM t';
    Statements[1] := 'INSERT INTO t(id) VALUES (1)';

    Transport.QueueInbound(EncodeMessage(MSG_ROW_DESCRIPTION, BuildRowDescriptionPayloadOneIntColumn('id'), 0, 1, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_DATA_ROW, BuildDataRowPayloadOneInt(7), 0, 2, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_COMMAND_COMPLETE, BuildCommandCompletePayload(0, 1, 0, 'SELECT 1'), 0, 3, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 0, 0), 0, 4, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_COMMAND_COMPLETE, BuildCommandCompletePayload(0, 1, 15, 'INSERT 0 1'), 0, 5, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 0, 0), 0, 6, nil, 0));

    Rowsets := Client.QueryMulti(Statements);
    AssertEqualInt(2, Length(Rowsets), 'multi result count');

    AssertEqualInt(1, Length(Rowsets[0].Columns), 'first rowset columns');
    AssertEqualString('id', Rowsets[0].Columns[0].Name, 'first column name');
    AssertEqualInt(1, Length(Rowsets[0].Rows), 'first rowset row count');
    AssertEqualInt(7, Integer(Rowsets[0].Rows[0][0]), 'first rowset value');
    AssertEqualInt64(1, Rowsets[0].RowsAffected, 'first rowset rows affected');
    AssertEqualString('SELECT 1', Rowsets[0].CommandTag, 'first rowset command tag');
    AssertTrue(not Rowsets[0].HasLastInsertId, 'first rowset generated key absent');

    AssertEqualInt(0, Length(Rowsets[1].Rows), 'second rowset row count');
    AssertEqualInt64(1, Rowsets[1].RowsAffected, 'second rowset rows affected');
    AssertEqualString('INSERT 0 1', Rowsets[1].CommandTag, 'second rowset command tag');
    AssertTrue(Rowsets[1].HasLastInsertId, 'second rowset generated key present');
    AssertEqualUInt64(15, Rowsets[1].LastInsertId, 'second rowset generated key');

    AssertEqualInt(2, Transport.WriteCount, 'multi write count');
    DecodeOutboundFrame(Transport.WriteAt(0), MsgType, Payload);
    AssertTrue(MsgType = MSG_QUERY, 'first multi message type');
    AssertEqualBytes(BuildQueryPayload(Statements[0], 0, 0, 0), Payload, 'first query payload');
    DecodeOutboundFrame(Transport.WriteAt(1), MsgType, Payload);
    AssertTrue(MsgType = MSG_QUERY, 'second multi message type');
    AssertEqualBytes(BuildQueryPayload(Statements[1], 0, 0, 0), Payload, 'second query payload');
  finally
    Client.Free;
  end;
end;

procedure TestQueryMultiRejectsBlankSql;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  Statements: array[0..0] of string;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport, True);
  try
    Statements[0] := '   ';
    try
      Client.QueryMulti(Statements);
      Fail('expected blank SQL guard in query multi');
    except
      on E: EScratchbirdSyntaxError do
        AssertEqualString('42601', E.SQLState, 'query multi blank SQL SQLSTATE');
    end;
  finally
    Client.Free;
  end;
end;

begin
  try
    TestQueryMultiCollectsRowsetsAndSummaries;
    TestQueryMultiRejectsBlankSql;
    Writeln('QueryMultiTests: OK');
  except
    on E: Exception do
    begin
      Writeln('QueryMultiTests: FAILED - ' + E.Message);
      Halt(1);
    end;
  end;
end.
