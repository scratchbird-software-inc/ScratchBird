// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

program TxnStateTransitionsTests;

{$mode delphi}
{$APPTYPE CONSOLE}

uses
  {$IFDEF UNIX}
  cthreads,
  {$ENDIF}
  SysUtils,
  ScratchBird.Client, ScratchBird.Config, ScratchBird.Errors, ScratchBird.Protocol, ScratchBird.Transport;

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

procedure AssertContains(const Needle, Haystack, MessageText: string);
begin
  if Pos(Needle, Haystack) = 0 then
    Fail(MessageText + ': missing substring "' + Needle + '"');
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

procedure AppendErrorField(var Payload: TBytes; FieldTag: Byte; const Value: string);
var
  ValueBytes: TBytes;
  Start: Integer;
begin
  Start := Length(Payload);
  SetLength(Payload, Start + 1);
  Payload[Start] := FieldTag;
  ValueBytes := TEncoding.UTF8.GetBytes(Value);
  Start := Length(Payload);
  SetLength(Payload, Start + Length(ValueBytes) + 1);
  if Length(ValueBytes) > 0 then
    Move(ValueBytes[0], Payload[Start], Length(ValueBytes));
  Payload[Start + Length(ValueBytes)] := 0;
end;

function BuildErrorPayload(const Severity, SqlState, MessageText, DetailText, HintText: string): TBytes;
begin
  SetLength(Result, 0);
  AppendErrorField(Result, Byte(AnsiChar('S')), Severity);
  AppendErrorField(Result, Byte(AnsiChar('C')), SqlState);
  AppendErrorField(Result, Byte(AnsiChar('M')), MessageText);
  if DetailText <> '' then
    AppendErrorField(Result, Byte(AnsiChar('D')), DetailText);
  if HintText <> '' then
    AppendErrorField(Result, Byte(AnsiChar('H')), HintText);
  SetLength(Result, Length(Result) + 1);
  Result[High(Result)] := 0;
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

procedure WriteUInt32LEAt(var Buffer: TBytes; Offset: Integer; Value: Cardinal);
begin
  Buffer[Offset] := Byte(Value and $FF);
  Buffer[Offset + 1] := Byte((Value shr 8) and $FF);
  Buffer[Offset + 2] := Byte((Value shr 16) and $FF);
  Buffer[Offset + 3] := Byte((Value shr 24) and $FF);
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

function BuildParameterStatusPayload(const Name, Value: string): TBytes;
var
  NameBytes, ValueBytes: TBytes;
  Offset: Integer;
begin
  NameBytes := TEncoding.UTF8.GetBytes(Name);
  ValueBytes := TEncoding.UTF8.GetBytes(Value);
  SetLength(Result, 8 + Length(NameBytes) + Length(ValueBytes));
  FillChar(Result[0], Length(Result), 0);
  Offset := 0;
  WriteUInt32LEAt(Result, Offset, Length(NameBytes));
  Inc(Offset, 4);
  if Length(NameBytes) > 0 then
  begin
    Move(NameBytes[0], Result[Offset], Length(NameBytes));
    Inc(Offset, Length(NameBytes));
  end;
  WriteUInt32LEAt(Result, Offset, Length(ValueBytes));
  Inc(Offset, 4);
  if Length(ValueBytes) > 0 then
    Move(ValueBytes[0], Result[Offset], Length(ValueBytes));
end;

function BuildQueryPlanPayload(Format: Cardinal; PlanningTimeUs, EstimatedRows,
  EstimatedCost: UInt64; const PlanBytes: TBytes): TBytes;
var
  Offset: Integer;
begin
  SetLength(Result, 32 + Length(PlanBytes));
  FillChar(Result[0], Length(Result), 0);
  Offset := 0;
  WriteUInt32LEAt(Result, Offset, Format);
  Inc(Offset, 4);
  WriteUInt32LEAt(Result, Offset, Length(PlanBytes));
  Inc(Offset, 4);
  WriteUInt64LEAt(Result, Offset, PlanningTimeUs);
  Inc(Offset, 8);
  WriteUInt64LEAt(Result, Offset, EstimatedRows);
  Inc(Offset, 8);
  WriteUInt64LEAt(Result, Offset, EstimatedCost);
  Inc(Offset, 8);
  if Length(PlanBytes) > 0 then
    Move(PlanBytes[0], Result[Offset], Length(PlanBytes));
end;

function BuildSblrCompiledPayload(Hash: UInt64; Version: Cardinal;
  const Bytecode: TBytes): TBytes;
var
  Offset: Integer;
begin
  SetLength(Result, 16 + Length(Bytecode));
  FillChar(Result[0], Length(Result), 0);
  Offset := 0;
  WriteUInt64LEAt(Result, Offset, Hash);
  Inc(Offset, 8);
  WriteUInt32LEAt(Result, Offset, Version);
  Inc(Offset, 4);
  WriteUInt32LEAt(Result, Offset, Length(Bytecode));
  Inc(Offset, 4);
  if Length(Bytecode) > 0 then
    Move(Bytecode[0], Result[Offset], Length(Bytecode));
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
  AssertTrue(Length(Frame) >= HEADER_SIZE, 'outbound frame includes header');
  Header := Copy(Frame, 0, HEADER_SIZE);
  AssertTrue(DecodeHeader(Header, MsgType, Flags, PayloadLength, Sequence, AttachmentId, TxnId), 'decode outbound header');
  AssertEqualInt(HEADER_SIZE + PayloadLength, Length(Frame), 'outbound frame length');
  Payload := Copy(Frame, HEADER_SIZE, PayloadLength);
end;

procedure DecodeOutboundType(const Frame: TBytes; out MsgType: TScratchBirdMessageType);
var
  Payload: TBytes;
begin
  DecodeOutboundFrame(Frame, MsgType, Payload);
end;

procedure TestBeginSavepointCommitLifecycleTransitions;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  MsgType: TScratchBirdMessageType;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport, True);
  try
    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(1, 41, 0), 0, 1, nil, 41));
    Client.BeginTransaction;

    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(1, 41, 0), 0, 2, nil, 41));
    Client.Savepoint('sp_a');

    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(1, 41, 0), 0, 3, nil, 41));
    Client.ReleaseSavepoint('sp_a');

    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(1, 41, 0), 0, 4, nil, 41));
    Client.RollbackToSavepoint('sp_a');

    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(1, 0, 0), 0, 5, nil, 0));
    Client.Commit;

    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(1, 0, 0), 0, 6, nil, 0));
    Client.Savepoint('sp_after_commit');

    AssertEqualInt(6, Transport.WriteCount, 'commit lifecycle write count');
    DecodeOutboundType(Transport.WriteAt(0), MsgType);
    AssertTrue(MsgType = MSG_TXN_BEGIN, 'first write should be txn begin');
    DecodeOutboundType(Transport.WriteAt(1), MsgType);
    AssertTrue(MsgType = MSG_TXN_SAVEPOINT, 'second write should be savepoint');
    DecodeOutboundType(Transport.WriteAt(2), MsgType);
    AssertTrue(MsgType = MSG_TXN_RELEASE, 'third write should be release savepoint');
    DecodeOutboundType(Transport.WriteAt(3), MsgType);
    AssertTrue(MsgType = MSG_TXN_ROLLBACK_TO, 'fourth write should be rollback to savepoint');
    DecodeOutboundType(Transport.WriteAt(4), MsgType);
    AssertTrue(MsgType = MSG_TXN_COMMIT, 'fifth write should be txn commit');
    DecodeOutboundType(Transport.WriteAt(5), MsgType);
    AssertTrue(MsgType = MSG_TXN_SAVEPOINT, 'sixth write should be savepoint in auto-started txn');
  finally
    Client.Free;
  end;
end;

procedure TestBeginRollbackStartsNextTxnState;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  MsgType: TScratchBirdMessageType;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport, True);
  try
    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(1, 77, 0), 0, 1, nil, 77));
    Client.BeginTransaction;

    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(1, 0, 0), 0, 2, nil, 0));
    Client.Rollback;

    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(1, 0, 0), 0, 3, nil, 0));
    Client.Savepoint('sp_after_rollback');

    AssertEqualInt(3, Transport.WriteCount, 'rollback lifecycle write count');
    DecodeOutboundType(Transport.WriteAt(0), MsgType);
    AssertTrue(MsgType = MSG_TXN_BEGIN, 'first write should be txn begin');
    DecodeOutboundType(Transport.WriteAt(1), MsgType);
    AssertTrue(MsgType = MSG_TXN_ROLLBACK, 'second write should be txn rollback');
    DecodeOutboundType(Transport.WriteAt(2), MsgType);
    AssertTrue(MsgType = MSG_TXN_SAVEPOINT, 'third write should be savepoint in auto-started txn');
  finally
    Client.Free;
  end;
end;

procedure TestRuntimeFreshBoundaryAdoptsDefaultBeginAndRejectsNonDefault;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  MsgType: TScratchBirdMessageType;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport, True);
  try
    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(1, 0, 0), 0, 1, nil, 0));
    Client.Ping;

    try
      Client.BeginTransactionEx(ISOLATION_SERIALIZABLE, 0, False, False, 0, 0, 0);
      Fail('fresh native boundary should reject non-default adoption');
    except
      on E: EScratchbirdNotSupported do
      begin
        AssertEqualString('0A000', E.SQLState, 'fresh-boundary adoption SQLSTATE');
        AssertContains('fresh native transaction boundaries only support default READ COMMITTED adoption',
          E.Message, 'fresh-boundary adoption message');
      end;
    end;

    Client.BeginTransaction;

    AssertEqualInt(1, Transport.WriteCount, 'default fresh-boundary adoption should not emit txn begin');
    DecodeOutboundType(Transport.WriteAt(0), MsgType);
    AssertTrue(MsgType = MSG_PING, 'fresh-boundary proof should only emit ping');
  finally
    Client.Free;
  end;
end;

procedure TestBeginTransactionExOptionMatrixEncodesPayload;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  MsgType: TScratchBirdMessageType;
  Payload: TBytes;
  ExpectedPayload: TBytes;
  Flags: Word;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport, True);
  try
    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 9001, 0), 0, 1, nil, 9001));
    Client.BeginTransactionEx(ISOLATION_SERIALIZABLE, 1, True, True, 250, 1, 2);

    Flags := TXN_FLAG_HAS_ISOLATION or TXN_FLAG_HAS_ACCESS or TXN_FLAG_HAS_DEFERRABLE or
      TXN_FLAG_HAS_WAIT or TXN_FLAG_HAS_TIMEOUT or TXN_FLAG_HAS_AUTOCOMMIT;
    ExpectedPayload := BuildTxnBeginPayload(Flags, 2, 1, ISOLATION_SERIALIZABLE, 1, 1, 1, 250);
    DecodeOutboundFrame(Transport.WriteAt(0), MsgType, Payload);
    AssertTrue(MsgType = MSG_TXN_BEGIN, 'full matrix write should be txn begin');
    AssertEqualBytes(ExpectedPayload, Payload, 'full matrix begin payload');

    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 0, 0), 0, 2, nil, 0));
    Client.Commit;

    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 9002, 0), 0, 3, nil, 9002));
    Client.BeginTransactionEx(ISOLATION_REPEATABLE_READ, 0, False, False, 0, 0, 0);

    Flags := TXN_FLAG_HAS_ISOLATION;
    ExpectedPayload := BuildTxnBeginPayload(Flags, 0, 0, ISOLATION_REPEATABLE_READ, 0, 0, 0, 0);
    DecodeOutboundFrame(Transport.WriteAt(2), MsgType, Payload);
    AssertTrue(MsgType = MSG_TXN_BEGIN, 'minimal matrix write should be txn begin');
    AssertEqualBytes(ExpectedPayload, Payload, 'minimal matrix begin payload');

    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 0, 0), 0, 4, nil, 0));
    Client.Rollback;

    AssertEqualInt(4, Transport.WriteCount, 'option matrix lifecycle write count');
    DecodeOutboundType(Transport.WriteAt(1), MsgType);
    AssertTrue(MsgType = MSG_TXN_COMMIT, 'second write should be commit');
    DecodeOutboundType(Transport.WriteAt(3), MsgType);
    AssertTrue(MsgType = MSG_TXN_ROLLBACK, 'fourth write should be rollback');
  finally
    Client.Free;
  end;
end;

procedure TestBeginTransactionExConflictPathRetainsTxnAvailability;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  MsgType: TScratchBirdMessageType;
  Payload: TBytes;
  ExpectedPayload: TBytes;
  ErrorPayload: TBytes;
  Flags: Word;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport, True);
  try
    ErrorPayload := BuildErrorPayload(
      'ERROR', '40001', 'serialization failure during begin', 'conflicting transaction',
      'retry the transaction');
    Transport.QueueInbound(EncodeMessage(MSG_ERROR, ErrorPayload, 0, 1, nil, 0));

    try
      Client.BeginTransactionEx(ISOLATION_SERIALIZABLE, 1, True, False, 0, 0, 2);
      Fail('BeginTransactionEx conflict path should raise transaction error');
    except
      on E: EScratchbirdTransactionError do
      begin
        AssertEqualString('40001', E.SQLState, 'begin conflict SQLSTATE');
        AssertTrue(Pos('serialization failure during begin', E.Message) > 0,
          'begin conflict message should round-trip');
      end;
    end;

    Flags := TXN_FLAG_HAS_ISOLATION or TXN_FLAG_HAS_ACCESS or TXN_FLAG_HAS_DEFERRABLE;
    ExpectedPayload := BuildTxnBeginPayload(Flags, 2, 0, ISOLATION_SERIALIZABLE, 1, 1, 0, 0);
    DecodeOutboundFrame(Transport.WriteAt(0), MsgType, Payload);
    AssertTrue(MsgType = MSG_TXN_BEGIN, 'conflict path first write should be txn begin');
    AssertEqualBytes(ExpectedPayload, Payload, 'conflict path begin payload');

    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 0, 0), 0, 2, nil, 0));
    Client.Savepoint('sp_after_conflict');

    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 9010, 0), 0, 3, nil, 9010));
    Client.BeginTransactionEx(ISOLATION_READ_COMMITTED, 0, False, False, 0, 0, 0);

    Flags := TXN_FLAG_HAS_ISOLATION;
    ExpectedPayload := BuildTxnBeginPayload(Flags, 0, 0, ISOLATION_READ_COMMITTED, 0, 0, 0, 0);
    DecodeOutboundType(Transport.WriteAt(1), MsgType);
    AssertTrue(MsgType = MSG_TXN_SAVEPOINT, 'second write should be savepoint after failed begin');

    DecodeOutboundFrame(Transport.WriteAt(2), MsgType, Payload);
    AssertTrue(MsgType = MSG_TXN_BEGIN, 'third write should be retry begin');
    AssertEqualBytes(ExpectedPayload, Payload, 'retry begin payload');

    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 0, 0), 0, 4, nil, 0));
    Client.Rollback;

    AssertEqualInt(4, Transport.WriteCount, 'conflict path lifecycle write count');
    DecodeOutboundType(Transport.WriteAt(3), MsgType);
    AssertTrue(MsgType = MSG_TXN_ROLLBACK, 'fourth write should be rollback after retry begin');
  finally
    Client.Free;
  end;
end;

procedure TestBeginTransactionExReadCommittedModePayloadAndValidation;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  MsgType: TScratchBirdMessageType;
  Payload: TBytes;
  ExpectedPayload: TBytes;
  Flags: Word;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport, True);
  try
    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 9100, 0), 0, 1, nil, 9100));
    Client.BeginTransactionEx(ISOLATION_READ_COMMITTED, 0, False, False, 25, 0, 0,
      READ_COMMITTED_MODE_READ_CONSISTENCY);

    Flags := TXN_FLAG_HAS_ISOLATION or TXN_FLAG_HAS_TIMEOUT or TXN_FLAG_HAS_READ_COMMITTED_MODE;
    ExpectedPayload := BuildTxnBeginPayload(Flags, 0, 0, ISOLATION_READ_COMMITTED, 0, 0, 0, 25,
      READ_COMMITTED_MODE_READ_CONSISTENCY);
    DecodeOutboundFrame(Transport.WriteAt(0), MsgType, Payload);
    AssertTrue(MsgType = MSG_TXN_BEGIN, 'read committed mode write should be txn begin');
    AssertEqualBytes(ExpectedPayload, Payload, 'read committed mode begin payload');

    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 0, 0), 0, 2, nil, 0));
    Client.Rollback;

    try
      Client.BeginTransactionEx(ISOLATION_SERIALIZABLE, 0, False, False, 0, 0, 0,
        READ_COMMITTED_MODE_READ_CONSISTENCY);
      Fail('BeginTransactionEx should reject read committed mode with snapshot aliases');
    except
      on E: EScratchbirdNotSupported do
      begin
        AssertEqualString('0A000', E.SQLState, 'read committed mode validation SQLSTATE');
        AssertTrue(Pos('READ COMMITTED isolation alias', E.Message) > 0,
          'read committed mode validation message');
      end;
    end;

    AssertEqualInt(2, Transport.WriteCount, 'validation failure should not emit extra writes');
  finally
    Client.Free;
  end;
end;

procedure TestDisconnectClearsAbandonedSessionState;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  Stream: TScratchBirdResultStream;
  Diagnostics: string;
  PlanBytes: TBytes;
  Bytecode: TBytes;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport, True);
  try
    SetLength(PlanBytes, 3);
    PlanBytes[0] := 1;
    PlanBytes[1] := 2;
    PlanBytes[2] := 3;
    SetLength(Bytecode, 3);
    Bytecode[0] := 4;
    Bytecode[1] := 5;
    Bytecode[2] := 6;

    Transport.QueueInbound(EncodeMessage(MSG_PARAMETER_STATUS,
      BuildParameterStatusPayload('attachment_id', '00112233-4455-6677-8899-aabbccddeeff'),
      0, 1, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_PARAMETER_STATUS,
      BuildParameterStatusPayload('current_txn_id', '42'),
      0, 2, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_QUERY_PLAN,
      BuildQueryPlanPayload(1, 2, 3, 4, PlanBytes),
      0, 3, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_SBLR_COMPILED,
      BuildSblrCompiledPayload(5, 6, Bytecode),
      0, 4, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_COMMAND_COMPLETE,
      BuildCommandCompletePayload(0, 0, 0, 'SELECT 0'),
      0, 5, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_READY,
      BuildReadyPayload(1, 42, 0),
      0, 6, nil, 42));

    Stream := Client.ExecuteQuery('select 1');
    try
      while Stream.ReadRow <> nil do
      begin
      end;
    finally
      Stream.Free;
    end;

    Diagnostics := Client.GetDiagnosticsJson;
    AssertContains('"connected":true', Diagnostics, 'pre-disconnect connected state');
    AssertContains('"transaction_active":true', Diagnostics, 'pre-disconnect txn state');
    AssertContains('"attachment_zeroed":false', Diagnostics, 'pre-disconnect attachment state');
    AssertContains('"parameter_count":2', Diagnostics, 'pre-disconnect parameter count');
    AssertContains('"has_last_plan":true', Diagnostics, 'pre-disconnect plan state');
    AssertContains('"has_last_sblr":true', Diagnostics, 'pre-disconnect sblr state');
    AssertContains('"next_sequence":1', Diagnostics, 'pre-disconnect sequence state');

    Client.Disconnect;

    Diagnostics := Client.GetDiagnosticsJson;
    AssertContains('"connected":false', Diagnostics, 'post-disconnect connected state');
    AssertContains('"transaction_active":false', Diagnostics, 'post-disconnect txn state');
    AssertContains('"attachment_zeroed":true', Diagnostics, 'post-disconnect attachment state');
    AssertContains('"parameter_count":0', Diagnostics, 'post-disconnect parameter count');
    AssertContains('"has_last_plan":false', Diagnostics, 'post-disconnect plan state');
    AssertContains('"has_last_sblr":false', Diagnostics, 'post-disconnect sblr state');
    AssertContains('"next_sequence":0', Diagnostics, 'post-disconnect sequence state');
    AssertContains('"last_query_sequence":0', Diagnostics, 'post-disconnect query sequence state');
  finally
    Client.Free;
  end;
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

begin
  try
    TestBeginSavepointCommitLifecycleTransitions;
    TestBeginRollbackStartsNextTxnState;
    TestRuntimeFreshBoundaryAdoptsDefaultBeginAndRejectsNonDefault;
    TestBeginTransactionExOptionMatrixEncodesPayload;
    TestBeginTransactionExConflictPathRetainsTxnAvailability;
    TestBeginTransactionExReadCommittedModePayloadAndValidation;
    TestDisconnectClearsAbandonedSessionState;
    Writeln('TxnStateTransitionsTests: OK');
  except
    on E: Exception do
    begin
      Writeln('TxnStateTransitionsTests: FAILED - ' + E.Message);
      Halt(1);
    end;
  end;
end.
