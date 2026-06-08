// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

unit ScratchBird.Protocol;

{$mode delphi}
{$H+}

interface

uses
  SysUtils, Classes;

const
  PROTOCOL_MAGIC = $50574253;
  PROTOCOL_VERSION_MAJOR = 1;
  PROTOCOL_VERSION_MINOR = 1;
  HEADER_SIZE = 40;
  MAX_MESSAGE_SIZE = 1024 * 1024 * 1024;

type
  TScratchBirdMessageType = Byte;

const
  MSG_STARTUP = $01;
  MSG_AUTH_RESPONSE = $02;
  MSG_QUERY = $03;
  MSG_PARSE = $04;
  MSG_BIND = $05;
  MSG_DESCRIBE = $06;
  MSG_EXECUTE = $07;
  MSG_CLOSE = $08;
  MSG_SYNC = $09;
  MSG_FLUSH = $0A;
  MSG_CANCEL = $0B;
  MSG_TERMINATE = $0C;
  MSG_COPY_DATA = $0D;
  MSG_COPY_DONE = $0E;
  MSG_COPY_FAIL = $0F;
  MSG_SBLR_EXECUTE = $10;
  MSG_SUBSCRIBE = $11;
  MSG_UNSUBSCRIBE = $12;
  MSG_FEDERATED_QUERY = $13;
  MSG_STREAM_CONTROL = $14;
  MSG_TXN_BEGIN = $15;
  MSG_TXN_COMMIT = $16;
  MSG_TXN_ROLLBACK = $17;
  MSG_TXN_SAVEPOINT = $18;
  MSG_TXN_RELEASE = $19;
  MSG_TXN_ROLLBACK_TO = $1A;
  MSG_PING = $1B;
  MSG_SET_OPTION = $1C;
  MSG_CLUSTER_AUTH = $1D;
  MSG_ATTACH_CREATE = $1E;
  MSG_ATTACH_DETACH = $1F;
  MSG_ATTACH_LIST = $20;

  MSG_AUTH_REQUEST = $40;
  MSG_AUTH_OK = $41;
  MSG_AUTH_CONTINUE = $42;
  MSG_READY = $43;
  MSG_ROW_DESCRIPTION = $44;
  MSG_DATA_ROW = $45;
  MSG_COMMAND_COMPLETE = $46;
  MSG_EMPTY_QUERY = $47;
  MSG_ERROR = $48;
  MSG_NOTICE = $49;
  MSG_PARSE_COMPLETE = $4A;
  MSG_BIND_COMPLETE = $4B;
  MSG_CLOSE_COMPLETE = $4C;
  MSG_PORTAL_SUSPENDED = $4D;
  MSG_NO_DATA = $4E;
  MSG_PARAMETER_STATUS = $4F;
  MSG_PARAMETER_DESCRIPTION = $50;
  MSG_COPY_IN_RESPONSE = $51;
  MSG_COPY_OUT_RESPONSE = $52;
  MSG_COPY_BOTH_RESPONSE = $53;
  MSG_NOTIFICATION = $54;
  MSG_FUNCTION_RESULT = $55;
  MSG_NEGOTIATE_VERSION = $56;
  MSG_SBLR_COMPILED = $57;
  MSG_QUERY_PLAN = $58;
  MSG_STREAM_READY = $59;
  MSG_STREAM_DATA = $5A;
  MSG_STREAM_END = $5B;
  MSG_TXN_STATUS = $5C;
  MSG_PONG = $5D;
  MSG_CLUSTER_AUTH_OK = $5E;
  MSG_FEDERATED_RESULT = $5F;
  MSG_HEARTBEAT = $80;
  MSG_EXTENSION = $81;

type
  TAuthMethod = Byte;

const
  AUTH_OK = 0;
  AUTH_PASSWORD = 1;
  AUTH_MD5 = 2;
  AUTH_SCRAM_SHA256 = 3;
  AUTH_SCRAM_SHA512 = 4;
  AUTH_TOKEN = 5;
  AUTH_PEER = 6;
  AUTH_REATTACH = 7;

const
  MSG_FLAG_COMPRESSED = $01;
  MSG_FLAG_CONTINUED = $02;
  MSG_FLAG_FINAL = $04;
  MSG_FLAG_URGENT = $08;
  MSG_FLAG_ENCRYPTED = $10;
  MSG_FLAG_CHECKSUM = $20;

const
  FEATURE_COMPRESSION = 1;
  FEATURE_STREAMING = 2;
  FEATURE_SBLR = 4;
  FEATURE_FEDERATION = 8;
  FEATURE_NOTIFICATIONS = 16;
  FEATURE_QUERY_PLAN = 32;
  FEATURE_BATCH = 64;
  FEATURE_PIPELINE = 128;
  FEATURE_BINARY_COPY = 256;
  FEATURE_SAVEPOINTS = 512;
  FEATURE_2PC = 1024;
  FEATURE_CHECKSUMS = 2048;

  QUERY_FLAG_DESCRIBE_ONLY = $01;
  QUERY_FLAG_NO_PORTAL = $02;
  QUERY_FLAG_BINARY_RESULT = $04;
  QUERY_FLAG_INCLUDE_PLAN = $08;
  QUERY_FLAG_RETURN_SBLR = $10;
  QUERY_FLAG_NO_CACHE = $20;

  ISOLATION_READ_UNCOMMITTED = 0;
  ISOLATION_READ_COMMITTED = 1;
  ISOLATION_REPEATABLE_READ = 2;
  ISOLATION_SERIALIZABLE = 3;

  READ_COMMITTED_MODE_DEFAULT = 0;
  READ_COMMITTED_MODE_READ_CONSISTENCY = 1;
  READ_COMMITTED_MODE_RECORD_VERSION = 2;
  READ_COMMITTED_MODE_NO_RECORD_VERSION = 3;

  TXN_FLAG_HAS_ISOLATION = $0001;
  TXN_FLAG_HAS_ACCESS = $0002;
  TXN_FLAG_HAS_DEFERRABLE = $0004;
  TXN_FLAG_HAS_WAIT = $0008;
  TXN_FLAG_HAS_TIMEOUT = $0010;
  TXN_FLAG_HAS_AUTOCOMMIT = $0020;
  TXN_FLAG_HAS_READ_COMMITTED_MODE = $0100;

  STREAM_START = 0;
  STREAM_PAUSE = 1;
  STREAM_RESUME = 2;
  STREAM_CANCEL = 3;
  STREAM_ACK = 4;

  SUB_TYPE_CHANNEL = 0;
  SUB_TYPE_TABLE = 1;
  SUB_TYPE_QUERY = 2;
  SUB_TYPE_EVENT = 3;

type
  TParamValue = record
    Format: Word;
    Data: TBytes;
    IsNull: Boolean;
  end;

  TColumnInfo = record
    Name: string;
    TableOid: Cardinal;
    ColumnIndex: Word;
    TypeOid: Cardinal;
    TypeSize: SmallInt;
    TypeModifier: Integer;
    Format: Word;
    Nullable: Boolean;
  end;

  TColumnValue = record
    Data: TBytes;
    IsNull: Boolean;
  end;

  TScratchBirdMessage = record
    MsgType: TScratchBirdMessageType;
    Flags: Byte;
    Payload: TBytes;
    Sequence: Cardinal;
    AttachmentId: TBytes;
    TxnId: UInt64;
  end;

function EncodeMessage(MsgType: TScratchBirdMessageType; const Payload: TBytes; Flags: Byte;
  Sequence: Cardinal; const AttachmentId: TBytes; TxnId: UInt64): TBytes;
function DecodeHeader(const Header: TBytes; out MsgType: TScratchBirdMessageType; out Flags: Byte;
  out Length: Integer; out Sequence: Cardinal; out AttachmentId: TBytes; out TxnId: UInt64): Boolean;

function BuildStartupPayload(Features: UInt64; const Params: TStringList): TBytes;
function BuildParamList(const Params: TStringList): TBytes;
function BuildQueryPayload(const Sql: string; Flags, MaxRows, TimeoutMs: Cardinal): TBytes;
function BuildParsePayload(const StatementName, Sql: string; const ParamTypes: array of Cardinal): TBytes;
function BuildBindPayload(const PortalName, StatementName: string; const Params: array of TParamValue;
  const ResultFormats: array of Word): TBytes;
function BuildExecutePayload(const PortalName: string; MaxRows: Cardinal): TBytes;
function BuildDescribePayload(DescribeType: Byte; const Name: string): TBytes;
function BuildCancelPayload(CancelType, TargetSequence: Cardinal): TBytes;
function BuildSblrExecutePayload(SblrHash: UInt64; const SblrBytecode: TBytes; const Params: array of TParamValue): TBytes;
function BuildSubscribePayload(SubscribeType: Byte; const Channel, FilterExpr: string): TBytes;
function BuildUnsubscribePayload(const Channel: string): TBytes;
function CanonicalReadCommittedModeName(ReadCommittedMode: Byte): string;
function BuildTxnBeginPayload(Flags: Word; ConflictAction, AutocommitMode, IsolationLevel, AccessMode, Deferrable, WaitMode: Byte;
  TimeoutMs: Cardinal; ReadCommittedMode: Byte = READ_COMMITTED_MODE_DEFAULT): TBytes;
function BuildTxnCommitPayload(Flags: Byte): TBytes;
function BuildTxnRollbackPayload(Flags: Byte): TBytes;
function BuildTxnSavepointPayload(const Name: string): TBytes;
function BuildTxnReleasePayload(const Name: string): TBytes;
function BuildTxnRollbackToPayload(const Name: string): TBytes;
function BuildSetOptionPayload(const Name, Value: string): TBytes;
function BuildStreamControlPayload(ControlType: Byte; WindowSize, TimeoutMs: Cardinal): TBytes;
function BuildAttachCreatePayload(const EmulationMode, DbName: string): TBytes;

procedure ParseAuthRequest(const Payload: TBytes; out Method: Byte; out Data: TBytes);
procedure ParseAuthContinue(const Payload: TBytes; out Method, Stage: Byte; out Data: TBytes);
procedure ParseAuthOk(const Payload: TBytes; out SessionId: TBytes; out ServerInfo: TBytes);
procedure ParseReady(const Payload: TBytes; out Status: Byte; out TxnId, Visibility: UInt64);
procedure ParseTxnStatus(const Payload: TBytes; out Status: Byte; out TxnId: UInt64);
procedure ParseParameterStatus(const Payload: TBytes; out Name, Value: string);
function ParseParameterDescription(const Payload: TBytes): TArray<Cardinal>;
function ParseRowDescription(const Payload: TBytes): TArray<TColumnInfo>;
function ParseRowData(const Payload: TBytes): TArray<TColumnValue>;
procedure ParseCommandComplete(const Payload: TBytes; out CommandType: Byte; out Rows, LastId: UInt64; out Tag: string);
procedure ParseErrorMessage(const Payload: TBytes; out Severity, SqlState, Message, Detail, Hint: string);

implementation

function ConcatBytes(const Left, Right: TBytes): TBytes;
begin
  SetLength(Result, Length(Left) + Length(Right));
  if Length(Left) > 0 then
    Move(Left[0], Result[0], Length(Left));
  if Length(Right) > 0 then
    Move(Right[0], Result[Length(Left)], Length(Right));
end;

function BytesOf(const Values: array of Byte): TBytes;
var
  I: Integer;
begin
  SetLength(Result, Length(Values));
  for I := 0 to High(Values) do
    Result[I] := Values[I];
end;

function WriteUInt16LE(Value: Word): TBytes;
begin
  SetLength(Result, 2);
  Result[0] := Byte(Value and $FF);
  Result[1] := Byte((Value shr 8) and $FF);
end;

function WriteInt16LE(Value: SmallInt): TBytes;
begin
  SetLength(Result, 2);
  Result[0] := Byte(Value and $FF);
  Result[1] := Byte((Value shr 8) and $FF);
end;

function WriteUInt32LE(Value: Cardinal): TBytes;
begin
  SetLength(Result, 4);
  Result[0] := Byte(Value and $FF);
  Result[1] := Byte((Value shr 8) and $FF);
  Result[2] := Byte((Value shr 16) and $FF);
  Result[3] := Byte((Value shr 24) and $FF);
end;

function WriteInt32LE(Value: Integer): TBytes;
begin
  SetLength(Result, 4);
  Result[0] := Byte(Value and $FF);
  Result[1] := Byte((Value shr 8) and $FF);
  Result[2] := Byte((Value shr 16) and $FF);
  Result[3] := Byte((Value shr 24) and $FF);
end;

function WriteUInt64LE(Value: UInt64): TBytes;
begin
  SetLength(Result, 8);
  Result[0] := Byte(Value and $FF);
  Result[1] := Byte((Value shr 8) and $FF);
  Result[2] := Byte((Value shr 16) and $FF);
  Result[3] := Byte((Value shr 24) and $FF);
  Result[4] := Byte((Value shr 32) and $FF);
  Result[5] := Byte((Value shr 40) and $FF);
  Result[6] := Byte((Value shr 48) and $FF);
  Result[7] := Byte((Value shr 56) and $FF);
end;

function ReadUInt16LE(const Data: TBytes; Offset: Integer): Word;
begin
  Result := Word(Data[Offset]) or (Word(Data[Offset + 1]) shl 8);
end;

function ReadInt16LE(const Data: TBytes; Offset: Integer): SmallInt;
begin
  Result := SmallInt(Word(Data[Offset]) or (Word(Data[Offset + 1]) shl 8));
end;

function ReadUInt32LE(const Data: TBytes; Offset: Integer): Cardinal;
begin
  Result := Cardinal(Data[Offset]) or (Cardinal(Data[Offset + 1]) shl 8) or
    (Cardinal(Data[Offset + 2]) shl 16) or (Cardinal(Data[Offset + 3]) shl 24);
end;

function ReadInt32LE(const Data: TBytes; Offset: Integer): Integer;
begin
  Result := Integer(ReadUInt32LE(Data, Offset));
end;

function ReadUInt64LE(const Data: TBytes; Offset: Integer): UInt64;
begin
  Result := UInt64(Data[Offset]) or (UInt64(Data[Offset + 1]) shl 8) or
    (UInt64(Data[Offset + 2]) shl 16) or (UInt64(Data[Offset + 3]) shl 24) or
    (UInt64(Data[Offset + 4]) shl 32) or (UInt64(Data[Offset + 5]) shl 40) or
    (UInt64(Data[Offset + 6]) shl 48) or (UInt64(Data[Offset + 7]) shl 56);
end;

function PadBytes(const Data: TBytes; Length: Integer): TBytes;
begin
  Result := Data;
  if System.Length(Result) > Length then
  begin
    SetLength(Result, Length);
    Exit;
  end;
  if System.Length(Result) < Length then
  begin
    SetLength(Result, Length);
    FillChar(Result[System.Length(Data)], Length - System.Length(Data), 0);
  end;
end;

function EncodeMessage(MsgType: TScratchBirdMessageType; const Payload: TBytes; Flags: Byte;
  Sequence: Cardinal; const AttachmentId: TBytes; TxnId: UInt64): TBytes;
var
  Header: TBytes;
begin
  SetLength(Header, 0);
  Header := ConcatBytes(Header, WriteUInt32LE(PROTOCOL_MAGIC));
  Header := ConcatBytes(Header, BytesOf([PROTOCOL_VERSION_MAJOR, PROTOCOL_VERSION_MINOR, MsgType, Flags]));
  Header := ConcatBytes(Header, WriteUInt32LE(System.Length(Payload)));
  Header := ConcatBytes(Header, WriteUInt32LE(Sequence));
  Header := ConcatBytes(Header, PadBytes(AttachmentId, 16));
  Header := ConcatBytes(Header, WriteUInt64LE(TxnId));
  Result := ConcatBytes(Header, Payload);
end;

function DecodeHeader(const Header: TBytes; out MsgType: TScratchBirdMessageType; out Flags: Byte;
  out Length: Integer; out Sequence: Cardinal; out AttachmentId: TBytes; out TxnId: UInt64): Boolean;
var
  Magic: Cardinal;
  Major, Minor: Byte;
begin
  Result := False;
  if System.Length(Header) <> HEADER_SIZE then
    Exit;
  Magic := ReadUInt32LE(Header, 0);
  if Magic <> PROTOCOL_MAGIC then
    Exit;
  Major := Header[4];
  Minor := Header[5];
  if (Major <> PROTOCOL_VERSION_MAJOR) or (Minor <> PROTOCOL_VERSION_MINOR) then
    Exit;
  MsgType := Header[6];
  Flags := Header[7];
  Length := Integer(ReadUInt32LE(Header, 8));
  if Length > MAX_MESSAGE_SIZE then
    Exit;
  Sequence := ReadUInt32LE(Header, 12);
  SetLength(AttachmentId, 16);
  Move(Header[16], AttachmentId[0], 16);
  TxnId := ReadUInt64LE(Header, 32);
  Result := True;
end;

function BuildParamList(const Params: TStringList): TBytes;
var
  I: Integer;
  Key, Value: string;
  Buffer: TBytes;
begin
  Buffer := nil;
  for I := 0 to Params.Count - 1 do
  begin
    Key := Params.Names[I];
    Value := Params.ValueFromIndex[I];
    Buffer := ConcatBytes(Buffer, TEncoding.UTF8.GetBytes(Key));
    Buffer := ConcatBytes(Buffer, BytesOf([0]));
    Buffer := ConcatBytes(Buffer, TEncoding.UTF8.GetBytes(Value));
    Buffer := ConcatBytes(Buffer, BytesOf([0]));
  end;
  Buffer := ConcatBytes(Buffer, BytesOf([0]));
  Result := Buffer;
end;

function BuildStartupPayload(Features: UInt64; const Params: TStringList): TBytes;
var
  Header: TBytes;
begin
  Header := BytesOf([PROTOCOL_VERSION_MAJOR, PROTOCOL_VERSION_MINOR, 0, 0]);
  Header := ConcatBytes(Header, WriteUInt64LE(Features));
  Result := ConcatBytes(Header, BuildParamList(Params));
end;

function BuildQueryPayload(const Sql: string; Flags, MaxRows, TimeoutMs: Cardinal): TBytes;
begin
  Result := ConcatBytes(WriteUInt32LE(Flags), WriteUInt32LE(MaxRows));
  Result := ConcatBytes(Result, WriteUInt32LE(TimeoutMs));
  Result := ConcatBytes(Result, TEncoding.UTF8.GetBytes(Sql));
  Result := ConcatBytes(Result, BytesOf([0]));
end;

function BuildParsePayload(const StatementName, Sql: string; const ParamTypes: array of Cardinal): TBytes;
var
  NameBytes, SqlBytes, Payload: TBytes;
  I: Integer;
begin
  NameBytes := TEncoding.UTF8.GetBytes(StatementName);
  SqlBytes := TEncoding.UTF8.GetBytes(Sql);
  Payload := ConcatBytes(WriteUInt32LE(System.Length(NameBytes)), NameBytes);
  Payload := ConcatBytes(Payload, WriteUInt32LE(System.Length(SqlBytes)));
  Payload := ConcatBytes(Payload, SqlBytes);
  Payload := ConcatBytes(Payload, WriteUInt16LE(Length(ParamTypes)));
  Payload := ConcatBytes(Payload, WriteUInt16LE(0));
  for I := 0 to High(ParamTypes) do
    Payload := ConcatBytes(Payload, WriteUInt32LE(ParamTypes[I]));
  Result := Payload;
end;

function BuildBindPayload(const PortalName, StatementName: string; const Params: array of TParamValue;
  const ResultFormats: array of Word): TBytes;
var
  PortalBytes, StmtBytes, Payload: TBytes;
  I: Integer;
begin
  PortalBytes := TEncoding.UTF8.GetBytes(PortalName);
  StmtBytes := TEncoding.UTF8.GetBytes(StatementName);
  Payload := ConcatBytes(WriteUInt32LE(System.Length(PortalBytes)), PortalBytes);
  Payload := ConcatBytes(Payload, WriteUInt32LE(System.Length(StmtBytes)));
  Payload := ConcatBytes(Payload, StmtBytes);
  Payload := ConcatBytes(Payload, WriteUInt16LE(Length(Params)));
  for I := 0 to High(Params) do
    Payload := ConcatBytes(Payload, WriteUInt16LE(Params[I].Format));
  Payload := ConcatBytes(Payload, WriteUInt16LE(Length(Params)));
  Payload := ConcatBytes(Payload, WriteUInt16LE(0));
  for I := 0 to High(Params) do
  begin
    if Params[I].IsNull then
      Payload := ConcatBytes(Payload, WriteInt32LE(-1))
    else
    begin
      Payload := ConcatBytes(Payload, WriteInt32LE(System.Length(Params[I].Data)));
      Payload := ConcatBytes(Payload, Params[I].Data);
    end;
  end;
  Payload := ConcatBytes(Payload, WriteUInt16LE(Length(ResultFormats)));
  for I := 0 to High(ResultFormats) do
    Payload := ConcatBytes(Payload, WriteUInt16LE(ResultFormats[I]));
  Result := Payload;
end;

function BuildExecutePayload(const PortalName: string; MaxRows: Cardinal): TBytes;
var
  PortalBytes: TBytes;
begin
  PortalBytes := TEncoding.UTF8.GetBytes(PortalName);
  Result := ConcatBytes(WriteUInt32LE(System.Length(PortalBytes)), PortalBytes);
  Result := ConcatBytes(Result, WriteUInt32LE(MaxRows));
end;

function BuildDescribePayload(DescribeType: Byte; const Name: string): TBytes;
var
  NameBytes: TBytes;
begin
  NameBytes := TEncoding.UTF8.GetBytes(Name);
  Result := BytesOf([DescribeType, 0, 0, 0]);
  Result := ConcatBytes(Result, WriteUInt32LE(System.Length(NameBytes)));
  Result := ConcatBytes(Result, NameBytes);
end;

function BuildCancelPayload(CancelType, TargetSequence: Cardinal): TBytes;
begin
  Result := ConcatBytes(WriteUInt32LE(CancelType), WriteUInt32LE(TargetSequence));
end;

function BuildSblrExecutePayload(SblrHash: UInt64; const SblrBytecode: TBytes; const Params: array of TParamValue): TBytes;
var
  Payload: TBytes;
  I: Integer;
begin
  Payload := WriteUInt64LE(SblrHash);
  Payload := ConcatBytes(Payload, WriteUInt32LE(System.Length(SblrBytecode)));
  Payload := ConcatBytes(Payload, WriteUInt16LE(Length(Params)));
  Payload := ConcatBytes(Payload, WriteUInt16LE(0));
  if System.Length(SblrBytecode) > 0 then
    Payload := ConcatBytes(Payload, SblrBytecode);
  for I := 0 to High(Params) do
  begin
    if Params[I].IsNull then
      Payload := ConcatBytes(Payload, WriteInt32LE(-1))
    else
    begin
      Payload := ConcatBytes(Payload, WriteInt32LE(System.Length(Params[I].Data)));
      Payload := ConcatBytes(Payload, Params[I].Data);
    end;
  end;
  Result := Payload;
end;

function BuildSubscribePayload(SubscribeType: Byte; const Channel, FilterExpr: string): TBytes;
var
  ChannelBytes, FilterBytes: TBytes;
begin
  ChannelBytes := TEncoding.UTF8.GetBytes(Channel);
  FilterBytes := TEncoding.UTF8.GetBytes(FilterExpr);
  Result := BytesOf([SubscribeType, 0, 0, 0]);
  Result := ConcatBytes(Result, WriteUInt32LE(System.Length(ChannelBytes)));
  Result := ConcatBytes(Result, ChannelBytes);
  Result := ConcatBytes(Result, WriteUInt32LE(System.Length(FilterBytes)));
  Result := ConcatBytes(Result, FilterBytes);
end;

function BuildUnsubscribePayload(const Channel: string): TBytes;
var
  ChannelBytes: TBytes;
begin
  ChannelBytes := TEncoding.UTF8.GetBytes(Channel);
  Result := ConcatBytes(WriteUInt32LE(System.Length(ChannelBytes)), ChannelBytes);
end;

function CanonicalReadCommittedModeName(ReadCommittedMode: Byte): string;
begin
  case ReadCommittedMode of
    READ_COMMITTED_MODE_DEFAULT:
      Result := 'READ COMMITTED';
    READ_COMMITTED_MODE_READ_CONSISTENCY:
      Result := 'READ COMMITTED READ CONSISTENCY';
    READ_COMMITTED_MODE_RECORD_VERSION:
      Result := 'READ COMMITTED RECORD VERSION';
    READ_COMMITTED_MODE_NO_RECORD_VERSION:
      Result := 'READ COMMITTED NO RECORD VERSION';
  else
    Result := 'UNKNOWN(' + IntToStr(ReadCommittedMode) + ')';
  end;
end;

function BuildTxnBeginPayload(Flags: Word; ConflictAction, AutocommitMode, IsolationLevel, AccessMode, Deferrable, WaitMode: Byte;
  TimeoutMs: Cardinal; ReadCommittedMode: Byte): TBytes;
begin
  Result := WriteUInt16LE(Flags);
  Result := ConcatBytes(Result, BytesOf([ConflictAction, AutocommitMode, IsolationLevel, AccessMode, Deferrable, WaitMode]));
  Result := ConcatBytes(Result, WriteUInt32LE(TimeoutMs));
  if (Flags and TXN_FLAG_HAS_READ_COMMITTED_MODE) <> 0 then
    Result := ConcatBytes(Result, BytesOf([ReadCommittedMode, 0, 0, 0]));
end;

function BuildTxnCommitPayload(Flags: Byte): TBytes;
begin
  Result := BytesOf([Flags, 0, 0, 0]);
end;

function BuildTxnRollbackPayload(Flags: Byte): TBytes;
begin
  Result := BytesOf([Flags, 0, 0, 0]);
end;

function BuildTxnSavepointPayload(const Name: string): TBytes;
var
  NameBytes: TBytes;
begin
  NameBytes := TEncoding.UTF8.GetBytes(Name);
  Result := ConcatBytes(WriteUInt32LE(System.Length(NameBytes)), NameBytes);
end;

function BuildTxnReleasePayload(const Name: string): TBytes;
begin
  Result := BuildTxnSavepointPayload(Name);
end;

function BuildTxnRollbackToPayload(const Name: string): TBytes;
begin
  Result := BuildTxnSavepointPayload(Name);
end;

function BuildSetOptionPayload(const Name, Value: string): TBytes;
var
  NameBytes, ValueBytes: TBytes;
begin
  NameBytes := TEncoding.UTF8.GetBytes(Name);
  ValueBytes := TEncoding.UTF8.GetBytes(Value);
  Result := ConcatBytes(WriteUInt32LE(System.Length(NameBytes)), NameBytes);
  Result := ConcatBytes(Result, WriteUInt32LE(System.Length(ValueBytes)));
  Result := ConcatBytes(Result, ValueBytes);
end;

function BuildStreamControlPayload(ControlType: Byte; WindowSize, TimeoutMs: Cardinal): TBytes;
begin
  Result := BytesOf([ControlType, 0, 0, 0]);
  Result := ConcatBytes(Result, WriteUInt32LE(WindowSize));
  Result := ConcatBytes(Result, WriteUInt32LE(TimeoutMs));
end;

function BuildAttachCreatePayload(const EmulationMode, DbName: string): TBytes;
var
  ModeBytes, DbBytes: TBytes;
begin
  ModeBytes := TEncoding.UTF8.GetBytes(EmulationMode);
  DbBytes := TEncoding.UTF8.GetBytes(DbName);
  Result := ConcatBytes(WriteUInt32LE(System.Length(ModeBytes)), ModeBytes);
  Result := ConcatBytes(Result, WriteUInt32LE(System.Length(DbBytes)));
  Result := ConcatBytes(Result, DbBytes);
end;

procedure ParseAuthRequest(const Payload: TBytes; out Method: Byte; out Data: TBytes);
var
  Offset: Integer;
begin
  if System.Length(Payload) < 4 then
    raise Exception.Create('Auth request truncated');
  Method := Payload[0];
  Offset := 4;
  if Offset < System.Length(Payload) then
  begin
    SetLength(Data, System.Length(Payload) - Offset);
    Move(Payload[Offset], Data[0], System.Length(Data));
  end
  else
    Data := nil;
end;

procedure ParseAuthContinue(const Payload: TBytes; out Method, Stage: Byte; out Data: TBytes);
var
  DataLen, Offset: Integer;
begin
  if System.Length(Payload) < 8 then
    raise Exception.Create('Auth continue truncated');
  Method := Payload[0];
  Stage := Payload[1];
  DataLen := Integer(ReadUInt32LE(Payload, 4));
  Offset := 8;
  if Offset + DataLen > System.Length(Payload) then
    raise Exception.Create('Auth continue truncated');
  if DataLen > 0 then
  begin
    SetLength(Data, DataLen);
    Move(Payload[Offset], Data[0], DataLen);
  end
  else
    Data := nil;
end;

procedure ParseAuthOk(const Payload: TBytes; out SessionId: TBytes; out ServerInfo: TBytes);
var
  InfoLen: Integer;
begin
  if System.Length(Payload) < 20 then
    raise Exception.Create('Auth ok truncated');
  SetLength(SessionId, 16);
  Move(Payload[0], SessionId[0], 16);
  InfoLen := Integer(ReadUInt32LE(Payload, 16));
  if 20 + InfoLen > System.Length(Payload) then
    raise Exception.Create('Auth ok truncated');
  if InfoLen > 0 then
  begin
    SetLength(ServerInfo, InfoLen);
    Move(Payload[20], ServerInfo[0], InfoLen);
  end
  else
    ServerInfo := nil;
end;

procedure ParseReady(const Payload: TBytes; out Status: Byte; out TxnId, Visibility: UInt64);
begin
  if System.Length(Payload) < 20 then
    raise Exception.Create('Ready truncated');
  Status := Payload[0];
  TxnId := ReadUInt64LE(Payload, 4);
  Visibility := ReadUInt64LE(Payload, 12);
end;

procedure ParseTxnStatus(const Payload: TBytes; out Status: Byte; out TxnId: UInt64);
begin
  if System.Length(Payload) < 12 then
    raise Exception.Create('Txn status truncated');
  Status := Payload[0];
  TxnId := ReadUInt64LE(Payload, 4);
end;

procedure ParseParameterStatus(const Payload: TBytes; out Name, Value: string);
var
  Offset, NameLen, ValueLen: Integer;
begin
  if System.Length(Payload) < 8 then
    raise Exception.Create('Parameter status truncated');
  Offset := 0;
  NameLen := Integer(ReadUInt32LE(Payload, Offset));
  Offset := Offset + 4;
  Name := TEncoding.UTF8.GetString(Payload, Offset, NameLen);
  Offset := Offset + NameLen;
  ValueLen := Integer(ReadUInt32LE(Payload, Offset));
  Offset := Offset + 4;
  Value := TEncoding.UTF8.GetString(Payload, Offset, ValueLen);
end;

function ParseParameterDescription(const Payload: TBytes): TArray<Cardinal>;
var
  Offset, Count, I: Integer;
begin
  if System.Length(Payload) < 4 then
    raise Exception.Create('Parameter description truncated');
  Offset := 0;
  Count := ReadUInt16LE(Payload, Offset);
  Offset := Offset + 4;
  SetLength(Result, Count);
  for I := 0 to Count - 1 do
  begin
    if Offset + 4 > System.Length(Payload) then
      raise Exception.Create('Parameter description truncated');
    Result[I] := ReadUInt32LE(Payload, Offset);
    Offset := Offset + 4;
  end;
end;

function ParseRowDescription(const Payload: TBytes): TArray<TColumnInfo>;
var
  Offset, Count, I: Integer;
  Col: TColumnInfo;
  NameLen: Integer;
begin
  if System.Length(Payload) < 4 then
    raise Exception.Create('Row description truncated');
  Offset := 0;
  Count := ReadUInt16LE(Payload, Offset);
  Offset := Offset + 4;
  SetLength(Result, Count);
  for I := 0 to Count - 1 do
  begin
    NameLen := Integer(ReadUInt32LE(Payload, Offset));
    Offset := Offset + 4;
    Col.Name := TEncoding.UTF8.GetString(Payload, Offset, NameLen);
    Offset := Offset + NameLen;
    Col.TableOid := ReadUInt32LE(Payload, Offset);
    Offset := Offset + 4;
    Col.ColumnIndex := ReadUInt16LE(Payload, Offset);
    Offset := Offset + 2;
    Col.TypeOid := ReadUInt32LE(Payload, Offset);
    Offset := Offset + 4;
    Col.TypeSize := ReadInt16LE(Payload, Offset);
    Offset := Offset + 2;
    Col.TypeModifier := ReadInt32LE(Payload, Offset);
    Offset := Offset + 4;
    Col.Format := Payload[Offset];
    Offset := Offset + 1;
    Col.Nullable := Payload[Offset] = 1;
    Offset := Offset + 3;
    Result[I] := Col;
  end;
end;

function ParseRowData(const Payload: TBytes): TArray<TColumnValue>;
var
  Offset, Count, NullBytes, I: Integer;
  Values: TArray<TColumnValue>;
  NullOffset, DataOffset: Integer;
  ByteIndex, BitIndex: Integer;
  IsNull: Boolean;
  Len: Integer;
begin
  if System.Length(Payload) < 4 then
    raise Exception.Create('Row data truncated');
  Offset := 0;
  Count := ReadUInt16LE(Payload, Offset);
  Offset := Offset + 2;
  NullBytes := ReadUInt16LE(Payload, Offset);
  Offset := Offset + 2;
  NullOffset := Offset;
  DataOffset := NullOffset + NullBytes;
  SetLength(Values, Count);
  for I := 0 to Count - 1 do
  begin
    ByteIndex := I div 8;
    BitIndex := I mod 8;
    IsNull := (ByteIndex < NullBytes) and ((Payload[NullOffset + ByteIndex] and (1 shl BitIndex)) <> 0);
    if IsNull then
    begin
      Values[I].IsNull := True;
      Values[I].Data := nil;
      Continue;
    end;
    Len := ReadInt32LE(Payload, DataOffset);
    DataOffset := DataOffset + 4;
    if Len < 0 then
    begin
      Values[I].IsNull := True;
      Values[I].Data := nil;
      Continue;
    end;
    SetLength(Values[I].Data, Len);
    if Len > 0 then
      Move(Payload[DataOffset], Values[I].Data[0], Len);
    Values[I].IsNull := False;
    DataOffset := DataOffset + Len;
  end;
  Result := Values;
end;

procedure ParseCommandComplete(const Payload: TBytes; out CommandType: Byte; out Rows, LastId: UInt64; out Tag: string);
var
  TagBytes: TBytes;
  TagStr: string;
  ZeroPos: Integer;
begin
  if System.Length(Payload) < 20 then
    raise Exception.Create('Command complete truncated');
  CommandType := Payload[0];
  Rows := ReadUInt64LE(Payload, 4);
  LastId := ReadUInt64LE(Payload, 12);
  if System.Length(Payload) > 20 then
  begin
    SetLength(TagBytes, System.Length(Payload) - 20);
    Move(Payload[20], TagBytes[0], System.Length(TagBytes));
    TagStr := TEncoding.UTF8.GetString(TagBytes);
    ZeroPos := Pos(#0, TagStr);
    if ZeroPos > 0 then
      Tag := Copy(TagStr, 1, ZeroPos - 1)
    else
      Tag := TagStr;
  end
  else
    Tag := '';
end;

procedure ParseErrorMessage(const Payload: TBytes; out Severity, SqlState, Message, Detail, Hint: string);
var
  Offset, Start: Integer;
  Field: Byte;
  Value: string;
begin
  Severity := '';
  SqlState := '';
  Message := '';
  Detail := '';
  Hint := '';
  Offset := 0;
  while Offset < System.Length(Payload) do
  begin
    Field := Payload[Offset];
    Inc(Offset);
    if Field = 0 then
      Break;
    Start := Offset;
    while (Offset < System.Length(Payload)) and (Payload[Offset] <> 0) do
      Inc(Offset);
    if Offset > System.Length(Payload) then
      Break;
    Value := TEncoding.UTF8.GetString(Payload, Start, Offset - Start);
    Inc(Offset);
    case AnsiChar(Field) of
      'S': Severity := Value;
      'C': SqlState := Value;
      'M': Message := Value;
      'D': Detail := Value;
      'H': Hint := Value;
    end;
  end;
end;

end.
