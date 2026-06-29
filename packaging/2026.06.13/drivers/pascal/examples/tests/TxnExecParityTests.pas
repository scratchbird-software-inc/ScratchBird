// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

program TxnExecParityTests;

{$mode delphi}
{$APPTYPE CONSOLE}

uses
  {$IFDEF UNIX}
  cthreads,
  {$ENDIF}
  SysUtils, Variants,
  ScratchBird.Client, ScratchBird.Config, ScratchBird.Protocol, ScratchBird.Sql, ScratchBird.Errors, ScratchBird.Transport;

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

procedure AssertEqualString(const Expected, Actual, MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText + ': expected="' + Expected + '" actual="' + Actual + '"');
end;

procedure AssertEqualInt(Expected, Actual: Integer; const MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText + ': expected=' + IntToStr(Expected) + ' actual=' + IntToStr(Actual));
end;

procedure AssertEqualWord(Expected, Actual: Word; const MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText + ': expected=' + IntToStr(Expected) + ' actual=' + IntToStr(Actual));
end;

procedure AssertEqualUInt32(Expected, Actual: Cardinal; const MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText + ': expected=' + IntToStr(Expected) + ' actual=' + IntToStr(Actual));
end;

procedure AssertContains(const Needle, Haystack, MessageText: string);
begin
  if Pos(Needle, Haystack) = 0 then
    Fail(MessageText + ': expected "' + Needle + '" in "' + Haystack + '"');
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

procedure TestBeginTransactionRequiresConnectedClient;
var
  Client: TScratchBirdClient;
begin
  Client := TScratchBirdClient.Create;
  try
    try
      Client.BeginTransaction;
      Fail('expected begin transaction disconnected guard');
    except
      on E: EScratchbirdConnectionError do
      begin
        AssertEqualString('08003', E.SQLState, 'begin disconnected SQLSTATE');
        AssertContains('Client is not connected', E.Message, 'begin disconnected message');
      end;
    end;
  finally
    Client.Free;
  end;
end;

procedure TestCommitRollbackNoopWithoutActiveTransaction;
var
  Client: TScratchBirdClient;
begin
  Client := TScratchBirdClient.Create;
  try
    Client.Commit;
    Client.Rollback;
  finally
    Client.Free;
  end;
end;

procedure TestSavepointRequiresActiveTransaction;
var
  Client: TScratchBirdClient;
begin
  Client := TScratchBirdClient.Create;
  try
    try
      Client.Savepoint('sp1');
      Fail('expected active transaction guard for savepoint');
    except
      on E: EScratchbirdTransactionError do
      begin
        AssertEqualString('25000', E.SQLState, 'savepoint active txn SQLSTATE');
        AssertContains('active transaction', E.Message, 'savepoint active txn message');
      end;
    end;
  finally
    Client.Free;
  end;
end;

procedure TestSavepointRejectsBlankName;
var
  Client: TScratchBirdClient;
begin
  Client := TScratchBirdClient.Create;
  try
    try
      Client.Savepoint('   ');
      Fail('expected savepoint name validation failure');
    except
      on E: EScratchbirdSyntaxError do
      begin
        AssertEqualString('42601', E.SQLState, 'savepoint name SQLSTATE');
        AssertContains('savepoint name is required', E.Message, 'savepoint name message');
      end;
    end;
  finally
    Client.Free;
  end;
end;

procedure TestExecRejectsBlankSql;
var
  Client: TScratchBirdClient;
begin
  Client := TScratchBirdClient.Create;
  try
    try
      Client.ExecSQL('    ');
      Fail('expected empty SQL guard for ExecSQL');
    except
      on E: EScratchbirdSyntaxError do
      begin
        AssertEqualString('42601', E.SQLState, 'exec empty SQLSTATE');
        AssertContains('SQL text is required', E.Message, 'exec empty SQL message');
      end;
    end;

    try
      Client.ExecuteQuery(#9#10);
      Fail('expected empty SQL guard for ExecuteQuery');
    except
      on E: EScratchbirdSyntaxError do
      begin
        AssertEqualString('42601', E.SQLState, 'query empty SQLSTATE');
        AssertContains('SQL text is required', E.Message, 'query empty SQL message');
      end;
    end;
  finally
    Client.Free;
  end;
end;

procedure TestTxnBeginPayloadEncodesFlagsAndTimeout;
var
  Flags: Word;
  Payload: TBytes;
begin
  Flags := TXN_FLAG_HAS_ISOLATION or TXN_FLAG_HAS_ACCESS or TXN_FLAG_HAS_DEFERRABLE or
    TXN_FLAG_HAS_WAIT or TXN_FLAG_HAS_TIMEOUT or TXN_FLAG_HAS_AUTOCOMMIT;
  Payload := BuildTxnBeginPayload(Flags, 2, 1, ISOLATION_SERIALIZABLE, 1, 1, 0, 250);
  AssertEqualWord(12, Word(Length(Payload)), 'txn begin payload length');
  AssertEqualWord(Flags, ReadUInt16LEAt(Payload, 0), 'txn begin payload flags');
  AssertTrue(Payload[2] = 2, 'txn begin conflict action');
  AssertTrue(Payload[3] = 1, 'txn begin autocommit');
  AssertTrue(Payload[4] = ISOLATION_SERIALIZABLE, 'txn begin isolation');
  AssertTrue(Payload[5] = 1, 'txn begin access mode');
  AssertTrue(Payload[6] = 1, 'txn begin deferrable');
  AssertTrue(Payload[7] = 0, 'txn begin wait mode');
  AssertEqualUInt32(250, ReadUInt32LEAt(Payload, 8), 'txn begin timeout');
end;

procedure TestTxnBeginPayloadExpandsForReadCommittedMode;
var
  Flags: Word;
  Payload: TBytes;
begin
  Flags := TXN_FLAG_HAS_ISOLATION or TXN_FLAG_HAS_TIMEOUT or TXN_FLAG_HAS_READ_COMMITTED_MODE;
  Payload := BuildTxnBeginPayload(Flags, 0, 0, ISOLATION_READ_COMMITTED, 0, 0, 0, 25,
    READ_COMMITTED_MODE_READ_CONSISTENCY);
  AssertEqualWord(16, Word(Length(Payload)), 'txn begin payload length with read committed mode');
  AssertEqualWord(Flags, ReadUInt16LEAt(Payload, 0), 'txn begin payload flags with read committed mode');
  AssertTrue(Payload[4] = ISOLATION_READ_COMMITTED, 'txn begin read committed isolation');
  AssertEqualUInt32(25, ReadUInt32LEAt(Payload, 8), 'txn begin read committed timeout');
  AssertTrue(Payload[12] = READ_COMMITTED_MODE_READ_CONSISTENCY, 'txn begin read committed mode byte');
  AssertEqualString('READ COMMITTED READ CONSISTENCY',
    CanonicalReadCommittedModeName(READ_COMMITTED_MODE_READ_CONSISTENCY),
    'canonical read committed mode helper');
  AssertEqualString('UNKNOWN(99)', CanonicalReadCommittedModeName(99), 'unknown read committed mode helper');
end;

procedure TestPreparedTransactionHelpersEmitCanonicalControlSql;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  MsgType: TScratchBirdMessageType;
  Payload: TBytes;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport, True);
  try
    Transport.QueueInbound(EncodeMessage(MSG_COMMAND_COMPLETE, BuildCommandCompletePayload(0, 0, 0, 'PREPARE TRANSACTION'), 0, 1, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 0, 0), 0, 2, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_COMMAND_COMPLETE, BuildCommandCompletePayload(0, 0, 0, 'COMMIT PREPARED'), 0, 3, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 0, 0), 0, 4, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_COMMAND_COMPLETE, BuildCommandCompletePayload(0, 0, 0, 'ROLLBACK PREPARED'), 0, 5, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 0, 0), 0, 6, nil, 0));

    Client.PrepareTransaction('gid-1');
    Client.CommitPrepared('gid-1');
    Client.RollbackPrepared('gid''2');

    AssertEqualInt(3, Transport.WriteCount, 'prepared helper write count');

    DecodeOutboundFrame(Transport.WriteAt(0), MsgType, Payload);
    AssertTrue(MsgType = MSG_QUERY, 'prepare transaction message type');
    AssertEqualBytes(BuildQueryPayload('PREPARE TRANSACTION ''gid-1''', 0, 0, 0), Payload,
      'prepare transaction payload');

    DecodeOutboundFrame(Transport.WriteAt(1), MsgType, Payload);
    AssertTrue(MsgType = MSG_QUERY, 'commit prepared message type');
    AssertEqualBytes(BuildQueryPayload('COMMIT PREPARED ''gid-1''', 0, 0, 0), Payload,
      'commit prepared payload');

    DecodeOutboundFrame(Transport.WriteAt(2), MsgType, Payload);
    AssertTrue(MsgType = MSG_QUERY, 'rollback prepared message type');
    AssertEqualBytes(BuildQueryPayload('ROLLBACK PREPARED ''gid''''2''', 0, 0, 0), Payload,
      'rollback prepared payload');
  finally
    Client.Free;
  end;
end;

procedure TestPreparedTransactionHelpersRejectBlankGid;
var
  Client: TScratchBirdClient;
begin
  Client := TScratchBirdClient.Create;
  try
    try
      Client.PrepareTransaction('   ');
      Fail('expected blank gid validation failure');
    except
      on E: EScratchbirdSyntaxError do
      begin
        AssertEqualString('42601', E.SQLState, 'blank gid SQLSTATE');
        AssertContains('global transaction id is required', E.Message, 'blank gid message');
      end;
    end;
  finally
    Client.Free;
  end;
end;

procedure TestDormantHelpersFailClosedAndCapabilitiesStayExplicit;
var
  Client: TScratchBirdClient;
begin
  Client := TScratchBirdClient.Create;
  try
    AssertTrue(Client.SupportsPreparedTransactions, 'prepared capability');
    AssertTrue(not Client.SupportsDormantReattach, 'dormant capability remains explicit');

    try
      Client.DetachToDormant;
      Fail('expected dormant detach not-supported failure');
    except
      on E: EScratchbirdNotSupported do
      begin
        AssertEqualString('0A000', E.SQLState, 'dormant detach SQLSTATE');
        AssertContains('Dormant detach', E.Message, 'dormant detach message');
      end;
    end;

    try
      Client.ReattachDormant('dormant-1', 'token');
      Fail('expected dormant reattach not-supported failure');
    except
      on E: EScratchbirdNotSupported do
      begin
        AssertEqualString('0A000', E.SQLState, 'dormant reattach SQLSTATE');
        AssertContains('Dormant reattach', E.Message, 'dormant reattach message');
      end;
    end;
  finally
    Client.Free;
  end;
end;

procedure TestNamedNormalizationPreservesCastMarkers;
var
  Names: TArray<string>;
  Params: TArray<TScratchBirdParamInput>;
  Ordered: TArray<TScratchBirdParamInput>;
  OutSql: string;
begin
  SetLength(Names, 1);
  Names[0] := 'id';
  SetLength(Params, 1);
  Params[0].Value := 7;
  Params[0].Obj := nil;
  OutSql := NormalizeNamedSql('SELECT :id::INTEGER AS v, ''::literal'' AS t', Names, Params, Ordered);
  AssertEqualString('SELECT $1::INTEGER AS v, ''::literal'' AS t', OutSql, 'named cast normalization');
  AssertEqualWord(1, Word(Length(Ordered)), 'named cast ordered count');
  AssertTrue(Ordered[0].Value = 7, 'named cast ordered value');
end;

begin
  try
    TestBeginTransactionRequiresConnectedClient;
    TestCommitRollbackNoopWithoutActiveTransaction;
    TestSavepointRequiresActiveTransaction;
    TestSavepointRejectsBlankName;
    TestExecRejectsBlankSql;
    TestTxnBeginPayloadEncodesFlagsAndTimeout;
    TestTxnBeginPayloadExpandsForReadCommittedMode;
    TestPreparedTransactionHelpersEmitCanonicalControlSql;
    TestPreparedTransactionHelpersRejectBlankGid;
    TestDormantHelpersFailClosedAndCapabilitiesStayExplicit;
    TestNamedNormalizationPreservesCastMarkers;
    Writeln('TxnExecParityTests: OK');
  except
    on E: Exception do
    begin
      Writeln('TxnExecParityTests: FAILED - ' + E.Message);
      Halt(1);
    end;
  end;
end.
