// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

unit ScratchBird.FireDAC;

{$mode delphi}
{$H+}

interface

uses
  SysUtils, Classes, Variants,
  ScratchBird.Client, ScratchBird.Common, ScratchBird.Sql, ScratchBird.Metadata;

type
  TScratchBirdFDConnection = class(TComponent)
  private
    FClient: TScratchBirdClient;
    FDsn: string;
    FConnected: Boolean;
  public
    constructor Create(AOwner: TComponent); override;
    destructor Destroy; override;
    procedure Open;
    procedure Close;
    procedure StartTransaction;
    procedure StartTransactionEx(IsolationLevel, AccessMode: Byte; Deferrable, WaitMode: Boolean;
      TimeoutMs: Cardinal; AutocommitMode, ConflictAction: Byte); overload;
    procedure StartTransactionEx(IsolationLevel, AccessMode: Byte; Deferrable, WaitMode: Boolean;
      TimeoutMs: Cardinal; AutocommitMode, ConflictAction, ReadCommittedMode: Byte); overload;
    procedure Commit;
    procedure Rollback;
    procedure ExecSQL(const Sql: string);
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

  TScratchBirdFDQuery = class(TComponent)
  private
    FConnection: TScratchBirdFDConnection;
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
    property Connection: TScratchBirdFDConnection read FConnection write FConnection;
  end;

implementation

constructor TScratchBirdFDConnection.Create(AOwner: TComponent);
begin
  inherited Create(AOwner);
  FClient := TScratchBirdClient.Create;
end;

destructor TScratchBirdFDConnection.Destroy;
begin
  FClient.Free;
  inherited Destroy;
end;

procedure TScratchBirdFDConnection.Open;
begin
  if FConnected then
    Exit;
  FClient.Connect(FDsn);
  FConnected := True;
end;

procedure TScratchBirdFDConnection.Close;
begin
  if not FConnected then
    Exit;
  FClient.Disconnect;
  FConnected := False;
end;

procedure TScratchBirdFDConnection.StartTransaction;
begin
  FClient.BeginTransaction;
end;

procedure TScratchBirdFDConnection.StartTransactionEx(IsolationLevel, AccessMode: Byte; Deferrable, WaitMode: Boolean;
  TimeoutMs: Cardinal; AutocommitMode, ConflictAction: Byte);
begin
  FClient.BeginTransactionEx(IsolationLevel, AccessMode, Deferrable, WaitMode, TimeoutMs, AutocommitMode, ConflictAction);
end;

procedure TScratchBirdFDConnection.StartTransactionEx(IsolationLevel, AccessMode: Byte; Deferrable, WaitMode: Boolean;
  TimeoutMs: Cardinal; AutocommitMode, ConflictAction, ReadCommittedMode: Byte);
begin
  FClient.BeginTransactionEx(IsolationLevel, AccessMode, Deferrable, WaitMode, TimeoutMs, AutocommitMode,
    ConflictAction, ReadCommittedMode);
end;

procedure TScratchBirdFDConnection.Commit;
begin
  FClient.Commit;
end;

procedure TScratchBirdFDConnection.Rollback;
begin
  FClient.Rollback;
end;

procedure TScratchBirdFDConnection.ExecSQL(const Sql: string);
begin
  FClient.ExecSQL(Sql);
end;

procedure TScratchBirdFDConnection.ExecSQLParams(const Sql: string; const Params: array of TScratchBirdParamInput);
begin
  FClient.ExecSQLParams(Sql, Params);
end;

function TScratchBirdFDConnection.ExecuteQueryParams(const Sql: string; const Params: array of TScratchBirdParamInput): TScratchBirdResultStream;
begin
  Result := FClient.ExecuteQueryParams(Sql, Params);
end;

function TScratchBirdFDConnection.QueryMetadata(const CollectionName: string): TScratchBirdResultStream;
begin
  Result := FClient.QueryMetadata(CollectionName);
end;

function TScratchBirdFDConnection.GetSchema(const CollectionName: string): TScratchBirdResultStream;
begin
  Result := FClient.GetSchema(CollectionName);
end;

function TScratchBirdFDConnection.QueryMetadataRows(const CollectionName: string): TMetadataRows;
begin
  Result := FClient.QueryMetadataRows(CollectionName);
end;

function TScratchBirdFDConnection.QueryMetadataRows(const CollectionName: string; const Restrictions: TMetadataRow): TMetadataRows;
begin
  Result := FClient.QueryMetadataRows(CollectionName, Restrictions);
end;

function TScratchBirdFDConnection.GetSchemaRows(const CollectionName: string): TMetadataRows;
begin
  Result := FClient.GetSchemaRows(CollectionName);
end;

function TScratchBirdFDConnection.GetSchemaRows(const CollectionName: string; const Restrictions: TMetadataRow): TMetadataRows;
begin
  Result := FClient.GetSchemaRows(CollectionName, Restrictions);
end;

function TScratchBirdFDConnection.GetCatalogs: TScratchBirdResultStream;
begin
  Result := FClient.GetCatalogs;
end;

function TScratchBirdFDConnection.GetSchemas: TScratchBirdResultStream;
begin
  Result := FClient.GetSchemas;
end;

function TScratchBirdFDConnection.GetTables: TScratchBirdResultStream;
begin
  Result := FClient.GetTables;
end;

function TScratchBirdFDConnection.GetColumns: TScratchBirdResultStream;
begin
  Result := FClient.GetColumns;
end;

function TScratchBirdFDConnection.GetIndexes: TScratchBirdResultStream;
begin
  Result := FClient.GetIndexes;
end;

function TScratchBirdFDConnection.GetIndexColumns: TScratchBirdResultStream;
begin
  Result := FClient.GetIndexColumns;
end;

function TScratchBirdFDConnection.GetConstraints: TScratchBirdResultStream;
begin
  Result := FClient.GetConstraints;
end;

function TScratchBirdFDConnection.GetProcedures: TScratchBirdResultStream;
begin
  Result := FClient.GetProcedures;
end;

function TScratchBirdFDConnection.GetFunctions: TScratchBirdResultStream;
begin
  Result := FClient.GetFunctions;
end;

function TScratchBirdFDConnection.GetRoutines: TScratchBirdResultStream;
begin
  Result := FClient.GetRoutines;
end;

function TScratchBirdFDConnection.GetPrimaryKeys: TScratchBirdResultStream;
begin
  Result := FClient.GetPrimaryKeys;
end;

function TScratchBirdFDConnection.GetForeignKeys: TScratchBirdResultStream;
begin
  Result := FClient.GetForeignKeys;
end;

function TScratchBirdFDConnection.GetTablePrivileges: TScratchBirdResultStream;
begin
  Result := FClient.GetTablePrivileges;
end;

function TScratchBirdFDConnection.GetColumnPrivileges: TScratchBirdResultStream;
begin
  Result := FClient.GetColumnPrivileges;
end;

function TScratchBirdFDConnection.GetTypeInfo: TScratchBirdResultStream;
begin
  Result := FClient.GetTypeInfo;
end;

constructor TScratchBirdFDQuery.Create(AOwner: TComponent);
begin
  inherited Create(AOwner);
  FSQL := TStringList.Create;
  FParams := TScratchBirdParams.Create;
  FPrepared := False;
end;

destructor TScratchBirdFDQuery.Destroy;
begin
  FParams.Free;
  FSQL.Free;
  inherited Destroy;
end;

procedure TScratchBirdFDQuery.Prepare;
var
  Ordered: TArray<TScratchBirdParamInput>;
begin
  if FConnection = nil then
    raise Exception.Create('Connection not assigned');
  FPreparedSql := BuildSql(Ordered);
  FPreparedParams := Ordered;
  FPrepared := True;
end;

procedure TScratchBirdFDQuery.Open;
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

procedure TScratchBirdFDQuery.ExecSQL;
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

procedure TScratchBirdFDQuery.Next;
begin
  if Assigned(FResult) then
    FResult.Next;
end;

function TScratchBirdFDQuery.Eof: Boolean;
begin
  Result := (FResult = nil) or FResult.Eof;
end;

function TScratchBirdFDQuery.FieldByName(const Name: string): Variant;
begin
  if FResult = nil then
    Result := Null
  else
    Result := FResult.FieldByName(Name);
end;

function TScratchBirdFDQuery.RowsAffected: Int64;
begin
  if FResult = nil then
    Result := 0
  else
    Result := FResult.RowsAffected;
end;

function TScratchBirdFDQuery.ParamByName(const Name: string): TScratchBirdParam;
begin
  Result := FParams.ParamByName(Name);
end;

function TScratchBirdFDQuery.BuildSql(out Ordered: TArray<TScratchBirdParamInput>): string;
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

end.
