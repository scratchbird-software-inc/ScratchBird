// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

program MetadataExecutionFlowTests;

{$mode delphi}
{$APPTYPE CONSOLE}

uses
  {$IFDEF UNIX}
  cthreads,
  {$ENDIF}
  SysUtils, Variants,
  ScratchBird.Client, ScratchBird.Config, ScratchBird.Metadata, ScratchBird.Protocol, ScratchBird.Transport, ScratchBird.Types;

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

function BuildRowDescriptionPayloadText(const ColumnNames: array of string): TBytes;
var
  I: Integer;
  NameBytes: TBytes;
begin
  Result := nil;
  AppendUInt16LE(Result, Length(ColumnNames));
  AppendUInt16LE(Result, 0);
  for I := 0 to High(ColumnNames) do
  begin
    NameBytes := TEncoding.UTF8.GetBytes(ColumnNames[I]);
    AppendUInt32LE(Result, Cardinal(Length(NameBytes)));
    AppendBytes(Result, NameBytes);
    AppendUInt32LE(Result, 0);
    AppendUInt16LE(Result, I + 1);
    AppendUInt32LE(Result, OID_TEXT);
    AppendUInt16LE(Result, $FFFF);
    AppendUInt32LE(Result, Cardinal($FFFFFFFF));
    AppendBytes(Result, TBytes.Create(FORMAT_TEXT, 1, 0, 0));
  end;
end;

function BuildDataRowPayloadText(const Values: array of string): TBytes;
var
  I: Integer;
  ValueBytes: TBytes;
  NullBytes: Integer;
begin
  Result := nil;
  AppendUInt16LE(Result, Length(Values));
  NullBytes := (Length(Values) + 7) div 8;
  AppendUInt16LE(Result, NullBytes);
  for I := 1 to NullBytes do
    AppendBytes(Result, TBytes.Create(0));
  for I := 0 to High(Values) do
  begin
    ValueBytes := TEncoding.UTF8.GetBytes(Values[I]);
    AppendUInt32LE(Result, Cardinal(Length(ValueBytes)));
    AppendBytes(Result, ValueBytes);
  end;
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

function MetadataField(const Name: string; const Value: Variant): TMetadataField;
begin
  Result.Name := Name;
  Result.Value := Value;
end;

function MetadataRow(const Fields: array of TMetadataField): TMetadataRow;
var
  I: Integer;
begin
  Result := nil;
  SetLength(Result, Length(Fields));
  for I := 0 to High(Fields) do
    Result[I] := Fields[I];
end;

procedure TestMetadataWrappersEmitExpectedCollectionQueries;
const
  CollectionCount = 15;
  Collections: array[0..CollectionCount - 1] of string =
    ('catalogs', 'schemas', 'tables', 'columns', 'indexes', 'index_columns', 'constraints',
     'procedures', 'functions', 'routines', 'primary_keys', 'foreign_keys',
     'table_privileges', 'column_privileges', 'type_info');
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  Stream: TScratchBirdResultStream;
  MsgType: TScratchBirdMessageType;
  Payload: TBytes;
  Row: TArray<Variant>;
  I: Integer;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport, True);
  try
    for I := 0 to CollectionCount - 1 do
    begin
      Transport.QueueInbound(EncodeMessage(MSG_ROW_DESCRIPTION, BuildRowDescriptionPayloadText(['name']), 0, 10 + I * 3, nil, 0));
      Transport.QueueInbound(EncodeMessage(MSG_COMMAND_COMPLETE, BuildCommandCompletePayload(0, 0, 0, 'SELECT 0'), 0, 11 + I * 3, nil, 0));
      Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 0, 0), 0, 12 + I * 3, nil, 0));
    end;

    for I := 0 to CollectionCount - 1 do
    begin
      if Collections[I] = 'catalogs' then
        Stream := Client.GetCatalogs
      else if Collections[I] = 'schemas' then
        Stream := Client.GetSchemas
      else if Collections[I] = 'tables' then
        Stream := Client.GetTables
      else if Collections[I] = 'columns' then
        Stream := Client.GetColumns
      else if Collections[I] = 'indexes' then
        Stream := Client.GetIndexes
      else if Collections[I] = 'index_columns' then
        Stream := Client.GetIndexColumns
      else if Collections[I] = 'constraints' then
        Stream := Client.GetConstraints
      else if Collections[I] = 'procedures' then
        Stream := Client.GetProcedures
      else if Collections[I] = 'functions' then
        Stream := Client.GetFunctions
      else if Collections[I] = 'routines' then
        Stream := Client.GetRoutines
      else if Collections[I] = 'primary_keys' then
        Stream := Client.GetPrimaryKeys
      else if Collections[I] = 'foreign_keys' then
        Stream := Client.GetForeignKeys
      else if Collections[I] = 'table_privileges' then
        Stream := Client.GetTablePrivileges
      else if Collections[I] = 'column_privileges' then
        Stream := Client.GetColumnPrivileges
      else
        Stream := Client.GetTypeInfo;
      try
        Row := Stream.ReadRow;
        AssertEqualInt(0, Length(Row), Collections[I] + ' expected zero-row stream');
      finally
        Stream.Free;
      end;
    end;

    AssertEqualInt(CollectionCount, Transport.WriteCount, 'metadata wrapper write count');
    for I := 0 to CollectionCount - 1 do
    begin
      DecodeOutboundFrame(Transport.WriteAt(I), MsgType, Payload);
      AssertTrue(MsgType = MSG_QUERY, Collections[I] + ' should send query message');
      AssertEqualBytes(
        BuildQueryPayload(ResolveMetadataCollectionQuery(Collections[I]), 0, 0, 0),
        Payload,
        Collections[I] + ' query payload');
    end;
  finally
    Client.Free;
  end;
end;

procedure TestQueryMetadataRowsAppliesRestrictionsFromWireRows;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  Restrictions: TMetadataRow;
  Rows: TMetadataRows;
  SchemaValue, TableValue: Variant;
  MsgType: TScratchBirdMessageType;
  Payload: TBytes;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport, True);
  try
    Transport.QueueInbound(EncodeMessage(
      MSG_ROW_DESCRIPTION,
      BuildRowDescriptionPayloadText(['table_schema', 'table_name']),
      0, 50, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_DATA_ROW, BuildDataRowPayloadText(['users', 'accounts']), 0, 51, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_DATA_ROW, BuildDataRowPayloadText(['sys', 'catalog_tables']), 0, 52, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_COMMAND_COMPLETE, BuildCommandCompletePayload(0, 2, 0, 'SELECT 2'), 0, 53, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 0, 0), 0, 54, nil, 0));

    Restrictions := MetadataRow([MetadataField('schema', 'users')]);
    Rows := Client.QueryMetadataRows('tables', Restrictions);

    AssertEqualInt(1, Length(Rows), 'filtered metadata row count');
    AssertTrue(MetadataRowTryGetValue(Rows[0], 'table_schema', SchemaValue), 'table_schema value should exist');
    AssertTrue(MetadataRowTryGetValue(Rows[0], 'table_name', TableValue), 'table_name value should exist');
    AssertEqualString('users', VarToStr(SchemaValue), 'filtered table_schema');
    AssertEqualString('accounts', VarToStr(TableValue), 'filtered table_name');

    AssertEqualInt(1, Transport.WriteCount, 'metadata rows write count');
    DecodeOutboundFrame(Transport.WriteAt(0), MsgType, Payload);
    AssertTrue(MsgType = MSG_QUERY, 'metadata rows should send query message');
    AssertEqualBytes(
      BuildQueryPayload(ResolveMetadataCollectionQuery('tables'), 0, 0, 0),
      Payload,
      'tables query payload');
  finally
    Client.Free;
  end;
end;

procedure TestQueryMetadataRowsAppliesRoutineRestrictionsFromWireRows;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  Restrictions: TMetadataRow;
  Rows: TMetadataRows;
  RoutineValue: Variant;
  MsgType: TScratchBirdMessageType;
  Payload: TBytes;
begin
  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport, True);
  try
    Transport.QueueInbound(EncodeMessage(
      MSG_ROW_DESCRIPTION,
      BuildRowDescriptionPayloadText(['routine_name', 'routine_type']),
      0, 80, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_DATA_ROW, BuildDataRowPayloadText(['refresh_cache', 'PROCEDURE']), 0, 81, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_DATA_ROW, BuildDataRowPayloadText(['to_json_text', 'FUNCTION']), 0, 82, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_COMMAND_COMPLETE, BuildCommandCompletePayload(0, 2, 0, 'SELECT 2'), 0, 83, nil, 0));
    Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 0, 0), 0, 84, nil, 0));

    Restrictions := MetadataRow([MetadataField('procedure', 'refresh_cache')]);
    Rows := Client.QueryMetadataRows('routines', Restrictions);

    AssertEqualInt(1, Length(Rows), 'filtered routines row count');
    AssertTrue(MetadataRowTryGetValue(Rows[0], 'routine_name', RoutineValue), 'routine_name value should exist');
    AssertEqualString('refresh_cache', VarToStr(RoutineValue), 'filtered routine_name');

    AssertEqualInt(1, Transport.WriteCount, 'routines metadata rows write count');
    DecodeOutboundFrame(Transport.WriteAt(0), MsgType, Payload);
    AssertTrue(MsgType = MSG_QUERY, 'routines metadata rows should send query message');
    AssertEqualBytes(
      BuildQueryPayload(ResolveMetadataCollectionQuery('routines'), 0, 0, 0),
      Payload,
      'routines query payload');
  finally
    Client.Free;
  end;
end;

procedure TestQueryMetadataRowsAppliesRestrictionsAcrossAdditionalFamilies;
type
  TRestrictionScenario = record
    CollectionName: string;
    RestrictionName: string;
    RestrictionValue: string;
    ValueColumn: string;
    AuxColumn: string;
    MatchAuxValue: string;
    OtherValue: string;
    OtherAuxValue: string;
  end;
var
  Transport: TFakeTransport;
  Client: TScratchBirdClient;
  Scenarios: array[0..11] of TRestrictionScenario;
  Restrictions: TMetadataRow;
  Rows: TMetadataRows;
  FilterValue: Variant;
  MsgType: TScratchBirdMessageType;
  Payload: TBytes;
  BaseSeq: Cardinal;
  I: Integer;
begin
  Scenarios[0].CollectionName := 'catalogs';
  Scenarios[0].RestrictionName := 'catalog';
  Scenarios[0].RestrictionValue := 'users';
  Scenarios[0].ValueColumn := 'catalog_name';
  Scenarios[0].AuxColumn := 'catalog_id';
  Scenarios[0].MatchAuxValue := '11';
  Scenarios[0].OtherValue := 'sys';
  Scenarios[0].OtherAuxValue := '12';

  Scenarios[1].CollectionName := 'columns';
  Scenarios[1].RestrictionName := 'column';
  Scenarios[1].RestrictionValue := 'email';
  Scenarios[1].ValueColumn := 'column_name';
  Scenarios[1].AuxColumn := 'table_name';
  Scenarios[1].MatchAuxValue := 'accounts';
  Scenarios[1].OtherValue := 'id';
  Scenarios[1].OtherAuxValue := 'accounts';

  Scenarios[2].CollectionName := 'indexes';
  Scenarios[2].RestrictionName := 'index';
  Scenarios[2].RestrictionValue := 'idx_accounts_email';
  Scenarios[2].ValueColumn := 'index_name';
  Scenarios[2].AuxColumn := 'table_name';
  Scenarios[2].MatchAuxValue := 'accounts';
  Scenarios[2].OtherValue := 'idx_accounts_id';
  Scenarios[2].OtherAuxValue := 'accounts';

  Scenarios[3].CollectionName := 'constraints';
  Scenarios[3].RestrictionName := 'constraint';
  Scenarios[3].RestrictionValue := 'pk_accounts';
  Scenarios[3].ValueColumn := 'constraint_name';
  Scenarios[3].AuxColumn := 'constraint_type';
  Scenarios[3].MatchAuxValue := 'PRIMARY KEY';
  Scenarios[3].OtherValue := 'fk_accounts_user';
  Scenarios[3].OtherAuxValue := 'FOREIGN KEY';

  Scenarios[4].CollectionName := 'primary_keys';
  Scenarios[4].RestrictionName := 'constraint';
  Scenarios[4].RestrictionValue := 'pk_users';
  Scenarios[4].ValueColumn := 'constraint_name';
  Scenarios[4].AuxColumn := 'table_name';
  Scenarios[4].MatchAuxValue := 'users';
  Scenarios[4].OtherValue := 'pk_roles';
  Scenarios[4].OtherAuxValue := 'roles';

  Scenarios[5].CollectionName := 'foreign_keys';
  Scenarios[5].RestrictionName := 'constraint';
  Scenarios[5].RestrictionValue := 'fk_users_role';
  Scenarios[5].ValueColumn := 'constraint_name';
  Scenarios[5].AuxColumn := 'table_name';
  Scenarios[5].MatchAuxValue := 'users';
  Scenarios[5].OtherValue := 'fk_accounts_user';
  Scenarios[5].OtherAuxValue := 'accounts';

  Scenarios[6].CollectionName := 'table_privileges';
  Scenarios[6].RestrictionName := 'table';
  Scenarios[6].RestrictionValue := 'accounts';
  Scenarios[6].ValueColumn := 'table_name';
  Scenarios[6].AuxColumn := 'privilege_type';
  Scenarios[6].MatchAuxValue := 'ALL';
  Scenarios[6].OtherValue := 'users';
  Scenarios[6].OtherAuxValue := 'ALL';

  Scenarios[7].CollectionName := 'column_privileges';
  Scenarios[7].RestrictionName := 'column';
  Scenarios[7].RestrictionValue := 'email';
  Scenarios[7].ValueColumn := 'column_name';
  Scenarios[7].AuxColumn := 'privilege_type';
  Scenarios[7].MatchAuxValue := 'ALL';
  Scenarios[7].OtherValue := 'id';
  Scenarios[7].OtherAuxValue := 'ALL';

  Scenarios[8].CollectionName := 'procedures';
  Scenarios[8].RestrictionName := 'procedure';
  Scenarios[8].RestrictionValue := 'refresh_cache';
  Scenarios[8].ValueColumn := 'procedure_name';
  Scenarios[8].AuxColumn := 'routine_type';
  Scenarios[8].MatchAuxValue := 'PROCEDURE';
  Scenarios[8].OtherValue := 'vacuum_stats';
  Scenarios[8].OtherAuxValue := 'PROCEDURE';

  Scenarios[9].CollectionName := 'functions';
  Scenarios[9].RestrictionName := 'function';
  Scenarios[9].RestrictionValue := 'to_json_text';
  Scenarios[9].ValueColumn := 'function_name';
  Scenarios[9].AuxColumn := 'return_type';
  Scenarios[9].MatchAuxValue := 'TEXT';
  Scenarios[9].OtherValue := 'to_uuid_text';
  Scenarios[9].OtherAuxValue := 'TEXT';

  Scenarios[10].CollectionName := 'type_info';
  Scenarios[10].RestrictionName := 'type';
  Scenarios[10].RestrictionValue := 'INTEGER';
  Scenarios[10].ValueColumn := 'type_name';
  Scenarios[10].AuxColumn := 'type_oid';
  Scenarios[10].MatchAuxValue := '23';
  Scenarios[10].OtherValue := 'TEXT';
  Scenarios[10].OtherAuxValue := '25';

  Scenarios[11].CollectionName := 'index_columns';
  Scenarios[11].RestrictionName := 'index';
  Scenarios[11].RestrictionValue := 'idx_accounts_email';
  Scenarios[11].ValueColumn := 'index_name';
  Scenarios[11].AuxColumn := 'column_name';
  Scenarios[11].MatchAuxValue := 'email';
  Scenarios[11].OtherValue := 'idx_accounts_id';
  Scenarios[11].OtherAuxValue := 'id';

  Transport := TFakeTransport.Create;
  Client := TScratchBirdClient.CreateWithTransport(Transport, True);
  try
    for I := Low(Scenarios) to High(Scenarios) do
    begin
      BaseSeq := 200 + Cardinal(I * 5);
      Transport.QueueInbound(EncodeMessage(
        MSG_ROW_DESCRIPTION,
        BuildRowDescriptionPayloadText([Scenarios[I].ValueColumn, Scenarios[I].AuxColumn]),
        0, BaseSeq, nil, 0));
      Transport.QueueInbound(EncodeMessage(MSG_DATA_ROW,
        BuildDataRowPayloadText([Scenarios[I].RestrictionValue, Scenarios[I].MatchAuxValue]),
        0, BaseSeq + 1, nil, 0));
      Transport.QueueInbound(EncodeMessage(MSG_DATA_ROW,
        BuildDataRowPayloadText([Scenarios[I].OtherValue, Scenarios[I].OtherAuxValue]),
        0, BaseSeq + 2, nil, 0));
      Transport.QueueInbound(EncodeMessage(MSG_COMMAND_COMPLETE, BuildCommandCompletePayload(0, 2, 0, 'SELECT 2'), 0, BaseSeq + 3, nil, 0));
      Transport.QueueInbound(EncodeMessage(MSG_READY, BuildReadyPayload(0, 0, 0), 0, BaseSeq + 4, nil, 0));

      Restrictions := MetadataRow([MetadataField(Scenarios[I].RestrictionName, Scenarios[I].RestrictionValue)]);
      Rows := Client.QueryMetadataRows(Scenarios[I].CollectionName, Restrictions);

      AssertEqualInt(1, Length(Rows), Scenarios[I].CollectionName + ' filtered row count');
      AssertTrue(MetadataRowTryGetValue(Rows[0], Scenarios[I].ValueColumn, FilterValue),
        Scenarios[I].CollectionName + ' value column should exist');
      AssertEqualString(Scenarios[I].RestrictionValue, VarToStr(FilterValue),
        Scenarios[I].CollectionName + ' restriction value');

      AssertEqualInt(I + 1, Transport.WriteCount, Scenarios[I].CollectionName + ' metadata rows write count');
      DecodeOutboundFrame(Transport.WriteAt(I), MsgType, Payload);
      AssertTrue(MsgType = MSG_QUERY, Scenarios[I].CollectionName + ' metadata rows should send query message');
      AssertEqualBytes(
        BuildQueryPayload(ResolveMetadataCollectionQuery(Scenarios[I].CollectionName), 0, 0, 0),
        Payload,
        Scenarios[I].CollectionName + ' query payload');
    end;
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
    TestMetadataWrappersEmitExpectedCollectionQueries;
    TestQueryMetadataRowsAppliesRestrictionsFromWireRows;
    TestQueryMetadataRowsAppliesRoutineRestrictionsFromWireRows;
    TestQueryMetadataRowsAppliesRestrictionsAcrossAdditionalFamilies;
    Writeln('MetadataExecutionFlowTests: OK');
  except
    on E: Exception do
    begin
      Writeln('MetadataExecutionFlowTests: FAILED - ' + E.Message);
      Halt(1);
    end;
  end;
end.
