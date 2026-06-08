// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

unit ScratchBird.Zeos;

{$mode delphi}
{$H+}

interface

uses
  SysUtils, Classes, Variants,
  ScratchBird.Client, ScratchBird.Common, ScratchBird.Sql, ScratchBird.Metadata;

type
  TScratchBirdZConnection = class(TComponent)
  private
    FClient: TScratchBirdClient;
    FDsn: string;
    FConnected: Boolean;
  public
    constructor Create(AOwner: TComponent); override;
    destructor Destroy; override;
    procedure Connect;
    procedure Disconnect;
    procedure StartTransaction;
    procedure StartTransactionEx(IsolationLevel, AccessMode: Byte; Deferrable, WaitMode: Boolean;
      TimeoutMs: Cardinal; AutocommitMode, ConflictAction: Byte); overload;
    procedure StartTransactionEx(IsolationLevel, AccessMode: Byte; Deferrable, WaitMode: Boolean;
      TimeoutMs: Cardinal; AutocommitMode, ConflictAction, ReadCommittedMode: Byte); overload;
    procedure Commit;
    procedure Rollback;
    procedure ExecSQLParams(const Sql: string; const Params: array of TScratchBirdParamInput); virtual;
    function ExecuteQueryParams(const Sql: string; const Params: array of TScratchBirdParamInput): TScratchBirdResultStream; virtual;
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
    property Connected: Boolean read FConnected;
    property Dsn: string read FDsn write FDsn;
    property Client: TScratchBirdClient read FClient;
  end;

  TScratchBirdZQuery = class(TComponent)
  private
    FConnection: TScratchBirdZConnection;
    FSQL: TStringList;
    FParams: TScratchBirdParams;
    FResult: TScratchBirdQueryResult;
    FPreparedSql: string;
    FPreparedParams: TArray<TScratchBirdParamInput>;
    FPrepared: Boolean;
    function BuildSql(out Ordered: TArray<TScratchBirdParamInput>): string;
  public
    constructor Create(AOwner: TComponent); override;
    destructor Destroy; override;
    procedure Prepare;
    procedure Open;
    procedure ExecSQL;
    procedure Next;
    function Eof: Boolean;
    function FieldByName(const Name: string): Variant;
    function RowsAffected: Int64;
    function ParamByName(const Name: string): TScratchBirdParam;
    property SQL: TStringList read FSQL;
    property Params: TScratchBirdParams read FParams;
    property Connection: TScratchBirdZConnection read FConnection write FConnection;
  end;

implementation

constructor TScratchBirdZConnection.Create(AOwner: TComponent);
begin
  inherited Create(AOwner);
  FClient := TScratchBirdClient.Create;
end;

destructor TScratchBirdZConnection.Destroy;
begin
  FClient.Free;
  inherited Destroy;
end;

procedure TScratchBirdZConnection.Connect;
begin
  if FConnected then
    Exit;
  FClient.Connect(FDsn);
  FConnected := True;
end;

procedure TScratchBirdZConnection.Disconnect;
begin
  if not FConnected then
    Exit;
  FClient.Disconnect;
  FConnected := False;
end;

procedure TScratchBirdZConnection.StartTransaction;
begin
  FClient.BeginTransaction;
end;

procedure TScratchBirdZConnection.StartTransactionEx(IsolationLevel, AccessMode: Byte; Deferrable, WaitMode: Boolean;
  TimeoutMs: Cardinal; AutocommitMode, ConflictAction: Byte);
begin
  FClient.BeginTransactionEx(IsolationLevel, AccessMode, Deferrable, WaitMode, TimeoutMs, AutocommitMode, ConflictAction);
end;

procedure TScratchBirdZConnection.StartTransactionEx(IsolationLevel, AccessMode: Byte; Deferrable, WaitMode: Boolean;
  TimeoutMs: Cardinal; AutocommitMode, ConflictAction, ReadCommittedMode: Byte);
begin
  FClient.BeginTransactionEx(IsolationLevel, AccessMode, Deferrable, WaitMode, TimeoutMs, AutocommitMode,
    ConflictAction, ReadCommittedMode);
end;

procedure TScratchBirdZConnection.Commit;
begin
  FClient.Commit;
end;

procedure TScratchBirdZConnection.Rollback;
begin
  FClient.Rollback;
end;

procedure TScratchBirdZConnection.ExecSQLParams(const Sql: string; const Params: array of TScratchBirdParamInput);
begin
  FClient.ExecSQLParams(Sql, Params);
end;

function TScratchBirdZConnection.ExecuteQueryParams(const Sql: string; const Params: array of TScratchBirdParamInput): TScratchBirdResultStream;
begin
  Result := FClient.ExecuteQueryParams(Sql, Params);
end;

function TScratchBirdZConnection.QueryMetadata(const CollectionName: string): TScratchBirdResultStream;
begin
  Result := FClient.QueryMetadata(CollectionName);
end;

function TScratchBirdZConnection.GetSchema(const CollectionName: string): TScratchBirdResultStream;
begin
  Result := FClient.GetSchema(CollectionName);
end;

function TScratchBirdZConnection.QueryMetadataRows(const CollectionName: string): TMetadataRows;
begin
  Result := FClient.QueryMetadataRows(CollectionName);
end;

function TScratchBirdZConnection.QueryMetadataRows(const CollectionName: string; const Restrictions: TMetadataRow): TMetadataRows;
begin
  Result := FClient.QueryMetadataRows(CollectionName, Restrictions);
end;

function TScratchBirdZConnection.GetSchemaRows(const CollectionName: string): TMetadataRows;
begin
  Result := FClient.GetSchemaRows(CollectionName);
end;

function TScratchBirdZConnection.GetSchemaRows(const CollectionName: string; const Restrictions: TMetadataRow): TMetadataRows;
begin
  Result := FClient.GetSchemaRows(CollectionName, Restrictions);
end;

function TScratchBirdZConnection.GetCatalogs: TScratchBirdResultStream;
begin
  Result := FClient.GetCatalogs;
end;

function TScratchBirdZConnection.GetSchemas: TScratchBirdResultStream;
begin
  Result := FClient.GetSchemas;
end;

function TScratchBirdZConnection.GetTables: TScratchBirdResultStream;
begin
  Result := FClient.GetTables;
end;

function TScratchBirdZConnection.GetColumns: TScratchBirdResultStream;
begin
  Result := FClient.GetColumns;
end;

function TScratchBirdZConnection.GetIndexes: TScratchBirdResultStream;
begin
  Result := FClient.GetIndexes;
end;

function TScratchBirdZConnection.GetIndexColumns: TScratchBirdResultStream;
begin
  Result := FClient.GetIndexColumns;
end;

function TScratchBirdZConnection.GetConstraints: TScratchBirdResultStream;
begin
  Result := FClient.GetConstraints;
end;

function TScratchBirdZConnection.GetProcedures: TScratchBirdResultStream;
begin
  Result := FClient.GetProcedures;
end;

function TScratchBirdZConnection.GetFunctions: TScratchBirdResultStream;
begin
  Result := FClient.GetFunctions;
end;

function TScratchBirdZConnection.GetRoutines: TScratchBirdResultStream;
begin
  Result := FClient.GetRoutines;
end;

function TScratchBirdZConnection.GetPrimaryKeys: TScratchBirdResultStream;
begin
  Result := FClient.GetPrimaryKeys;
end;

function TScratchBirdZConnection.GetForeignKeys: TScratchBirdResultStream;
begin
  Result := FClient.GetForeignKeys;
end;

function TScratchBirdZConnection.GetTablePrivileges: TScratchBirdResultStream;
begin
  Result := FClient.GetTablePrivileges;
end;

function TScratchBirdZConnection.GetColumnPrivileges: TScratchBirdResultStream;
begin
  Result := FClient.GetColumnPrivileges;
end;

function TScratchBirdZConnection.GetTypeInfo: TScratchBirdResultStream;
begin
  Result := FClient.GetTypeInfo;
end;

constructor TScratchBirdZQuery.Create(AOwner: TComponent);
begin
  inherited Create(AOwner);
  FSQL := TStringList.Create;
  FParams := TScratchBirdParams.Create;
  FPrepared := False;
end;

destructor TScratchBirdZQuery.Destroy;
begin
  FParams.Free;
  FSQL.Free;
  inherited Destroy;
end;

function TScratchBirdZQuery.BuildSql(out Ordered: TArray<TScratchBirdParamInput>): string;
var
  Names: array of string;
  Values: array of TScratchBirdParamInput;
  I: Integer;
begin
  if FParams.Count = 0 then
  begin
    Ordered := nil;
    Exit(FSQL.Text);
  end;
  SetLength(Names, FParams.Count);
  SetLength(Values, FParams.Count);
  for I := 0 to FParams.Count - 1 do
  begin
    Names[I] := FParams[I].Name;
    Values[I].Value := FParams[I].Value;
    Values[I].Obj := FParams[I].ObjectValue;
  end;
  if (Pos('@', FSQL.Text) > 0) or (Pos(':', FSQL.Text) > 0) then
    Result := NormalizeNamedSql(FSQL.Text, Names, Values, Ordered)
  else
    Result := NormalizePositionalSql(FSQL.Text, Values, Ordered);
end;

procedure TScratchBirdZQuery.Prepare;
var
  Ordered: TArray<TScratchBirdParamInput>;
begin
  if FConnection = nil then
    raise Exception.Create('Connection not assigned');
  FPreparedSql := BuildSql(Ordered);
  FPreparedParams := Ordered;
  FPrepared := True;
end;

procedure TScratchBirdZQuery.Open;
var
  Ordered: TArray<TScratchBirdParamInput>;
  SqlText: string;
begin
  if FConnection = nil then
    raise Exception.Create('Connection not assigned');
  if FPrepared then
  begin
    SqlText := FPreparedSql;
    Ordered := Copy(FPreparedParams);
  end
  else
    SqlText := BuildSql(Ordered);
  FResult := TScratchBirdQueryResult.Create(FConnection.ExecuteQueryParams(SqlText, Ordered));
  FResult.Next;
end;

procedure TScratchBirdZQuery.ExecSQL;
var
  Ordered: TArray<TScratchBirdParamInput>;
  SqlText: string;
begin
  if FConnection = nil then
    raise Exception.Create('Connection not assigned');
  if FPrepared then
  begin
    SqlText := FPreparedSql;
    Ordered := Copy(FPreparedParams);
  end
  else
    SqlText := BuildSql(Ordered);
  FConnection.ExecSQLParams(SqlText, Ordered);
end;

procedure TScratchBirdZQuery.Next;
begin
  if Assigned(FResult) then
    FResult.Next;
end;

function TScratchBirdZQuery.Eof: Boolean;
begin
  Result := (FResult = nil) or FResult.Eof;
end;

function TScratchBirdZQuery.FieldByName(const Name: string): Variant;
begin
  if FResult = nil then
    Result := Null
  else
    Result := FResult.FieldByName(Name);
end;

function TScratchBirdZQuery.RowsAffected: Int64;
begin
  if FResult = nil then
    Result := 0
  else
    Result := FResult.RowsAffected;
end;

function TScratchBirdZQuery.ParamByName(const Name: string): TScratchBirdParam;
begin
  Result := FParams.ParamByName(Name);
end;

end.
