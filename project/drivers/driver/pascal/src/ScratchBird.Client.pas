// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

unit ScratchBird.Client;

{$mode delphi}
{$H+}

interface

uses
  SysUtils, Classes, DateUtils,
  ScratchBird.Config, ScratchBird.Protocol, ScratchBird.AuthBootstrap, ScratchBird.Errors, ScratchBird.Scram, ScratchBird.Types, ScratchBird.Sql, ScratchBird.Metadata,
  ScratchBird.Transport, ScratchBird.Transport.Native,
  {$IFDEF SCRATCHBIRD_USE_INDY}
  ScratchBird.Transport.Indy,
  {$ENDIF}
  SBCircuitBreaker, SBKeepalive, SBLeakDetector, SBTelemetry
  {$IFDEF MSWINDOWS}
  , Windows
  {$ENDIF}
  {$IFNDEF MSWINDOWS}
  , BaseUnix
  {$ENDIF};

type
  TScratchBirdResultStream = class
  private
    FClient: TObject;
    FColumns: TArray<TColumnInfo>;
    FRowsAffected: Int64;
    FCommandTag: string;
    FDone: Boolean;
    FSeenRows: Int64;
    FLastInsertId: UInt64;
    FHasLastInsertId: Boolean;
    FResponseStarted: Boolean;
    FIgnoredStrayReady: Boolean;
  public
    constructor Create(Client: TObject);
    destructor Destroy; override;
    function ReadRow: TArray<Variant>;
    property Columns: TArray<TColumnInfo> read FColumns;
    property RowsAffected: Int64 read FRowsAffected;
    property CommandTag: string read FCommandTag;
    property LastInsertId: UInt64 read FLastInsertId;
    property HasLastInsertId: Boolean read FHasLastInsertId;
  end;

  TNotification = record
    ProcessId: Cardinal;
    Channel: string;
    Payload: TBytes;
    ChangeType: string;
    RowId: UInt64;
    HasRowId: Boolean;
  end;

  TNotificationHandler = procedure(const Notice: TNotification) of object;

  TNotificationListenerEntry = record
    Id: UInt64;
    Handler: TNotificationHandler;
  end;

  TQueryPlan = record
    Format: Cardinal;
    PlanningTimeUs: UInt64;
    EstimatedRows: UInt64;
    EstimatedCost: UInt64;
    Plan: TBytes;
  end;

  TSblrCompiled = record
    Hash: UInt64;
    Version: Cardinal;
    Bytecode: TBytes;
  end;

  TScratchBirdBatchResult = record
    RowsAffected: Int64;
    CommandTag: string;
    LastInsertId: UInt64;
    HasLastInsertId: Boolean;
  end;

  TScratchBirdBatchResults = TArray<TScratchBirdBatchResult>;

  TScratchBirdRowset = record
    Columns: TArray<TColumnInfo>;
    Rows: TArray<TArray<Variant>>;
    RowsAffected: Int64;
    CommandTag: string;
    LastInsertId: UInt64;
    HasLastInsertId: Boolean;
  end;

  TScratchBirdRowsets = TArray<TScratchBirdRowset>;

  TScratchBirdClient = class
  private
    FConfig: TScratchBirdConfig;
    FTransport: IScratchBirdTransport;
    FConnected: Boolean;
    FAttachmentId: TBytes;
    FTxnId: UInt64;
    FTransactionActive: Boolean;
    FRuntimeBoundarySeen: Boolean;
    FExplicitTransaction: Boolean;
    FPortalResumePending: Boolean;
    FSequence: Cardinal;
    FLastQuerySequence: Cardinal;
    FLastMaxRows: Cardinal;
    FParameters: TStringList;
    FOnNotification: TNotificationHandler;
    FLastPlan: TQueryPlan;
    FHasLastPlan: Boolean;
    FLastSblr: TSblrCompiled;
    FHasLastSblr: Boolean;
    FNotificationQueue: TArray<TNotification>;
    FNotificationListeners: TArray<TNotificationListenerEntry>;
    FNextNotificationListenerId: UInt64;
    FCircuitBreaker: TCircuitBreaker;
    FTelemetry: TTelemetryCollector;
    FKeepaliveTracker: TKeepaliveTracker;
    FLeakDetector: TLeakDetector;
    FConnectionId: string;
    FResolvedAuthContext: TScratchBirdResolvedAuthContext;
    function ReadExact(Length: Integer): TBytes;
    procedure SendBytes(const Data: TBytes);
    function ReceiveMessage: TScratchBirdMessage;
    procedure SendManagerFrame(MsgType: Byte; const Payload: TBytes);
    procedure ReceiveManagerFrame(out MsgType: Byte; out Payload: TBytes);
    procedure AppendLengthPrefixedString(var Buffer: TBytes; const Value: string);
    procedure PerformManagerConnect;
    procedure HandshakeAndAuth;
    function BuildStartupParams: TStringList;
    procedure ResetResolvedAuthContext;
    procedure MarkResolvedAuthContextDetached;
    function ProbeDirectAuthSurface: TScratchBirdAuthProbeResult;
    function ProbeManagerAuthSurface: TScratchBirdAuthProbeResult;
    procedure ApplySchema;
    function BuildQueryError(const Payload: TBytes): EScratchBirdError;
    procedure HandleParameterStatus(const Name, Value: string);
    function HandleAsyncMessage(const Msg: TScratchBirdMessage): Boolean;
    procedure DrainUntilReady;
    function DescribeStatement(const StatementName: string): Integer;
    function SendMessage(MsgType: TScratchBirdMessageType; const Payload: TBytes; Flags: Byte; ForceZero: Boolean): Cardinal;
    procedure SendSimpleQuery(const Sql: string; MaxRows: Cardinal);
    procedure SendExtendedQuery(const Sql: string; const Params: array of TScratchBirdParamInput; MaxRows: Cardinal);
    function CurrentMaxRows: Cardinal;
    procedure ParseNotification(const Payload: TBytes; out Notice: TNotification);
    procedure ParseQueryPlan(const Payload: TBytes; out Plan: TQueryPlan);
    procedure ParseSblrCompiled(const Payload: TBytes; out Compiled: TSblrCompiled);
    function ParseUuidBytes(const Value: string): TBytes;
    procedure EnsureConnected;
    procedure EnsureTransactionActive(const Operation: string);
    procedure ApplyRuntimeTxnId(TxnId: UInt64);
    procedure ApplyRuntimeReadyState(Status: Byte; TxnId: UInt64);
    procedure ClearTransactionState;
    function CanAdoptFreshNativeBoundary(IsolationLevel: Byte; AccessMode: Byte; Deferrable: Boolean;
      WaitMode: Boolean; TimeoutMs: Cardinal; AutocommitMode: Byte; ConflictAction: Byte;
      HasReadCommittedMode: Boolean; ReadCommittedMode: Byte): Boolean;
    function NormalizeSavepointName(const Name: string): string;
    function NormalizeSqlText(const Sql: string): string;
    function QuoteStringLiteral(const Value: string): string;
    function BuildPreparedTransactionSql(const Verb, GlobalTransactionId: string): string;
    procedure AllowPortalResume;
    procedure ResumeSuspendedPortal(MaxRows: Cardinal);
    procedure EnqueueNotification(const Notice: TNotification);
    procedure DispatchNotificationListeners(const Notice: TNotification);
    procedure InitializeClient(const Transport: IScratchBirdTransport);
    function BeginOperation(const Name, Sql: string): TSpanContext;
    procedure EndOperation(Span: TSpanContext; Success: Boolean);
    procedure BeginTransactionExInternal(IsolationLevel: Byte; AccessMode: Byte; Deferrable: Boolean; WaitMode: Boolean;
      TimeoutMs: Cardinal; AutocommitMode: Byte; ConflictAction: Byte; HasReadCommittedMode: Boolean;
      ReadCommittedMode: Byte);
  public
    constructor Create; overload;
    constructor CreateWithTransport(const Transport: IScratchBirdTransport; StartConnected: Boolean = False); overload;
    destructor Destroy; override;
    procedure Connect(const Dsn: string);
    procedure Disconnect;
    function ProbeAuthSurface(const Dsn: string): TScratchBirdAuthProbeResult;
    function GetResolvedAuthContext: TScratchBirdResolvedAuthContext;
    procedure BeginTransaction;
    procedure BeginTransactionEx(IsolationLevel: Byte; AccessMode: Byte; Deferrable: Boolean; WaitMode: Boolean;
      TimeoutMs: Cardinal; AutocommitMode: Byte; ConflictAction: Byte); overload;
    procedure BeginTransactionEx(IsolationLevel: Byte; AccessMode: Byte; Deferrable: Boolean; WaitMode: Boolean;
      TimeoutMs: Cardinal; AutocommitMode: Byte; ConflictAction: Byte; ReadCommittedMode: Byte); overload;
    procedure Commit(Flags: Byte = 0);
    procedure Rollback(Flags: Byte = 0);
    function SupportsPreparedTransactions: Boolean;
    procedure PrepareTransaction(const GlobalTransactionId: string);
    procedure CommitPrepared(const GlobalTransactionId: string);
    procedure RollbackPrepared(const GlobalTransactionId: string);
    function SupportsDormantReattach: Boolean;
    procedure DetachToDormant;
    procedure ReattachDormant(const DormantId: string; const AuthToken: string = '');
    procedure Savepoint(const Name: string);
    procedure ReleaseSavepoint(const Name: string);
    procedure RollbackToSavepoint(const Name: string);
    procedure SetOption(const Name, Value: string);
    procedure Ping;
    procedure Terminate;
    procedure Subscribe(SubscribeType: Byte; const Channel, FilterExpr: string);
    procedure Unsubscribe(const Channel: string);
    procedure Listen(const Channel: string; const FilterExpr: string = '');
    procedure Unlisten(const Channel: string);
    procedure UnlistenAll;
    procedure NotifyChannel(const Channel: string); overload;
    procedure NotifyChannel(const Channel, Payload: string); overload;
    function AddNotificationListener(const Handler: TNotificationHandler): UInt64;
    function RemoveNotificationListener(ListenerId: UInt64): Boolean;
    function GetNotification(out Notice: TNotification): Boolean;
    function GetNotifications: TArray<TNotification>;
    procedure ClearNotifications;
    function NotificationCount: Integer;
    function ExecuteSblr(SblrHash: UInt64; const Bytecode: TBytes; const Params: array of TScratchBirdParamInput): TScratchBirdResultStream;
    procedure StreamControl(ControlType: Byte; WindowSize, TimeoutMs: Cardinal);
    procedure AttachCreate(const EmulationMode, DbName: string);
    procedure AttachDetach;
    function AttachList: TScratchBirdResultStream;
    procedure ExecSQL(const Sql: string);
    procedure ExecSQLParams(const Sql: string; const Params: array of TScratchBirdParamInput);
    function ExecuteQuery(const Sql: string): TScratchBirdResultStream;
    function ExecuteQueryParams(const Sql: string; const Params: array of TScratchBirdParamInput): TScratchBirdResultStream;
    function ExecuteBatch(const Statements: array of string): TScratchBirdBatchResults;
    function QueryMulti(const Statements: array of string): TScratchBirdRowsets;
    function QueryMetadata(const CollectionName: string = 'tables'): TScratchBirdResultStream;
    function GetSchema(const CollectionName: string = 'tables'): TScratchBirdResultStream;
    function QueryMetadataRows(const CollectionName: string = 'tables'): TMetadataRows; overload;
    function QueryMetadataRows(const CollectionName: string; const Restrictions: TMetadataRow): TMetadataRows; overload;
    function GetSchemaRows(const CollectionName: string = 'tables'): TMetadataRows; overload;
    function GetSchemaRows(const CollectionName: string; const Restrictions: TMetadataRow): TMetadataRows; overload;
    function GetCatalogs: TScratchBirdResultStream;
    function GetSchemas: TScratchBirdResultStream;
    function GetTables: TScratchBirdResultStream;
    function GetColumns: TScratchBirdResultStream;
    function GetIndexes: TScratchBirdResultStream;
    function GetIndexColumns: TScratchBirdResultStream;
    function GetConstraints: TScratchBirdResultStream;
    function GetProcedures: TScratchBirdResultStream;
    function GetFunctions: TScratchBirdResultStream;
    function GetRoutines: TScratchBirdResultStream;
    function GetPrimaryKeys: TScratchBirdResultStream;
    function GetForeignKeys: TScratchBirdResultStream;
    function GetTablePrivileges: TScratchBirdResultStream;
    function GetColumnPrivileges: TScratchBirdResultStream;
    function GetTypeInfo: TScratchBirdResultStream;
    function GetDiagnosticsJson: string;
    function GetTelemetrySummaryJson: string;
    procedure ResetTelemetry;
    function GetSlowOperationsJson: string;
    function ExportTelemetryPrometheus: string;
    function GetCircuitBreakerSummaryJson: string;
    function GetKeepaliveSummaryJson: string;
    function GetLeakSummaryJson: string;
    procedure Cancel;
    function GetLastPlan(out Plan: TQueryPlan): Boolean;
    function GetLastSblr(out Compiled: TSblrCompiled): Boolean;
    property OnNotification: TNotificationHandler read FOnNotification write FOnNotification;
    property Connected: Boolean read FConnected;
    property Config: TScratchBirdConfig read FConfig;
  end;

implementation

const
  MANAGER_PROTOCOL_MAGIC = $42444253; // SBDB
  MANAGER_PROTOCOL_VERSION = $0101;
  MANAGER_HEADER_SIZE = 12;
  MANAGER_MAX_PAYLOAD_SIZE = 16 * 1024 * 1024;
  MCP_PROTOCOL_VERSION = $0100;

  MCP_MSG_CONNECT_RESPONSE = $02;
  MCP_MSG_AUTH_CHALLENGE = $12;
  MCP_MSG_AUTH_RESPONSE = $11;
  MCP_MSG_STATUS_RESPONSE = $64;
  MCP_MSG_HELLO = $65;
  MCP_MSG_AUTH_START = $66;
  MCP_MSG_AUTH_CONTINUE = $67;
  MCP_MSG_DB_CONNECT = $69;
  MCP_AUTH_METHOD_TOKEN = 4;

function ReadUInt16LEValue(const Data: TBytes; Offset: Integer): Word;
begin
  Result := Word(Data[Offset]) or (Word(Data[Offset + 1]) shl 8);
end;

function ReadUInt32LEValue(const Data: TBytes; Offset: Integer): Cardinal;
begin
  Result := Cardinal(Data[Offset]) or (Cardinal(Data[Offset + 1]) shl 8) or
    (Cardinal(Data[Offset + 2]) shl 16) or (Cardinal(Data[Offset + 3]) shl 24);
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

function GetProcessIdValue: Cardinal;
begin
  {$IFDEF MSWINDOWS}
  Result := GetCurrentProcessId;
  {$ELSE}
  Result := fpGetPid;
  {$ENDIF}
end;

function QuoteIdentifier(const Name: string): string;
begin
  Result := '"' + StringReplace(Name, '"', '""', [rfReplaceAll]) + '"';
end;

function BuildSchemaStatement(const Schema: string): string;
var
  Parts: TStringList;
  I: Integer;
  Trimmed: string;
begin
  Trimmed := Trim(Schema);
  if Trimmed = '' then
    Exit('');
  if Pos(',', Trimmed) > 0 then
  begin
    Parts := TStringList.Create;
    try
      ExtractStrings([','], [], PChar(Trimmed), Parts);
      for I := Parts.Count - 1 downto 0 do
      begin
        Parts[I] := Trim(Parts[I]);
        if Parts[I] = '' then
          Parts.Delete(I)
        else
          Parts[I] := QuoteIdentifier(Parts[I]);
      end;
      if Parts.Count = 0 then
        Exit('');
      Result := 'SET SEARCH_PATH TO ' + StringReplace(Parts.CommaText, ',', ', ', [rfReplaceAll]);
      Exit;
    finally
      Parts.Free;
    end;
  end;
  Result := 'SET SCHEMA ' + QuoteIdentifier(Trimmed);
end;

function EscapeJson(const Value: string): string;
var
  I: Integer;
  Ch: Char;
begin
  Result := '';
  for I := 1 to Length(Value) do
  begin
    Ch := Value[I];
    case Ch of
      '"': Result := Result + '\"';
      '\': Result := Result + '\\';
      #8: Result := Result + '\b';
      #9: Result := Result + '\t';
      #10: Result := Result + '\n';
      #12: Result := Result + '\f';
      #13: Result := Result + '\r';
    else
      if Ord(Ch) < 32 then
        Result := Result + '\u00' + IntToHex(Ord(Ch), 2)
      else
        Result := Result + Ch;
    end;
  end;
end;

function BytesToHex(const Data: TBytes): string;
const
  HexChars: array[0..15] of Char = ('0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f');
var
  I: Integer;
begin
  SetLength(Result, Length(Data) * 2);
  for I := 0 to High(Data) do
  begin
    Result[I * 2 + 1] := HexChars[(Data[I] shr 4) and $0F];
    Result[I * 2 + 2] := HexChars[Data[I] and $0F];
  end;
end;

constructor TScratchBirdResultStream.Create(Client: TObject);
begin
  inherited Create;
  FClient := Client;
  FRowsAffected := -1;
  FCommandTag := '';
  FDone := False;
  FSeenRows := 0;
  FLastInsertId := 0;
  FHasLastInsertId := False;
  FResponseStarted := False;
  FIgnoredStrayReady := False;
end;

destructor TScratchBirdResultStream.Destroy;
begin
  if not FDone then
  begin
    try
      while ReadRow <> nil do
      begin
      end;
    except
    end;
  end;
  inherited Destroy;
end;

function TScratchBirdResultStream.ReadRow: TArray<Variant>;
var
  Client: TScratchBirdClient;
  Msg: TScratchBirdMessage;
  Values: TArray<TColumnValue>;
  Row: TArray<Variant>;
  I: Integer;
  CommandType: Byte;
  Rows, LastId: UInt64;
  Tag: string;
  Status: Byte;
  TxnId, Visibility: UInt64;
begin
  if FDone then
    Exit(nil);
  Client := TScratchBirdClient(FClient);
  while True do
  begin
    Msg := Client.ReceiveMessage;
    if Client.HandleAsyncMessage(Msg) then
      Continue;
    case Msg.MsgType of
      MSG_ERROR:
        raise Client.BuildQueryError(Msg.Payload);
      MSG_ROW_DESCRIPTION:
        begin
          FColumns := ParseRowDescription(Msg.Payload);
          FResponseStarted := True;
        end;
      MSG_DATA_ROW:
      begin
        Values := ParseRowData(Msg.Payload);
        FResponseStarted := True;
        SetLength(Row, Length(Values));
        for I := 0 to High(Values) do
        begin
          if I < Length(FColumns) then
            Row[I] := DecodeValue(FColumns[I].TypeOid, Values[I].Data, FColumns[I].Format)
          else
            Row[I] := DecodeValue(0, Values[I].Data, FORMAT_BINARY);
        end;
        Inc(FSeenRows);
        Exit(Row);
      end;
      MSG_COMMAND_COMPLETE:
      begin
        ParseCommandComplete(Msg.Payload, CommandType, Rows, LastId, Tag);
        FResponseStarted := True;
        FCommandTag := Tag;
        FRowsAffected := Rows;
        FLastInsertId := LastId;
        FHasLastInsertId := LastId <> 0;
      end;
      MSG_PORTAL_SUSPENDED:
      begin
        FResponseStarted := True;
        Client.AllowPortalResume;
        Client.ResumeSuspendedPortal(Client.CurrentMaxRows);
      end;
      MSG_READY:
      begin
        ParseReady(Msg.Payload, Status, TxnId, Visibility);
        Client.ApplyRuntimeReadyState(Status, TxnId);
        if (not FResponseStarted) and (not FIgnoredStrayReady) then
        begin
          FIgnoredStrayReady := True;
          Continue;
        end;
        FDone := True;
        if FRowsAffected < 0 then
          FRowsAffected := FSeenRows;
        Exit(nil);
      end;
    end;
  end;
end;

procedure TScratchBirdClient.InitializeClient(const Transport: IScratchBirdTransport);
begin
  FTransport := Transport;
  if not Assigned(FTransport) then
    raise EScratchbirdConnectionError.CreateWithInfo('Transport is not assigned', '08001', '', '');

  SetLength(FAttachmentId, 16);
  FillChar(FAttachmentId[0], 16, 0);
  FSequence := 0;
  FTxnId := 0;
  FTransactionActive := False;
  FRuntimeBoundarySeen := False;
  FExplicitTransaction := False;
  FPortalResumePending := False;
  FLastMaxRows := 0;
  FParameters := TStringList.Create;
  FHasLastPlan := False;
  FHasLastSblr := False;
  FOnNotification := nil;
  SetLength(FNotificationQueue, 0);
  SetLength(FNotificationListeners, 0);
  FNextNotificationListenerId := 1;
  FConnectionId := 'conn-' + IntToStr(NativeInt(Self));
  FCircuitBreaker := TCircuitBreaker.Create(DefaultCircuitBreakerConfig, 'pascal');
  FTelemetry := TTelemetryCollector.Create(DefaultTelemetryConfig);
  FKeepaliveTracker := TKeepaliveTracker.Create(DefaultKeepaliveConfig);
  FLeakDetector := TLeakDetector.Create(DefaultLeakDetectionConfig);
  FLeakDetector.Start;
  FResolvedAuthContext := DefaultResolvedAuthContext('direct');
end;

constructor TScratchBirdClient.Create;
var
  Transport: IScratchBirdTransport;
begin
  inherited Create;
  {$IFDEF SCRATCHBIRD_USE_INDY}
  Transport := TIndyScratchBirdTransport.Create;
  {$ELSE}
  Transport := TNativeScratchBirdTransport.Create;
  {$ENDIF}
  InitializeClient(Transport);
end;

constructor TScratchBirdClient.CreateWithTransport(const Transport: IScratchBirdTransport; StartConnected: Boolean);
begin
  inherited Create;
  InitializeClient(Transport);
  if StartConnected then
  begin
    FConnected := True;
    FTransactionActive := True;
    FRuntimeBoundarySeen := False;
    FExplicitTransaction := False;
  end;
end;

destructor TScratchBirdClient.Destroy;
begin
  Disconnect;
  if Assigned(FLeakDetector) then
  begin
    FLeakDetector.Checkin(FConnectionId);
    FLeakDetector.Stop;
    FLeakDetector.Free;
  end;
  FKeepaliveTracker.Free;
  FTelemetry.Free;
  FCircuitBreaker.Free;
  FParameters.Free;
  inherited Destroy;
end;

procedure TScratchBirdClient.Connect(const Dsn: string);
begin
  FConfig := ParseConfig(Dsn);
  if (Trim(FConfig.Protocol) = '') or SameText(FConfig.Protocol, 'native') or
     SameText(FConfig.Protocol, 'scratchbird') or SameText(FConfig.Protocol, 'scratchbird-native') or
     SameText(FConfig.Protocol, 'scratchbird_native') then
    FConfig.Protocol := 'native'
  else
    raise EScratchbirdNotSupported.CreateWithInfo(
      'Only protocol=native is supported; connect to the native parser listener/port.',
      '0A000', '', '');
  if (FConfig.UserName = '') or (FConfig.Database = '') then
    raise EScratchbirdConnectionError.CreateWithInfo('user and database are required', '08001', '', '');
  FConfig.FrontDoorMode := LowerCase(Trim(FConfig.FrontDoorMode));
  if (FConfig.FrontDoorMode = '') then
    FConfig.FrontDoorMode := 'direct'
  else if (FConfig.FrontDoorMode = 'manager-proxy') or (FConfig.FrontDoorMode = 'managed') then
    FConfig.FrontDoorMode := 'manager_proxy';
  if (FConfig.FrontDoorMode <> 'direct') and (FConfig.FrontDoorMode <> 'manager_proxy') then
    raise EScratchbirdNotSupported.CreateWithInfo(
      'front_door_mode must be direct or manager_proxy.',
      '0A000', '', '');
  if (FConfig.FrontDoorMode = 'manager_proxy') and (Trim(FConfig.ManagerAuthToken) = '') then
    raise EScratchbirdConnectionError.CreateWithInfo(
      'manager_proxy mode requires manager_auth_token',
      '08001', '', '');
  ResetResolvedAuthContext;
  FTransport.Configure(FConfig);
  FTransport.Connect;
  try
    if FConfig.FrontDoorMode = 'manager_proxy' then
      PerformManagerConnect;
    HandshakeAndAuth;
    FConnected := True;
    ApplySchema;
    if Assigned(FLeakDetector) then
      FLeakDetector.Checkout(FConnectionId, []);
  except
    on E: Exception do
    begin
      if FTransport.IsConnected then
        FTransport.Disconnect;
      FConnected := False;
      MarkResolvedAuthContextDetached;
      raise;
    end;
  end;
end;

procedure TScratchBirdClient.Disconnect;
begin
  if not FConnected then
    Exit;
  FTransport.Disconnect;
  FConnected := False;
  ClearTransactionState;
  FPortalResumePending := False;
  FSequence := 0;
  FLastQuerySequence := 0;
  FLastMaxRows := 0;
  FRuntimeBoundarySeen := False;
  if Assigned(FParameters) then
    FParameters.Clear;
  FHasLastPlan := False;
  FHasLastSblr := False;
  SetLength(FNotificationQueue, 0);
  if Length(FAttachmentId) = 16 then
    FillChar(FAttachmentId[0], 16, 0);
  MarkResolvedAuthContextDetached;
  if Assigned(FLeakDetector) then
    FLeakDetector.Checkin(FConnectionId);
end;

procedure TScratchBirdClient.ResetResolvedAuthContext;
begin
  FResolvedAuthContext := DefaultResolvedAuthContext(FConfig.FrontDoorMode);
end;

procedure TScratchBirdClient.MarkResolvedAuthContextDetached;
begin
  FResolvedAuthContext.Attached := False;
  FResolvedAuthContext.ManagerAuthenticated := False;
end;

function TScratchBirdClient.GetResolvedAuthContext: TScratchBirdResolvedAuthContext;
begin
  Result := FResolvedAuthContext;
end;

function TScratchBirdClient.ProbeAuthSurface(const Dsn: string): TScratchBirdAuthProbeResult;
begin
  FConfig := ParseConfig(Dsn);
  if (Trim(FConfig.Protocol) = '') or SameText(FConfig.Protocol, 'native') or
     SameText(FConfig.Protocol, 'scratchbird') or SameText(FConfig.Protocol, 'scratchbird-native') or
     SameText(FConfig.Protocol, 'scratchbird_native') then
    FConfig.Protocol := 'native'
  else
    raise EScratchbirdNotSupported.CreateWithInfo(
      'Only protocol=native is supported; connect to the native parser listener/port.',
      '0A000', '', '');
  FConfig.FrontDoorMode := LowerCase(Trim(FConfig.FrontDoorMode));
  if FConfig.FrontDoorMode = '' then
    FConfig.FrontDoorMode := 'direct'
  else if (FConfig.FrontDoorMode = 'manager-proxy') or (FConfig.FrontDoorMode = 'managed') then
    FConfig.FrontDoorMode := 'manager_proxy';
  if (FConfig.FrontDoorMode <> 'direct') and (FConfig.FrontDoorMode <> 'manager_proxy') then
    raise EScratchbirdNotSupported.CreateWithInfo(
      'front_door_mode must be direct or manager_proxy.',
      '0A000', '', '');
  ResetResolvedAuthContext;
  FTransport.Configure(FConfig);
  FTransport.Connect;
  try
    if FConfig.FrontDoorMode = 'manager_proxy' then
      Result := ProbeManagerAuthSurface
    else
      Result := ProbeDirectAuthSurface;
  finally
    if FTransport.IsConnected then
      FTransport.Disconnect;
    MarkResolvedAuthContextDetached;
  end;
end;

procedure TScratchBirdClient.ApplySchema;
var
  Schema: string;
  Statement: string;
begin
  Schema := Trim(FConfig.Schema);
  if (Schema = '') or SameText(Schema, 'public') then
    Exit;
  Statement := BuildSchemaStatement(Schema);
  if Statement = '' then
    Exit;
  ExecSQL(Statement);
end;

procedure TScratchBirdClient.BeginTransaction;
begin
  BeginTransactionEx(ISOLATION_READ_COMMITTED, 0, False, False, 0, 0, 0);
end;

procedure TScratchBirdClient.BeginTransactionEx(IsolationLevel: Byte; AccessMode: Byte; Deferrable: Boolean; WaitMode: Boolean;
  TimeoutMs: Cardinal; AutocommitMode: Byte; ConflictAction: Byte);
begin
  BeginTransactionExInternal(IsolationLevel, AccessMode, Deferrable, WaitMode, TimeoutMs, AutocommitMode,
    ConflictAction, False, READ_COMMITTED_MODE_DEFAULT);
end;

procedure TScratchBirdClient.BeginTransactionEx(IsolationLevel: Byte; AccessMode: Byte; Deferrable: Boolean; WaitMode: Boolean;
  TimeoutMs: Cardinal; AutocommitMode: Byte; ConflictAction: Byte; ReadCommittedMode: Byte);
begin
  BeginTransactionExInternal(IsolationLevel, AccessMode, Deferrable, WaitMode, TimeoutMs, AutocommitMode,
    ConflictAction, True, ReadCommittedMode);
end;

procedure TScratchBirdClient.BeginTransactionExInternal(IsolationLevel: Byte; AccessMode: Byte; Deferrable: Boolean; WaitMode: Boolean;
  TimeoutMs: Cardinal; AutocommitMode: Byte; ConflictAction: Byte; HasReadCommittedMode: Boolean;
  ReadCommittedMode: Byte);
var
  Flags: Word;
  Payload: TBytes;
begin
  EnsureConnected;
  // Pascal currently forwards SQL-style compatibility isolation bytes:
  //   ISOLATION_READ_UNCOMMITTED => legacy alias, not a distinct canonical MGA mode
  //   ISOLATION_READ_COMMITTED   => canonical READ COMMITTED
  //   ISOLATION_REPEATABLE_READ  => canonical SNAPSHOT
  //   ISOLATION_SERIALIZABLE     => canonical SNAPSHOT TABLE STABILITY
  // ReadCommittedMode exposes the canonical READ COMMITTED sub-modes directly:
  //   READ_COMMITTED_MODE_DEFAULT           => READ COMMITTED
  //   READ_COMMITTED_MODE_READ_CONSISTENCY  => READ COMMITTED READ CONSISTENCY
  //   READ_COMMITTED_MODE_RECORD_VERSION    => READ COMMITTED RECORD VERSION
  //   READ_COMMITTED_MODE_NO_RECORD_VERSION => READ COMMITTED NO RECORD VERSION
  if HasReadCommittedMode and not (IsolationLevel in [ISOLATION_READ_UNCOMMITTED, ISOLATION_READ_COMMITTED]) then
    raise EScratchbirdNotSupported.CreateWithInfo(
      'ReadCommittedMode requires a READ COMMITTED isolation alias',
      '0A000',
      '',
      ''
    );
  if FRuntimeBoundarySeen then
  begin
    if FTransactionActive and (FTxnId = 0) then
    begin
      if FExplicitTransaction then
        raise EScratchbirdTransactionError.CreateWithInfo('transaction already active', '25001', '', '');
      if not CanAdoptFreshNativeBoundary(IsolationLevel, AccessMode, Deferrable, WaitMode, TimeoutMs,
        AutocommitMode, ConflictAction, HasReadCommittedMode, ReadCommittedMode) then
        raise EScratchbirdNotSupported.CreateWithInfo(
          'fresh native transaction boundaries only support default READ COMMITTED adoption in the Pascal lane',
          '0A000', '', '');
      FExplicitTransaction := True;
      Exit;
    end;
    if FTransactionActive then
      raise EScratchbirdTransactionError.CreateWithInfo('transaction already active', '25001', '', '');
  end;
  Flags := TXN_FLAG_HAS_ISOLATION;
  if HasReadCommittedMode then
    Flags := Flags or TXN_FLAG_HAS_READ_COMMITTED_MODE;
  if AccessMode <> 0 then
    Flags := Flags or TXN_FLAG_HAS_ACCESS;
  if Deferrable then
    Flags := Flags or TXN_FLAG_HAS_DEFERRABLE;
  if WaitMode then
    Flags := Flags or TXN_FLAG_HAS_WAIT;
  if TimeoutMs > 0 then
    Flags := Flags or TXN_FLAG_HAS_TIMEOUT;
  if AutocommitMode <> 0 then
    Flags := Flags or TXN_FLAG_HAS_AUTOCOMMIT;
  Payload := BuildTxnBeginPayload(Flags, ConflictAction, AutocommitMode, IsolationLevel, AccessMode,
    Ord(Deferrable), Ord(WaitMode), TimeoutMs, ReadCommittedMode);
  SendMessage(MSG_TXN_BEGIN, Payload, 0, False);
  DrainUntilReady;
  FExplicitTransaction := True;
end;

procedure TScratchBirdClient.Commit(Flags: Byte = 0);
var
  Payload: TBytes;
begin
  if not FConnected then
    Exit;
  EnsureConnected;
  Payload := BuildTxnCommitPayload(Flags);
  SendMessage(MSG_TXN_COMMIT, Payload, 0, False);
  DrainUntilReady;
  FExplicitTransaction := False;
end;

procedure TScratchBirdClient.Rollback(Flags: Byte = 0);
var
  Payload: TBytes;
begin
  if not FConnected then
    Exit;
  EnsureConnected;
  Payload := BuildTxnRollbackPayload(Flags);
  SendMessage(MSG_TXN_ROLLBACK, Payload, 0, False);
  DrainUntilReady;
  FExplicitTransaction := False;
end;

function TScratchBirdClient.SupportsPreparedTransactions: Boolean;
begin
  Result := True;
end;

procedure TScratchBirdClient.PrepareTransaction(const GlobalTransactionId: string);
begin
  // Prepared / limbo state is explicit engine truth, not reconnect folklore.
  ExecSQL(BuildPreparedTransactionSql('PREPARE TRANSACTION', GlobalTransactionId));
end;

procedure TScratchBirdClient.CommitPrepared(const GlobalTransactionId: string);
begin
  ExecSQL(BuildPreparedTransactionSql('COMMIT PREPARED', GlobalTransactionId));
end;

procedure TScratchBirdClient.RollbackPrepared(const GlobalTransactionId: string);
begin
  ExecSQL(BuildPreparedTransactionSql('ROLLBACK PREPARED', GlobalTransactionId));
end;

function TScratchBirdClient.SupportsDormantReattach: Boolean;
begin
  Result := False;
end;

procedure TScratchBirdClient.DetachToDormant;
begin
  raise EScratchbirdNotSupported.CreateWithInfo(
    'Dormant detach requires an explicit public token flow and is not yet exposed in this lane',
    '0A000', '', ''
  );
end;

procedure TScratchBirdClient.ReattachDormant(const DormantId: string; const AuthToken: string = '');
begin
  raise EScratchbirdNotSupported.CreateWithInfo(
    'Dormant reattach requires an explicit engine-issued token and is not yet exposed in this lane',
    '0A000', '', ''
  );
end;

procedure TScratchBirdClient.Savepoint(const Name: string);
var
  SavepointName: string;
begin
  SavepointName := NormalizeSavepointName(Name);
  EnsureTransactionActive('savepoint');
  EnsureConnected;
  SendMessage(MSG_TXN_SAVEPOINT, BuildTxnSavepointPayload(SavepointName), 0, False);
  DrainUntilReady;
end;

procedure TScratchBirdClient.ReleaseSavepoint(const Name: string);
var
  SavepointName: string;
begin
  SavepointName := NormalizeSavepointName(Name);
  EnsureTransactionActive('release_savepoint');
  EnsureConnected;
  SendMessage(MSG_TXN_RELEASE, BuildTxnReleasePayload(SavepointName), 0, False);
  DrainUntilReady;
end;

procedure TScratchBirdClient.RollbackToSavepoint(const Name: string);
var
  SavepointName: string;
begin
  SavepointName := NormalizeSavepointName(Name);
  EnsureTransactionActive('rollback_to_savepoint');
  EnsureConnected;
  SendMessage(MSG_TXN_ROLLBACK_TO, BuildTxnRollbackToPayload(SavepointName), 0, False);
  DrainUntilReady;
end;

procedure TScratchBirdClient.SetOption(const Name, Value: string);
begin
  SendMessage(MSG_SET_OPTION, BuildSetOptionPayload(Name, Value), 0, False);
  DrainUntilReady;
end;

procedure TScratchBirdClient.Ping;
var
  Msg: TScratchBirdMessage;
  Status: Byte;
  TxnId, Visibility: UInt64;
begin
  SendMessage(MSG_PING, nil, 0, False);
  while True do
  begin
    Msg := ReceiveMessage;
    if HandleAsyncMessage(Msg) then
      Continue;
    case Msg.MsgType of
      MSG_PONG:
        Exit;
        MSG_READY:
          begin
            ParseReady(Msg.Payload, Status, TxnId, Visibility);
            ApplyRuntimeReadyState(Status, TxnId);
            FPortalResumePending := False;
            Exit;
          end;
      MSG_ERROR:
        raise BuildQueryError(Msg.Payload);
    end;
  end;
end;

procedure TScratchBirdClient.Terminate;
begin
  SendMessage(MSG_TERMINATE, nil, 0, False);
  Disconnect;
end;

procedure TScratchBirdClient.Subscribe(SubscribeType: Byte; const Channel, FilterExpr: string);
begin
  SendMessage(MSG_SUBSCRIBE, BuildSubscribePayload(SubscribeType, Channel, FilterExpr), 0, False);
  DrainUntilReady;
end;

procedure TScratchBirdClient.Unsubscribe(const Channel: string);
begin
  SendMessage(MSG_UNSUBSCRIBE, BuildUnsubscribePayload(Channel), 0, False);
  DrainUntilReady;
end;

procedure TScratchBirdClient.Listen(const Channel: string; const FilterExpr: string);
begin
  Subscribe(SUB_TYPE_CHANNEL, Channel, FilterExpr);
end;

procedure TScratchBirdClient.Unlisten(const Channel: string);
begin
  Unsubscribe(Channel);
end;

procedure TScratchBirdClient.UnlistenAll;
begin
  ExecSQL('UNLISTEN *');
end;

procedure TScratchBirdClient.NotifyChannel(const Channel: string);
begin
  NotifyChannel(Channel, '');
end;

procedure TScratchBirdClient.NotifyChannel(const Channel, Payload: string);
var
  Sql: string;
begin
  Sql := 'NOTIFY ' + QuoteIdentifier(Channel);
  if Payload <> '' then
    Sql := Sql + ', ''' + StringReplace(Payload, '''', '''''', [rfReplaceAll]) + '''';
  ExecSQL(Sql);
end;

function TScratchBirdClient.AddNotificationListener(const Handler: TNotificationHandler): UInt64;
var
  Index: Integer;
begin
  if not Assigned(Handler) then
    raise EScratchbirdConnectionError.CreateWithInfo('listener is required', '22004', '', '');
  Result := FNextNotificationListenerId;
  Inc(FNextNotificationListenerId);
  Index := Length(FNotificationListeners);
  SetLength(FNotificationListeners, Index + 1);
  FNotificationListeners[Index].Id := Result;
  FNotificationListeners[Index].Handler := Handler;
end;

function TScratchBirdClient.RemoveNotificationListener(ListenerId: UInt64): Boolean;
var
  I, LastIndex: Integer;
begin
  Result := False;
  for I := 0 to High(FNotificationListeners) do
  begin
    if FNotificationListeners[I].Id = ListenerId then
    begin
      LastIndex := High(FNotificationListeners);
      if I <> LastIndex then
        FNotificationListeners[I] := FNotificationListeners[LastIndex];
      SetLength(FNotificationListeners, LastIndex);
      Exit(True);
    end;
  end;
end;

function TScratchBirdClient.GetNotification(out Notice: TNotification): Boolean;
var
  I: Integer;
begin
  Result := Length(FNotificationQueue) > 0;
  if not Result then
    Exit(False);
  Notice := FNotificationQueue[0];
  for I := 1 to High(FNotificationQueue) do
    FNotificationQueue[I - 1] := FNotificationQueue[I];
  SetLength(FNotificationQueue, Length(FNotificationQueue) - 1);
end;

function TScratchBirdClient.GetNotifications: TArray<TNotification>;
begin
  Result := Copy(FNotificationQueue);
  SetLength(FNotificationQueue, 0);
end;

procedure TScratchBirdClient.ClearNotifications;
begin
  SetLength(FNotificationQueue, 0);
end;

function TScratchBirdClient.NotificationCount: Integer;
begin
  Result := Length(FNotificationQueue);
end;

function TScratchBirdClient.ExecuteSblr(SblrHash: UInt64; const Bytecode: TBytes; const Params: array of TScratchBirdParamInput): TScratchBirdResultStream;
var
  ParamValues: TArray<TParamValue>;
  Param: TParamValue;
  Oid: Cardinal;
  I: Integer;
  Payload: TBytes;
  Span: TSpanContext;
begin
  Span := BeginOperation('sblr_execute', '');
  try
    SetLength(ParamValues, Length(Params));
    for I := 0 to High(Params) do
    begin
      EncodeParam(Params[I].Value, Params[I].Obj, Param, Oid);
      ParamValues[I] := Param;
    end;
    FHasLastPlan := False;
    FHasLastSblr := False;
    FPortalResumePending := False;
    Payload := BuildSblrExecutePayload(SblrHash, Bytecode, ParamValues);
    FLastQuerySequence := SendMessage(MSG_SBLR_EXECUTE, Payload, 0, False);
    SendMessage(MSG_SYNC, nil, 0, False);
    Result := TScratchBirdResultStream.Create(Self);
    EndOperation(Span, True);
  except
    on E: Exception do
    begin
      EndOperation(Span, False);
      raise;
    end;
  end;
end;

procedure TScratchBirdClient.StreamControl(ControlType: Byte; WindowSize, TimeoutMs: Cardinal);
begin
  SendMessage(MSG_STREAM_CONTROL, BuildStreamControlPayload(ControlType, WindowSize, TimeoutMs), 0, False);
end;

procedure TScratchBirdClient.AttachCreate(const EmulationMode, DbName: string);
begin
  SendMessage(MSG_ATTACH_CREATE, BuildAttachCreatePayload(EmulationMode, DbName), 0, False);
  DrainUntilReady;
end;

procedure TScratchBirdClient.AttachDetach;
begin
  SendMessage(MSG_ATTACH_DETACH, nil, 0, False);
  DrainUntilReady;
end;

function TScratchBirdClient.AttachList: TScratchBirdResultStream;
begin
  SendMessage(MSG_ATTACH_LIST, nil, 0, False);
  SendMessage(MSG_SYNC, nil, 0, False);
  Result := TScratchBirdResultStream.Create(Self);
end;

procedure TScratchBirdClient.ExecSQL(const Sql: string);
begin
  ExecSQLParams(Sql, []);
end;

procedure TScratchBirdClient.ExecSQLParams(const Sql: string; const Params: array of TScratchBirdParamInput);
var
  Span: TSpanContext;
  SqlText: string;
  OrderedParams: TArray<TScratchBirdParamInput>;
begin
  SqlText := NormalizeSqlText(Sql);
  EnsureConnected;
  Span := BeginOperation('exec', SqlText);
  try
    if Length(Params) = 0 then
    begin
      SendSimpleQuery(SqlText, 0);
    end
    else
    begin
      SqlText := NormalizePositionalSql(SqlText, Params, OrderedParams);
      SendExtendedQuery(SqlText, OrderedParams, 0);
    end;
    DrainUntilReady;
    EndOperation(Span, True);
  except
    on E: Exception do
    begin
      EndOperation(Span, False);
      raise;
    end;
  end;
end;

function TScratchBirdClient.ExecuteQuery(const Sql: string): TScratchBirdResultStream;
begin
  Result := ExecuteQueryParams(Sql, []);
end;

function TScratchBirdClient.ExecuteQueryParams(const Sql: string; const Params: array of TScratchBirdParamInput): TScratchBirdResultStream;
var
  Span: TSpanContext;
  SqlText: string;
  OrderedParams: TArray<TScratchBirdParamInput>;
begin
  SqlText := NormalizeSqlText(Sql);
  EnsureConnected;
  Span := BeginOperation('query', SqlText);
  try
    if Length(Params) = 0 then
      SendSimpleQuery(SqlText, Cardinal(FConfig.FetchSize))
    else
    begin
      SqlText := NormalizePositionalSql(SqlText, Params, OrderedParams);
      SendExtendedQuery(SqlText, OrderedParams, Cardinal(FConfig.FetchSize));
    end;
    Result := TScratchBirdResultStream.Create(Self);
    EndOperation(Span, True);
  except
    on E: Exception do
    begin
      EndOperation(Span, False);
      raise;
    end;
  end;
end;

function TScratchBirdClient.ExecuteBatch(const Statements: array of string): TScratchBirdBatchResults;
var
  I: Integer;
  Stream: TScratchBirdResultStream;
begin
  SetLength(Result, Length(Statements));
  for I := 0 to High(Statements) do
  begin
    Stream := ExecuteQuery(Statements[I]);
    try
      while Stream.ReadRow <> nil do
      begin
      end;
      Result[I].RowsAffected := Stream.RowsAffected;
      Result[I].CommandTag := Stream.CommandTag;
      Result[I].LastInsertId := Stream.LastInsertId;
      Result[I].HasLastInsertId := Stream.HasLastInsertId;
    finally
      Stream.Free;
    end;
  end;
end;

function TScratchBirdClient.QueryMulti(const Statements: array of string): TScratchBirdRowsets;
var
  I, RowIndex: Integer;
  Stream: TScratchBirdResultStream;
  Row: TArray<Variant>;
begin
  SetLength(Result, Length(Statements));
  for I := 0 to High(Statements) do
  begin
    Stream := ExecuteQuery(Statements[I]);
    try
      SetLength(Result[I].Rows, 0);
      while True do
      begin
        Row := Stream.ReadRow;
        if Row = nil then
          Break;
        RowIndex := Length(Result[I].Rows);
        SetLength(Result[I].Rows, RowIndex + 1);
        Result[I].Rows[RowIndex] := Row;
      end;
      Result[I].Columns := Copy(Stream.Columns);
      Result[I].RowsAffected := Stream.RowsAffected;
      Result[I].CommandTag := Stream.CommandTag;
      Result[I].LastInsertId := Stream.LastInsertId;
      Result[I].HasLastInsertId := Stream.HasLastInsertId;
    finally
      Stream.Free;
    end;
  end;
end;

function TScratchBirdClient.QueryMetadata(const CollectionName: string): TScratchBirdResultStream;
begin
  Result := ExecuteQuery(ResolveMetadataCollectionQuery(CollectionName));
end;

function TScratchBirdClient.GetSchema(const CollectionName: string): TScratchBirdResultStream;
begin
  Result := QueryMetadata(CollectionName);
end;

function TScratchBirdClient.QueryMetadataRows(const CollectionName: string): TMetadataRows;
var
  EmptyRestrictions: TMetadataRow;
begin
  SetLength(EmptyRestrictions, 0);
  Result := QueryMetadataRows(CollectionName, EmptyRestrictions);
end;

function TScratchBirdClient.QueryMetadataRows(const CollectionName: string; const Restrictions: TMetadataRow): TMetadataRows;
var
  Stream: TScratchBirdResultStream;
  RawRow: TArray<Variant>;
  Columns: TArray<TColumnInfo>;
  RowIndex, I: Integer;
begin
  Stream := QueryMetadata(CollectionName);
  try
    Result := nil;
    SetLength(Result, 0);
    while True do
    begin
      RawRow := Stream.ReadRow;
      if RawRow = nil then
        Break;

      Columns := Stream.Columns;
      RowIndex := Length(Result);
      SetLength(Result, RowIndex + 1);
      SetLength(Result[RowIndex], Length(RawRow));
      for I := 0 to High(RawRow) do
      begin
        if I < Length(Columns) then
          Result[RowIndex][I].Name := Columns[I].Name
        else
          Result[RowIndex][I].Name := 'column_' + IntToStr(I + 1);
        Result[RowIndex][I].Value := RawRow[I];
      end;
    end;
  finally
    Stream.Free;
  end;

  Result := FilterMetadataRowsByRestrictions(Result, Restrictions, CollectionName);
end;

function TScratchBirdClient.GetSchemaRows(const CollectionName: string): TMetadataRows;
begin
  Result := QueryMetadataRows(CollectionName);
end;

function TScratchBirdClient.GetSchemaRows(const CollectionName: string; const Restrictions: TMetadataRow): TMetadataRows;
begin
  Result := QueryMetadataRows(CollectionName, Restrictions);
end;

function TScratchBirdClient.GetCatalogs: TScratchBirdResultStream;
begin
  Result := QueryMetadata('catalogs');
end;

function TScratchBirdClient.GetSchemas: TScratchBirdResultStream;
begin
  Result := QueryMetadata('schemas');
end;

function TScratchBirdClient.GetTables: TScratchBirdResultStream;
begin
  Result := QueryMetadata('tables');
end;

function TScratchBirdClient.GetColumns: TScratchBirdResultStream;
begin
  Result := QueryMetadata('columns');
end;

function TScratchBirdClient.GetIndexes: TScratchBirdResultStream;
begin
  Result := QueryMetadata('indexes');
end;

function TScratchBirdClient.GetIndexColumns: TScratchBirdResultStream;
begin
  Result := QueryMetadata('index_columns');
end;

function TScratchBirdClient.GetConstraints: TScratchBirdResultStream;
begin
  Result := QueryMetadata('constraints');
end;

function TScratchBirdClient.GetProcedures: TScratchBirdResultStream;
begin
  Result := QueryMetadata('procedures');
end;

function TScratchBirdClient.GetFunctions: TScratchBirdResultStream;
begin
  Result := QueryMetadata('functions');
end;

function TScratchBirdClient.GetRoutines: TScratchBirdResultStream;
begin
  Result := QueryMetadata('routines');
end;

function TScratchBirdClient.GetPrimaryKeys: TScratchBirdResultStream;
begin
  Result := QueryMetadata('primary_keys');
end;

function TScratchBirdClient.GetForeignKeys: TScratchBirdResultStream;
begin
  Result := QueryMetadata('foreign_keys');
end;

function TScratchBirdClient.GetTablePrivileges: TScratchBirdResultStream;
begin
  Result := QueryMetadata('table_privileges');
end;

function TScratchBirdClient.GetColumnPrivileges: TScratchBirdResultStream;
begin
  Result := QueryMetadata('column_privileges');
end;

function TScratchBirdClient.GetTypeInfo: TScratchBirdResultStream;
begin
  Result := QueryMetadata('type_info');
end;

function TScratchBirdClient.GetTelemetrySummaryJson: string;
begin
  if Assigned(FTelemetry) then
    Result := FTelemetry.ExportTelemetrySummaryJson
  else
    Result := '{"total_invocations":0,"total_successes":0,"total_failures":0,"total_duration_ms":0,"operations":[],"histogram":{"ms_0_10":0,"ms_10_100":0,"ms_100_1000":0,"ms_1000_10000":0,"ms_over_10000":0}}';
end;

procedure TScratchBirdClient.ResetTelemetry;
begin
  if Assigned(FTelemetry) then
    FTelemetry.Reset;
end;

function TScratchBirdClient.GetSlowOperationsJson: string;
begin
  if Assigned(FTelemetry) then
    Result := FTelemetry.ExportSlowQueriesJson
  else
    Result := '[]';
end;

function TScratchBirdClient.ExportTelemetryPrometheus: string;
begin
  if Assigned(FTelemetry) then
    Result := FTelemetry.ExportPrometheusMetrics
  else
    Result := '';
end;

function TScratchBirdClient.GetCircuitBreakerSummaryJson: string;
var
  Stats: TCircuitBreakerStats;
  StateText: string;
begin
  if not Assigned(FCircuitBreaker) then
    Exit('{"state":"open","failure_count":0,"success_count":0}');
  Stats := FCircuitBreaker.GetStats;
  case Stats.State of
    csClosed: StateText := 'closed';
    csHalfOpen: StateText := 'half_open';
  else
    StateText := 'open';
  end;
  Result := '{' +
    '"state":"' + StateText + '",' +
    '"failure_count":' + IntToStr(Stats.FailureCount) + ',' +
    '"success_count":' + IntToStr(Stats.SuccessCount) +
  '}';
end;

function TScratchBirdClient.GetKeepaliveSummaryJson: string;
var
  IdleMs: Cardinal;
  MonitoredCount: Integer;
begin
  IdleMs := 0;
  if Assigned(FKeepaliveTracker) then
    IdleMs := FKeepaliveTracker.GetIdleDurationMs;
  if FConnected then
    MonitoredCount := 1
  else
    MonitoredCount := 0;
  Result := '{' +
    '"monitored_count":' + IntToStr(MonitoredCount) + ',' +
    '"idle_duration_ms":' + IntToStr(IdleMs) +
  '}';
end;

function TScratchBirdClient.GetLeakSummaryJson: string;
var
  ActiveCount: Integer;
begin
  ActiveCount := 0;
  if Assigned(FLeakDetector) then
    ActiveCount := FLeakDetector.GetActiveCount;
  Result := '{' +
    '"active_checkouts":' + IntToStr(ActiveCount) + ',' +
    '"potential_leaks":0' +
  '}';
end;

function TScratchBirdClient.GetDiagnosticsJson: string;
var
  TelemetrySummary: string;
  AttachmentZeroed: Boolean;
  I: Integer;
  ParameterCount: Integer;
begin
  TelemetrySummary := GetTelemetrySummaryJson;
  AttachmentZeroed := Length(FAttachmentId) = 16;
  if AttachmentZeroed then
    for I := 0 to High(FAttachmentId) do
      if FAttachmentId[I] <> 0 then
      begin
        AttachmentZeroed := False;
        Break;
      end;
  if Assigned(FParameters) then
    ParameterCount := FParameters.Count
  else
    ParameterCount := 0;
  Result := '{' +
    '"captured_unix_ms":' + IntToStr(DateTimeToUnix(Now, False) * 1000) + ',' +
    '"connected":' + LowerCase(BoolToStr(FConnected, True)) + ',' +
    '"transaction_active":' + LowerCase(BoolToStr(FTransactionActive, True)) + ',' +
    '"front_door_mode":"' + EscapeJson(FConfig.FrontDoorMode) + '",' +
    '"protocol":"' + EscapeJson(FConfig.Protocol) + '",' +
    '"host":"' + EscapeJson(FConfig.Host) + '",' +
    '"port":' + IntToStr(FConfig.Port) + ',' +
    '"database":"' + EscapeJson(FConfig.Database) + '",' +
    '"attachment_zeroed":' + LowerCase(BoolToStr(AttachmentZeroed, True)) + ',' +
    '"parameter_count":' + IntToStr(ParameterCount) + ',' +
    '"has_last_plan":' + LowerCase(BoolToStr(FHasLastPlan, True)) + ',' +
    '"has_last_sblr":' + LowerCase(BoolToStr(FHasLastSblr, True)) + ',' +
    '"next_sequence":' + IntToStr(FSequence) + ',' +
    '"last_query_sequence":' + IntToStr(FLastQuerySequence) + ',' +
    '"notification_queue_depth":' + IntToStr(NotificationCount) + ',' +
    '"circuit":' + GetCircuitBreakerSummaryJson + ',' +
    '"keepalive":' + GetKeepaliveSummaryJson + ',' +
    '"leak_detection":' + GetLeakSummaryJson + ',' +
    '"telemetry":' + TelemetrySummary +
  '}';
end;

procedure TScratchBirdClient.Cancel;
begin
  FPortalResumePending := False;
  SendMessage(MSG_CANCEL, BuildCancelPayload(0, FLastQuerySequence), MSG_FLAG_URGENT, False);
end;

function TScratchBirdClient.GetLastPlan(out Plan: TQueryPlan): Boolean;
begin
  Result := FHasLastPlan;
  if Result then
    Plan := FLastPlan;
end;

function TScratchBirdClient.GetLastSblr(out Compiled: TSblrCompiled): Boolean;
begin
  Result := FHasLastSblr;
  if Result then
    Compiled := FLastSblr;
end;

procedure TScratchBirdClient.SendBytes(const Data: TBytes);
begin
  if Length(Data) = 0 then
    Exit;
  FTransport.Write(Data);
end;

function TScratchBirdClient.ReadExact(Length: Integer): TBytes;
begin
  Result := FTransport.ReadExact(Length);
end;

function TScratchBirdClient.ReceiveMessage: TScratchBirdMessage;
var
  Header: TBytes;
  MsgType: TScratchBirdMessageType;
  Flags: Byte;
  PayloadLen: Integer;
  Sequence: Cardinal;
  AttachmentId: TBytes;
  TxnId: UInt64;
begin
  Header := ReadExact(HEADER_SIZE);
  if not DecodeHeader(Header, MsgType, Flags, PayloadLen, Sequence, AttachmentId, TxnId) then
    raise EScratchbirdConnectionError.CreateWithInfo('Invalid header', '08006', '', '');
  Result.MsgType := MsgType;
  Result.Flags := Flags;
  Result.Sequence := Sequence;
  Result.AttachmentId := AttachmentId;
  Result.TxnId := TxnId;
  if PayloadLen > 0 then
    Result.Payload := ReadExact(PayloadLen)
  else
    Result.Payload := nil;
end;

procedure TScratchBirdClient.AppendLengthPrefixedString(var Buffer: TBytes; const Value: string);
var
  Bytes: TBytes;
begin
  Bytes := TEncoding.UTF8.GetBytes(Value);
  AppendUInt32LE(Buffer, Cardinal(Length(Bytes)));
  AppendBytes(Buffer, Bytes);
end;

procedure TScratchBirdClient.SendManagerFrame(MsgType: Byte; const Payload: TBytes);
var
  Frame: TBytes;
  Start: Integer;
begin
  SetLength(Frame, 0);
  AppendUInt32LE(Frame, MANAGER_PROTOCOL_MAGIC);
  AppendUInt16LE(Frame, MANAGER_PROTOCOL_VERSION);
  Start := Length(Frame);
  SetLength(Frame, Start + 2);
  Frame[Start] := MsgType;
  Frame[Start + 1] := 0;
  AppendUInt32LE(Frame, Cardinal(Length(Payload)));
  AppendBytes(Frame, Payload);
  SendBytes(Frame);
end;

procedure TScratchBirdClient.ReceiveManagerFrame(out MsgType: Byte; out Payload: TBytes);
var
  Header: TBytes;
  Magic: Cardinal;
  Version: Word;
  PayloadLen: Cardinal;
begin
  Header := ReadExact(MANAGER_HEADER_SIZE);
  Magic := ReadUInt32LEValue(Header, 0);
  if Magic <> MANAGER_PROTOCOL_MAGIC then
    raise EScratchbirdConnectionError.CreateWithInfo('Manager frame magic mismatch', '08P01', '', '');
  Version := ReadUInt16LEValue(Header, 4);
  if Version <> MANAGER_PROTOCOL_VERSION then
    raise EScratchbirdConnectionError.CreateWithInfo('Manager frame version mismatch', '08P01', '', '');
  MsgType := Header[6];
  PayloadLen := ReadUInt32LEValue(Header, 8);
  if PayloadLen > MANAGER_MAX_PAYLOAD_SIZE then
    raise EScratchbirdConnectionError.CreateWithInfo('Manager payload too large', '08P01', '', '');
  if PayloadLen > 0 then
    Payload := ReadExact(PayloadLen)
  else
    SetLength(Payload, 0);
end;

function TScratchBirdClient.BuildStartupParams: TStringList;
begin
  Result := TStringList.Create;
  Result.Values['database'] := FConfig.Database;
  Result.Values['user'] := FConfig.UserName;
  Result.Values['client_flags'] := IntToStr(FConfig.ConnectClientFlags);
  if FConfig.Role <> '' then
    Result.Values['role'] := FConfig.Role;
  if FConfig.ApplicationName <> '' then
    Result.Values['application_name'] := FConfig.ApplicationName;
  if (FConfig.AuthMethodId <> '') and
     (Copy(FConfig.AuthMethodId, 1, Length('scratchbird.auth.')) <> 'scratchbird.auth.') then
    raise EScratchbirdAuthError.CreateWithInfo('invalid auth_method_id namespace', '28000', '', '');
  ApplyAuthPluginSelection(FConfig, Result);
end;

function TScratchBirdClient.ProbeManagerAuthSurface: TScratchBirdAuthProbeResult;
var
  HelloPayload: TBytes;
  MsgType: Byte;
  Payload: TBytes;
  Surface: TScratchBirdAuthMethodSurface;
begin
  FillChar(Result, SizeOf(Result), 0);
  SetLength(HelloPayload, 0);
  AppendUInt16LE(HelloPayload, MCP_PROTOCOL_VERSION);
  AppendUInt16LE(HelloPayload, Word(FConfig.ManagerClientFlags and $FFFF));
  SendManagerFrame(MCP_MSG_HELLO, HelloPayload);
  ReceiveManagerFrame(MsgType, Payload);
  if MsgType <> MCP_MSG_STATUS_RESPONSE then
    raise EScratchbirdConnectionError.CreateWithInfo('Expected MCP hello status response', '08P01', '', '');
  if not TryDescribeAuthMethod(AUTH_TOKEN, FConfig.AuthMethodId, Surface) then
    raise EScratchbirdAuthError.CreateWithInfo('TOKEN auth surface unavailable', '28000', '', '');
  Result.Reachable := True;
  Result.FrontDoorMode := 'manager_proxy';
  Result.RequiredMethodCode := AUTH_TOKEN;
  Result.RequiredMethodName := Surface.MethodName;
  Result.RequiredPluginMethodId := Surface.PluginMethodId;
  Result.RequiredMethodBrokerRequired := Surface.BrokerRequired;
  Result.AdditionalContinuationPossible := AdditionalContinuationPossible(AUTH_TOKEN);
  SetLength(Result.AdmittedMethods, 1);
  Result.AdmittedMethods[0] := Surface;
end;

function TScratchBirdClient.ProbeDirectAuthSurface: TScratchBirdAuthProbeResult;
var
  Params: TStringList;
  Features: UInt64;
  Startup: TBytes;
  Msg: TScratchBirdMessage;
  Method, Stage: Byte;
  Data: TBytes;
  Surface: TScratchBirdAuthMethodSurface;
begin
  FillChar(Result, SizeOf(Result), 0);
  Params := BuildStartupParams;
  try
    Features := 0;
    if SameText(FConfig.Compression, 'zstd') then
      Features := Features or FEATURE_COMPRESSION;
    if FConfig.BinaryTransfer then
      Features := Features or FEATURE_STREAMING;
    Startup := BuildStartupPayload(Features, Params);
    SendMessage(MSG_STARTUP, Startup, 0, True);
  finally
    Params.Free;
  end;

  while True do
  begin
    Msg := ReceiveMessage;
    case Msg.MsgType of
      MSG_NEGOTIATE_VERSION, MSG_PARAMETER_STATUS, MSG_NOTICE:
        Continue;
      MSG_AUTH_REQUEST:
        begin
          ParseAuthRequest(Msg.Payload, Method, Data);
          if not TryDescribeAuthMethod(Method, FConfig.AuthMethodId, Surface) then
            raise EScratchbirdAuthError.CreateWithInfo('Unsupported auth method during probe', '28000', '', '');
          Result.Reachable := True;
          Result.FrontDoorMode := 'direct';
          Result.RequiredMethodCode := Method;
          Result.RequiredMethodName := Surface.MethodName;
          Result.RequiredPluginMethodId := Surface.PluginMethodId;
          Result.RequiredMethodBrokerRequired := Surface.BrokerRequired;
          Result.AdditionalContinuationPossible := AdditionalContinuationPossible(Method);
          SetLength(Result.AdmittedMethods, 1);
          Result.AdmittedMethods[0] := Surface;
          Exit;
        end;
      MSG_AUTH_CONTINUE:
        begin
          ParseAuthContinue(Msg.Payload, Method, Stage, Data);
          if not TryDescribeAuthMethod(Method, FConfig.AuthMethodId, Surface) then
            raise EScratchbirdAuthError.CreateWithInfo('Unsupported auth continue during probe', '28000', '', '');
          Result.Reachable := True;
          Result.FrontDoorMode := 'direct';
          Result.RequiredMethodCode := Method;
          Result.RequiredMethodName := Surface.MethodName;
          Result.RequiredPluginMethodId := Surface.PluginMethodId;
          Result.RequiredMethodBrokerRequired := Surface.BrokerRequired;
          Result.AdditionalContinuationPossible := True;
          SetLength(Result.AdmittedMethods, 1);
          Result.AdmittedMethods[0] := Surface;
          Exit;
        end;
      MSG_AUTH_OK:
        begin
          if not TryDescribeAuthMethod(AUTH_OK, FConfig.AuthMethodId, Surface) then
            raise EScratchbirdAuthError.CreateWithInfo('Unable to resolve auth-ok surface', '28000', '', '');
          Result.Reachable := True;
          Result.FrontDoorMode := 'direct';
          Result.RequiredMethodCode := AUTH_OK;
          Result.RequiredMethodName := Surface.MethodName;
          Result.RequiredPluginMethodId := Surface.PluginMethodId;
          Result.RequiredMethodBrokerRequired := False;
          Result.AdditionalContinuationPossible := False;
          SetLength(Result.AdmittedMethods, 1);
          Result.AdmittedMethods[0] := Surface;
          Exit;
        end;
      MSG_ERROR:
        raise BuildQueryError(Msg.Payload);
    end;
  end;
end;

procedure TScratchBirdClient.PerformManagerConnect;
var
  Token: string;
  ManagerUser, ManagerDatabase, ManagerProfile, ManagerIntent: string;
  HelloPayload, AuthStart, AuthContinue, DbConnect, Nonce, TokenBytes: TBytes;
  MsgType: Byte;
  Payload: TBytes;
  Start: Integer;
  I: Integer;
  ErrText: string;
  ErrOffset, ErrLen: Cardinal;
begin
  Token := Trim(FConfig.ManagerAuthToken);
  if Token = '' then
    raise EScratchbirdConnectionError.CreateWithInfo('manager_proxy mode requires manager_auth_token', '08001', '', '');

  ManagerUser := Trim(FConfig.ManagerUsername);
  if ManagerUser = '' then
  begin
    if Trim(FConfig.UserName) <> '' then
      ManagerUser := FConfig.UserName
    else
      ManagerUser := 'admin';
  end;
  ManagerDatabase := Trim(FConfig.ManagerDatabase);
  if ManagerDatabase = '' then
    ManagerDatabase := FConfig.Database;
  ManagerProfile := Trim(FConfig.ManagerConnectionProfile);
  if ManagerProfile = '' then
    ManagerProfile := 'SBsql';
  ManagerIntent := Trim(FConfig.ManagerClientIntent);
  if ManagerIntent = '' then
    ManagerIntent := 'SBsql';

  SetLength(HelloPayload, 0);
  AppendUInt16LE(HelloPayload, MCP_PROTOCOL_VERSION);
  AppendUInt16LE(HelloPayload, Word(FConfig.ManagerClientFlags and $FFFF));
  SendManagerFrame(MCP_MSG_HELLO, HelloPayload);
  ReceiveManagerFrame(MsgType, Payload);
  if MsgType <> MCP_MSG_STATUS_RESPONSE then
    raise EScratchbirdConnectionError.CreateWithInfo('Expected MCP hello status response', '08P01', '', '');

  SetLength(AuthStart, 0);
  AppendLengthPrefixedString(AuthStart, ManagerUser);
  Start := Length(AuthStart);
  SetLength(AuthStart, Start + 1);
  AuthStart[Start] := MCP_AUTH_METHOD_TOKEN;
  if FConfig.ManagerAuthFastPath then
  begin
    TokenBytes := TEncoding.UTF8.GetBytes(Token);
    AppendUInt32LE(AuthStart, Cardinal(Length(TokenBytes)));
    AppendBytes(AuthStart, TokenBytes);
  end
  else
    AppendUInt32LE(AuthStart, 0);

  SendManagerFrame(MCP_MSG_AUTH_START, AuthStart);
  ReceiveManagerFrame(MsgType, Payload);
  if MsgType = MCP_MSG_AUTH_CHALLENGE then
  begin
    TokenBytes := TEncoding.UTF8.GetBytes(Token);
    SetLength(AuthContinue, 0);
    AppendUInt32LE(AuthContinue, Cardinal(Length(TokenBytes)));
    AppendBytes(AuthContinue, TokenBytes);
    SendManagerFrame(MCP_MSG_AUTH_CONTINUE, AuthContinue);
    ReceiveManagerFrame(MsgType, Payload);
  end;
  if MsgType <> MCP_MSG_AUTH_RESPONSE then
    raise EScratchbirdConnectionError.CreateWithInfo('Expected MCP auth response', '08P01', '', '');
  if Length(Payload) < (1 + 4 + 256) then
    raise EScratchbirdConnectionError.CreateWithInfo('Truncated MCP auth response', '08P01', '', '');
  if Payload[0] <> 0 then
  begin
    ErrText := StringReplace(TEncoding.UTF8.GetString(Copy(Payload, 5, 256)), #0, '', [rfReplaceAll]);
    if Trim(ErrText) = '' then
      ErrText := 'MCP authentication failed';
    raise EScratchbirdAuthError.CreateWithInfo(ErrText, '28000', '', '');
  end;

  SetLength(DbConnect, 0);
  AppendBytes(DbConnect, TEncoding.ASCII.GetBytes('MCP1'));
  AppendLengthPrefixedString(DbConnect, ManagerDatabase);
  AppendLengthPrefixedString(DbConnect, ManagerProfile);
  AppendLengthPrefixedString(DbConnect, ManagerIntent);
  SetLength(Nonce, 16);
  Randomize;
  for I := 0 to High(Nonce) do
    Nonce[I] := Byte(Random(256));
  AppendUInt16LE(DbConnect, Word(Length(Nonce)));
  AppendBytes(DbConnect, Nonce);
  SendManagerFrame(MCP_MSG_DB_CONNECT, DbConnect);
  ReceiveManagerFrame(MsgType, Payload);
  if MsgType <> MCP_MSG_CONNECT_RESPONSE then
    raise EScratchbirdConnectionError.CreateWithInfo('Expected MCP connect response', '08P01', '', '');
  if Length(Payload) < (1 + 2 + 2 + 16 + 64 + 32) then
    raise EScratchbirdConnectionError.CreateWithInfo('Truncated MCP connect response', '08P01', '', '');
  if Payload[0] <> 0 then
  begin
    ErrText := 'MCP database connect failed';
    ErrOffset := 1 + 2 + 2 + 16 + 64 + 32;
    if Length(Payload) >= Integer(ErrOffset + 4) then
    begin
      ErrLen := ReadUInt32LEValue(Payload, ErrOffset);
      if Length(Payload) >= Integer(ErrOffset + 4 + ErrLen) then
        ErrText := TEncoding.UTF8.GetString(Copy(Payload, ErrOffset + 4, ErrLen));
    end;
    raise EScratchbirdAuthError.CreateWithInfo(ErrText, '28000', '', '');
  end;
  FResolvedAuthContext.FrontDoorMode := 'manager_proxy';
  FResolvedAuthContext.ManagerAuthenticated := True;
end;

function TScratchBirdClient.SendMessage(MsgType: TScratchBirdMessageType; const Payload: TBytes; Flags: Byte; ForceZero: Boolean): Cardinal;
var
  AttachmentId: TBytes;
  TxnId: UInt64;
begin
  Result := FSequence;
  Inc(FSequence);
  if ForceZero then
  begin
    SetLength(AttachmentId, 16);
    FillChar(AttachmentId[0], 16, 0);
    TxnId := 0;
  end
  else
  begin
    AttachmentId := FAttachmentId;
    TxnId := FTxnId;
  end;
  SendBytes(EncodeMessage(MsgType, Payload, Flags, Result, AttachmentId, TxnId));
end;

procedure TScratchBirdClient.HandshakeAndAuth;
var
  Params: TStringList;
  Features: UInt64;
  Startup: TBytes;
  Msg: TScratchBirdMessage;
  Scram: TScramClient;
  Method, Stage: Byte;
  Data, SessionId, ServerInfo: TBytes;
  Name, Value: string;
  Status: Byte;
  TxnId, Visibility: UInt64;
begin
  Params := BuildStartupParams;
  try
    Features := 0;
    if SameText(FConfig.Compression, 'zstd') then
      Features := Features or FEATURE_COMPRESSION;
    if FConfig.BinaryTransfer then
      Features := Features or FEATURE_STREAMING;
    Startup := BuildStartupPayload(Features, Params);
    SendMessage(MSG_STARTUP, Startup, 0, True);
  finally
    Params.Free;
  end;

  Scram := nil;
  try
    while True do
    begin
      Msg := ReceiveMessage;
      case Msg.MsgType of
        MSG_NEGOTIATE_VERSION:
          Continue;
        MSG_AUTH_REQUEST:
          begin
            ParseAuthRequest(Msg.Payload, Method, Data);
            if Method = AUTH_OK then
              Continue;
            FResolvedAuthContext.ResolvedMethodCode := Method;
            FResolvedAuthContext.ResolvedMethodName := AuthMethodName(Method);
            FResolvedAuthContext.ResolvedAuthPluginId := AuthPluginIdForMethod(Method, FConfig.AuthMethodId);
            if Method = AUTH_PASSWORD then
            begin
              SendMessage(MSG_AUTH_RESPONSE, TEncoding.UTF8.GetBytes(FConfig.Password), 0, True);
              Continue;
            end;
            if (Method = AUTH_SCRAM_SHA256) or (Method = AUTH_SCRAM_SHA512) then
            begin
              if Scram = nil then
              begin
                if Method = AUTH_SCRAM_SHA512 then
                  Scram := TScramClient.Create(FConfig.UserName, 'sha512')
                else
                  Scram := TScramClient.Create(FConfig.UserName, 'sha256');
              end;
              SendMessage(MSG_AUTH_RESPONSE, TEncoding.UTF8.GetBytes(Scram.ClientFirstMessage), 0, True);
              Continue;
            end;
            if Method = AUTH_TOKEN then
            begin
              SendMessage(MSG_AUTH_RESPONSE, ResolveTokenAuthPayload(FConfig), 0, True);
              Continue;
            end;
            if Method = AUTH_PEER then
              raise EScratchbirdAuthError.CreateWithInfo('Auth method PEER requires external broker support in this lane', '28000', '', '');
            if Method = AUTH_MD5 then
              raise EScratchbirdAuthError.CreateWithInfo('Auth method MD5 is admitted by the server but is not locally executable in this lane', '28000', '', '');
            if Method = AUTH_REATTACH then
              raise EScratchbirdAuthError.CreateWithInfo('Auth method REATTACH requires an engine-issued dormant token flow and is not yet public in this lane', '28000', '', '');
            raise EScratchbirdAuthError.CreateWithInfo('Unsupported auth method', '28000', '', '');
          end;
        MSG_AUTH_CONTINUE:
          begin
            ParseAuthContinue(Msg.Payload, Method, Stage, Data);
            if ((Method = AUTH_SCRAM_SHA256) or (Method = AUTH_SCRAM_SHA512)) and (Scram <> nil) then
            begin
              SendMessage(MSG_AUTH_RESPONSE, TEncoding.UTF8.GetBytes(Scram.HandleServerFirst(FConfig.Password,
                TEncoding.UTF8.GetString(Data))), 0, True);
              Continue;
            end;
            if Method = AUTH_TOKEN then
            begin
              SendMessage(MSG_AUTH_RESPONSE, ResolveTokenAuthPayload(FConfig), 0, True);
              Continue;
            end;
            if Method = AUTH_PEER then
              raise EScratchbirdAuthError.CreateWithInfo('Auth continuation PEER requires external broker support in this lane', '28000', '', '');
            raise EScratchbirdAuthError.CreateWithInfo('Unsupported auth continue', '28000', '', '');
          end;
        MSG_AUTH_OK:
          begin
            ParseAuthOk(Msg.Payload, SessionId, ServerInfo);
            if Length(SessionId) = 16 then
              FAttachmentId := SessionId
            else
              FAttachmentId := Msg.AttachmentId;
            ApplyRuntimeTxnId(Msg.TxnId);
            if (Scram <> nil) and (Length(ServerInfo) > 0) then
              Scram.VerifyServerFinal(TEncoding.UTF8.GetString(ServerInfo));
            Continue;
          end;
        MSG_PARAMETER_STATUS:
          begin
            ParseParameterStatus(Msg.Payload, Name, Value);
            HandleParameterStatus(Name, Value);
            Continue;
          end;
        MSG_READY:
          begin
            ParseReady(Msg.Payload, Status, TxnId, Visibility);
            ApplyRuntimeReadyState(Status, TxnId);
            FResolvedAuthContext.Attached := True;
            Exit;
          end;
        MSG_ERROR:
          raise BuildQueryError(Msg.Payload);
      end;
    end;
  finally
    Scram.Free;
  end;
end;

function TScratchBirdClient.BuildQueryError(const Payload: TBytes): EScratchBirdError;
var
  Severity, SqlState, Msg, Detail, Hint: string;
begin
  ParseErrorMessage(Payload, Severity, SqlState, Msg, Detail, Hint);
  Result := MapSqlState(SqlState, Msg, Detail, Hint);
end;

procedure TScratchBirdClient.HandleParameterStatus(const Name, Value: string);
var
  Bytes: TBytes;
  Parsed: UInt64;
begin
  FParameters.Values[Name] := Value;
  if SameText(Name, 'attachment_id') then
  begin
    Bytes := ParseUuidBytes(Value);
    if Length(Bytes) = 16 then
      FAttachmentId := Bytes;
  end;
  if SameText(Name, 'current_txn_id') then
  begin
    if TryStrToUInt64(Value, Parsed) then
      ApplyRuntimeTxnId(Parsed);
  end;
end;

procedure TScratchBirdClient.EnqueueNotification(const Notice: TNotification);
var
  Index: Integer;
begin
  Index := Length(FNotificationQueue);
  SetLength(FNotificationQueue, Index + 1);
  FNotificationQueue[Index] := Notice;
end;

procedure TScratchBirdClient.DispatchNotificationListeners(const Notice: TNotification);
var
  I: Integer;
begin
  for I := 0 to High(FNotificationListeners) do
  begin
    if Assigned(FNotificationListeners[I].Handler) then
      FNotificationListeners[I].Handler(Notice);
  end;
end;

function TScratchBirdClient.HandleAsyncMessage(const Msg: TScratchBirdMessage): Boolean;
var
  Name, Value: string;
  Notice: TNotification;
  Plan: TQueryPlan;
  Compiled: TSblrCompiled;
  Status: Byte;
  TxnId: UInt64;
begin
  Result := True;
  case Msg.MsgType of
    MSG_PARAMETER_STATUS:
      begin
        ParseParameterStatus(Msg.Payload, Name, Value);
        HandleParameterStatus(Name, Value);
      end;
    MSG_NOTIFICATION:
      begin
        ParseNotification(Msg.Payload, Notice);
        EnqueueNotification(Notice);
        if Assigned(FOnNotification) then
          FOnNotification(Notice);
        DispatchNotificationListeners(Notice);
      end;
    MSG_QUERY_PLAN:
      begin
        ParseQueryPlan(Msg.Payload, Plan);
        FLastPlan := Plan;
        FHasLastPlan := True;
      end;
    MSG_SBLR_COMPILED:
      begin
        ParseSblrCompiled(Msg.Payload, Compiled);
        FLastSblr := Compiled;
        FHasLastSblr := True;
      end;
    MSG_NOTICE:
      begin
        // Notice payloads are informational; keep result-stream processing uninterrupted.
      end;
    MSG_TXN_STATUS:
      begin
        ParseTxnStatus(Msg.Payload, Status, TxnId);
        ApplyRuntimeReadyState(Status, TxnId);
      end;
  else
    Result := False;
  end;
end;

procedure TScratchBirdClient.ParseNotification(const Payload: TBytes; out Notice: TNotification);
var
  Offset: Integer;
  ChannelLen, PayloadLen: Cardinal;
  function ReadUInt32LE(const Buffer: TBytes; Index: Integer): Cardinal;
  begin
    Result := 0;
    if Index + SizeOf(Result) <= Length(Buffer) then
      Move(Buffer[Index], Result, SizeOf(Result));
  end;
  function ReadUInt64LE(const Buffer: TBytes; Index: Integer): UInt64;
  begin
    Result := 0;
    if Index + SizeOf(Result) <= Length(Buffer) then
      Move(Buffer[Index], Result, SizeOf(Result));
  end;
begin
  FillChar(Notice, SizeOf(Notice), 0);
  Offset := 0;
  if Length(Payload) < 12 then
    Exit;
  Notice.ProcessId := ReadUInt32LE(Payload, Offset);
  Inc(Offset, 4);
  ChannelLen := ReadUInt32LE(Payload, Offset);
  Inc(Offset, 4);
  if Offset + Integer(ChannelLen) + 4 > Length(Payload) then
    Exit;
  Notice.Channel := TEncoding.UTF8.GetString(Copy(Payload, Offset, ChannelLen));
  Inc(Offset, ChannelLen);
  PayloadLen := ReadUInt32LE(Payload, Offset);
  Inc(Offset, 4);
  if Offset + Integer(PayloadLen) > Length(Payload) then
    Exit;
  Notice.Payload := Copy(Payload, Offset, PayloadLen);
  Inc(Offset, PayloadLen);
  Notice.ChangeType := '';
  Notice.HasRowId := False;
  if Offset < Length(Payload) then
  begin
    Notice.ChangeType := Char(Payload[Offset]);
    Inc(Offset);
    if Offset + SizeOf(UInt64) <= Length(Payload) then
    begin
      Notice.RowId := ReadUInt64LE(Payload, Offset);
      Notice.HasRowId := True;
    end;
  end;
end;

procedure TScratchBirdClient.ParseQueryPlan(const Payload: TBytes; out Plan: TQueryPlan);
var
  Offset: Integer;
  PlanLen: Cardinal;
  function ReadUInt32LE(const Buffer: TBytes; Index: Integer): Cardinal;
  begin
    Result := 0;
    if Index + SizeOf(Result) <= Length(Buffer) then
      Move(Buffer[Index], Result, SizeOf(Result));
  end;
  function ReadUInt64LE(const Buffer: TBytes; Index: Integer): UInt64;
  begin
    Result := 0;
    if Index + SizeOf(Result) <= Length(Buffer) then
      Move(Buffer[Index], Result, SizeOf(Result));
  end;
begin
  FillChar(Plan, SizeOf(Plan), 0);
  if Length(Payload) < 32 then
    Exit;
  Offset := 0;
  Plan.Format := ReadUInt32LE(Payload, Offset);
  Inc(Offset, 4);
  PlanLen := ReadUInt32LE(Payload, Offset);
  Inc(Offset, 4);
  Plan.PlanningTimeUs := ReadUInt64LE(Payload, Offset);
  Inc(Offset, 8);
  Plan.EstimatedRows := ReadUInt64LE(Payload, Offset);
  Inc(Offset, 8);
  Plan.EstimatedCost := ReadUInt64LE(Payload, Offset);
  Inc(Offset, 8);
  if Offset + Integer(PlanLen) > Length(Payload) then
    Exit;
  Plan.Plan := Copy(Payload, Offset, PlanLen);
end;

procedure TScratchBirdClient.ParseSblrCompiled(const Payload: TBytes; out Compiled: TSblrCompiled);
var
  Offset: Integer;
  Len: Cardinal;
  function ReadUInt32LE(const Buffer: TBytes; Index: Integer): Cardinal;
  begin
    Result := 0;
    if Index + SizeOf(Result) <= Length(Buffer) then
      Move(Buffer[Index], Result, SizeOf(Result));
  end;
  function ReadUInt64LE(const Buffer: TBytes; Index: Integer): UInt64;
  begin
    Result := 0;
    if Index + SizeOf(Result) <= Length(Buffer) then
      Move(Buffer[Index], Result, SizeOf(Result));
  end;
begin
  FillChar(Compiled, SizeOf(Compiled), 0);
  if Length(Payload) < 16 then
    Exit;
  Offset := 0;
  Compiled.Hash := ReadUInt64LE(Payload, Offset);
  Inc(Offset, 8);
  Compiled.Version := ReadUInt32LE(Payload, Offset);
  Inc(Offset, 4);
  Len := ReadUInt32LE(Payload, Offset);
  Inc(Offset, 4);
  if Offset + Integer(Len) > Length(Payload) then
    Exit;
  Compiled.Bytecode := Copy(Payload, Offset, Len);
end;

function TScratchBirdClient.ParseUuidBytes(const Value: string): TBytes;
var
  Hex: string;
  I: Integer;
  ByteVal: Integer;
begin
  Hex := StringReplace(Value, '-', '', [rfReplaceAll]);
  if Length(Hex) <> 32 then
  begin
    SetLength(Result, 0);
    Exit;
  end;
  SetLength(Result, 16);
  for I := 0 to 15 do
  begin
    if not TryStrToInt('$' + Copy(Hex, I * 2 + 1, 2), ByteVal) then
    begin
      SetLength(Result, 0);
      Exit;
    end;
    Result[I] := Byte(ByteVal);
  end;
end;

procedure TScratchBirdClient.EnsureConnected;
begin
  if not FConnected then
    raise EScratchbirdConnectionError.CreateWithInfo('Client is not connected', '08003', '', '');
end;

procedure TScratchBirdClient.EnsureTransactionActive(const Operation: string);
begin
  if (not FConnected) or (not FTransactionActive) then
    raise EScratchbirdTransactionError.CreateWithInfo(Operation + ' requires an active transaction', '25000', '', '');
end;

procedure TScratchBirdClient.ApplyRuntimeTxnId(TxnId: UInt64);
begin
  FTxnId := TxnId;
  FRuntimeBoundarySeen := True;
  if TxnId <> 0 then
    FTransactionActive := True
end;

procedure TScratchBirdClient.ApplyRuntimeReadyState(Status: Byte; TxnId: UInt64);
begin
  FRuntimeBoundarySeen := True;
  if Status <> 0 then
  begin
    FTxnId := TxnId;
    FTransactionActive := True;
    Exit;
  end;
  ClearTransactionState;
end;

procedure TScratchBirdClient.ClearTransactionState;
begin
  FTransactionActive := False;
  FExplicitTransaction := False;
  FTxnId := 0;
  FPortalResumePending := False;
end;

function TScratchBirdClient.CanAdoptFreshNativeBoundary(IsolationLevel: Byte; AccessMode: Byte;
  Deferrable: Boolean; WaitMode: Boolean; TimeoutMs: Cardinal; AutocommitMode: Byte;
  ConflictAction: Byte; HasReadCommittedMode: Boolean; ReadCommittedMode: Byte): Boolean;
begin
  Result :=
    (IsolationLevel in [ISOLATION_READ_UNCOMMITTED, ISOLATION_READ_COMMITTED]) and
    (AccessMode = 0) and
    (not Deferrable) and
    (not WaitMode) and
    (TimeoutMs = 0) and
    (AutocommitMode = 0) and
    (ConflictAction = 0) and
    ((not HasReadCommittedMode) or (ReadCommittedMode = READ_COMMITTED_MODE_DEFAULT));
end;

function TScratchBirdClient.NormalizeSavepointName(const Name: string): string;
begin
  Result := Trim(Name);
  if Result = '' then
    raise EScratchbirdSyntaxError.CreateWithInfo('savepoint name is required', '42601', '', '');
end;

function TScratchBirdClient.NormalizeSqlText(const Sql: string): string;
begin
  Result := Trim(Sql);
  if Result = '' then
    raise EScratchbirdSyntaxError.CreateWithInfo('SQL text is required', '42601', '', '');
end;

function TScratchBirdClient.QuoteStringLiteral(const Value: string): string;
begin
  Result := '''' + StringReplace(Value, '''', '''''', [rfReplaceAll]) + '''';
end;

function TScratchBirdClient.BuildPreparedTransactionSql(const Verb, GlobalTransactionId: string): string;
var
  NormalizedGid: string;
begin
  NormalizedGid := Trim(GlobalTransactionId);
  if NormalizedGid = '' then
    raise EScratchbirdSyntaxError.CreateWithInfo(
      'global transaction id is required',
      '42601', '', ''
    );
  Result := Verb + ' ' + QuoteStringLiteral(NormalizedGid);
end;

procedure TScratchBirdClient.AllowPortalResume;
begin
  FPortalResumePending := True;
end;

procedure TScratchBirdClient.ResumeSuspendedPortal(MaxRows: Cardinal);
begin
  if not FPortalResumePending then
    raise EScratchBirdError.CreateWithInfo(
      'portal resume requires an explicit suspended result state',
      '55000', '', ''
    );
  FPortalResumePending := False;
  FLastQuerySequence := SendMessage(MSG_EXECUTE, BuildExecutePayload('', MaxRows), 0, False);
end;

function TScratchBirdClient.BeginOperation(const Name, Sql: string): TSpanContext;
begin
  if Assigned(FCircuitBreaker) and (not FCircuitBreaker.AllowRequest) then
    raise EScratchbirdConnectionError.CreateWithInfo('Circuit breaker is OPEN', '08006', '', '');
  if Assigned(FKeepaliveTracker) and FKeepaliveTracker.NeedsValidation then
  begin
    Ping;
    FKeepaliveTracker.MarkActive;
  end;
  if Assigned(FTelemetry) then
    Result := FTelemetry.StartSpan(Name)
  else
    Result := nil;
  if (Result <> nil) and (Sql <> '') then
    Result.WithAttribute('db.statement', TTelemetryCollector.SanitizeQuery(Sql));
end;

procedure TScratchBirdClient.EndOperation(Span: TSpanContext; Success: Boolean);
begin
  if Assigned(FCircuitBreaker) then
  begin
    if Success then
      FCircuitBreaker.RecordSuccess
    else
      FCircuitBreaker.RecordFailure;
  end;
  if Assigned(FKeepaliveTracker) then
    FKeepaliveTracker.MarkActive;
  if Assigned(FTelemetry) then
    FTelemetry.EndSpan(Span, Success);
end;

procedure TScratchBirdClient.DrainUntilReady;
var
  Msg: TScratchBirdMessage;
  Status: Byte;
  TxnId, Visibility: UInt64;
begin
  while True do
  begin
    Msg := ReceiveMessage;
    if HandleAsyncMessage(Msg) then
      Continue;
    case Msg.MsgType of
      MSG_ERROR:
        begin
          FExplicitTransaction := False;
        raise BuildQueryError(Msg.Payload);
        end;
      MSG_READY:
        begin
          ParseReady(Msg.Payload, Status, TxnId, Visibility);
          ApplyRuntimeReadyState(Status, TxnId);
          FPortalResumePending := False;
          Exit;
        end;
    end;
  end;
end;

function TScratchBirdClient.DescribeStatement(const StatementName: string): Integer;
var
  Payload: TBytes;
  Msg: TScratchBirdMessage;
  Status: Byte;
  TxnId, Visibility: UInt64;
  Types: TArray<Cardinal>;
begin
  Payload := BuildDescribePayload(Ord('S'), StatementName);
  SendMessage(MSG_DESCRIBE, Payload, 0, False);
  SendMessage(MSG_SYNC, nil, 0, False);
  Result := -1;
  while True do
  begin
    Msg := ReceiveMessage;
    if HandleAsyncMessage(Msg) then
      Continue;
    case Msg.MsgType of
      MSG_PARAMETER_DESCRIPTION:
        begin
          Types := ParseParameterDescription(Msg.Payload);
          Result := Length(Types);
        end;
      MSG_ERROR:
        raise BuildQueryError(Msg.Payload);
      MSG_READY:
        begin
          ParseReady(Msg.Payload, Status, TxnId, Visibility);
          ApplyRuntimeReadyState(Status, TxnId);
          FPortalResumePending := False;
          Exit;
        end;
    end;
  end;
end;

procedure TScratchBirdClient.SendSimpleQuery(const Sql: string; MaxRows: Cardinal);
var
  Flags: Cardinal;
  Payload: TBytes;
begin
  Flags := 0;
  if FConfig.BinaryTransfer then
    Flags := Flags or $04;
  FLastMaxRows := MaxRows;
  FPortalResumePending := False;
  Payload := BuildQueryPayload(Sql, Flags, MaxRows, 0);
  FHasLastPlan := False;
  FHasLastSblr := False;
  FLastQuerySequence := SendMessage(MSG_QUERY, Payload, 0, False);
end;

procedure TScratchBirdClient.SendExtendedQuery(const Sql: string; const Params: array of TScratchBirdParamInput; MaxRows: Cardinal);
var
  ParamValues: TArray<TParamValue>;
  ParamTypes: TArray<Cardinal>;
  I: Integer;
  Param: TParamValue;
  Oid: Cardinal;
  ParamCount: Integer;
  ParsePayload, BindPayload, ExecPayload: TBytes;
  ResultFormats: TArray<Word>;
begin
  SetLength(ParamValues, Length(Params));
  SetLength(ParamTypes, Length(Params));
  for I := 0 to High(Params) do
  begin
    EncodeParam(Params[I].Value, Params[I].Obj, Param, Oid);
    ParamValues[I] := Param;
    ParamTypes[I] := Oid;
  end;
  ParsePayload := BuildParsePayload('', Sql, ParamTypes);
  SendMessage(MSG_PARSE, ParsePayload, 0, False);
  ParamCount := DescribeStatement('');
  if (ParamCount >= 0) and (ParamCount <> Length(Params)) then
    raise EScratchBirdError.CreateWithInfo('parameter count mismatch', '07001', '', '');
  if FConfig.BinaryTransfer then
  begin
    SetLength(ResultFormats, 1);
    ResultFormats[0] := FORMAT_BINARY;
  end
  else
    ResultFormats := nil;
  BindPayload := BuildBindPayload('', '', ParamValues, ResultFormats);
  SendMessage(MSG_BIND, BindPayload, 0, False);
  FLastMaxRows := MaxRows;
  FPortalResumePending := False;
  ExecPayload := BuildExecutePayload('', MaxRows);
  FHasLastPlan := False;
  FHasLastSblr := False;
  FLastQuerySequence := SendMessage(MSG_EXECUTE, ExecPayload, 0, False);
  if MaxRows = 0 then
    SendMessage(MSG_SYNC, nil, 0, False);
end;

function TScratchBirdClient.CurrentMaxRows: Cardinal;
begin
  Result := FLastMaxRows;
end;

end.
