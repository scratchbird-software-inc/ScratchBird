// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

program AdapterPrepareLifecycleTests;

{$mode delphi}
{$APPTYPE CONSOLE}

uses
  {$IFDEF UNIX}
  cthreads,
  {$ENDIF}
  SysUtils, Variants,
  ScratchBird.Client, ScratchBird.Common, ScratchBird.Sql,
  ScratchBird.FireDAC, ScratchBird.IBX, ScratchBird.Zeos, ScratchBird.SQLdb;

type
  TFDConnectionProbe = class(TScratchBirdFDConnection)
  public
    LastSql: string;
    LastParams: TArray<TScratchBirdParamInput>;
    ExecCalls: Integer;
    procedure ExecSQLParams(const Sql: string; const Params: array of TScratchBirdParamInput); override;
    function ExecuteQueryParams(const Sql: string; const Params: array of TScratchBirdParamInput): TScratchBirdResultStream; override;
  end;

  TIBDatabaseProbe = class(TScratchBirdIBDatabase)
  public
    LastSql: string;
    LastParams: TArray<TScratchBirdParamInput>;
    ExecCalls: Integer;
    procedure ExecSQLParams(const Sql: string; const Params: array of TScratchBirdParamInput); override;
    function ExecuteQueryParams(const Sql: string; const Params: array of TScratchBirdParamInput): TScratchBirdResultStream; override;
  end;

  TZConnectionProbe = class(TScratchBirdZConnection)
  public
    LastSql: string;
    LastParams: TArray<TScratchBirdParamInput>;
    ExecCalls: Integer;
    procedure ExecSQLParams(const Sql: string; const Params: array of TScratchBirdParamInput); override;
    function ExecuteQueryParams(const Sql: string; const Params: array of TScratchBirdParamInput): TScratchBirdResultStream; override;
  end;

  TSQLConnectionProbe = class(TScratchBirdSQLConnection)
  public
    LastSql: string;
    LastParams: TArray<TScratchBirdParamInput>;
    ExecCalls: Integer;
    procedure ExecSQLParams(const Sql: string; const Params: array of TScratchBirdParamInput); override;
    function ExecuteQueryParams(const Sql: string; const Params: array of TScratchBirdParamInput): TScratchBirdResultStream; override;
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

procedure AssertContains(const Needle, Haystack, MessageText: string);
begin
  if Pos(Needle, Haystack) = 0 then
    Fail(MessageText + ': expected "' + Needle + '" in "' + Haystack + '"');
end;

procedure CaptureParams(const Params: array of TScratchBirdParamInput; out Target: TArray<TScratchBirdParamInput>);
var
  I: Integer;
begin
  SetLength(Target, Length(Params));
  for I := 0 to High(Params) do
    Target[I] := Params[I];
end;

procedure TFDConnectionProbe.ExecSQLParams(const Sql: string; const Params: array of TScratchBirdParamInput);
begin
  LastSql := Sql;
  CaptureParams(Params, LastParams);
  Inc(ExecCalls);
end;

function TFDConnectionProbe.ExecuteQueryParams(const Sql: string; const Params: array of TScratchBirdParamInput): TScratchBirdResultStream;
begin
  LastSql := Sql;
  CaptureParams(Params, LastParams);
  Result := nil;
end;

procedure TIBDatabaseProbe.ExecSQLParams(const Sql: string; const Params: array of TScratchBirdParamInput);
begin
  LastSql := Sql;
  CaptureParams(Params, LastParams);
  Inc(ExecCalls);
end;

function TIBDatabaseProbe.ExecuteQueryParams(const Sql: string; const Params: array of TScratchBirdParamInput): TScratchBirdResultStream;
begin
  LastSql := Sql;
  CaptureParams(Params, LastParams);
  Result := nil;
end;

procedure TZConnectionProbe.ExecSQLParams(const Sql: string; const Params: array of TScratchBirdParamInput);
begin
  LastSql := Sql;
  CaptureParams(Params, LastParams);
  Inc(ExecCalls);
end;

function TZConnectionProbe.ExecuteQueryParams(const Sql: string; const Params: array of TScratchBirdParamInput): TScratchBirdResultStream;
begin
  LastSql := Sql;
  CaptureParams(Params, LastParams);
  Result := nil;
end;

procedure TSQLConnectionProbe.ExecSQLParams(const Sql: string; const Params: array of TScratchBirdParamInput);
begin
  LastSql := Sql;
  CaptureParams(Params, LastParams);
  Inc(ExecCalls);
end;

function TSQLConnectionProbe.ExecuteQueryParams(const Sql: string; const Params: array of TScratchBirdParamInput): TScratchBirdResultStream;
begin
  LastSql := Sql;
  CaptureParams(Params, LastParams);
  Result := nil;
end;

procedure TestFireDACPrepareRequiresConnection;
var
  Query: TScratchBirdFDQuery;
begin
  Query := TScratchBirdFDQuery.Create(nil);
  try
    try
      Query.Prepare;
      Fail('expected FireDAC prepare connection guard');
    except
      on E: Exception do
        AssertContains('Connection not assigned', E.Message, 'FireDAC prepare guard message');
    end;
  finally
    Query.Free;
  end;
end;

procedure TestIBXPrepareRequiresDatabase;
var
  Query: TScratchBirdIBQuery;
begin
  Query := TScratchBirdIBQuery.Create(nil);
  try
    try
      Query.Prepare;
      Fail('expected IBX prepare database guard');
    except
      on E: Exception do
        AssertContains('Database not assigned', E.Message, 'IBX prepare guard message');
    end;
  finally
    Query.Free;
  end;
end;

procedure TestZeosPrepareRequiresConnection;
var
  Query: TScratchBirdZQuery;
begin
  Query := TScratchBirdZQuery.Create(nil);
  try
    try
      Query.Prepare;
      Fail('expected Zeos prepare connection guard');
    except
      on E: Exception do
        AssertContains('Connection not assigned', E.Message, 'Zeos prepare guard message');
    end;
  finally
    Query.Free;
  end;
end;

procedure TestSQLdbPrepareRequiresConnection;
var
  Query: TScratchBirdSQLQuery;
begin
  Query := TScratchBirdSQLQuery.Create(nil);
  try
    try
      Query.Prepare;
      Fail('expected SQLdb prepare connection guard');
    except
      on E: Exception do
        AssertContains('Connection not assigned', E.Message, 'SQLdb prepare guard message');
    end;
  finally
    Query.Free;
  end;
end;

procedure TestFireDACPrepareCachesSqlAndParamsForExec;
var
  Conn: TFDConnectionProbe;
  Query: TScratchBirdFDQuery;
begin
  Conn := TFDConnectionProbe.Create(nil);
  Query := TScratchBirdFDQuery.Create(nil);
  try
    Query.Connection := Conn;
    Query.SQL.Text := 'SELECT :id::INTEGER AS before_prepare';
    Query.ParamByName('id').Value := 7;

    Query.Prepare;

    Query.SQL.Text := 'SELECT :id::INTEGER AS after_prepare';
    Query.ParamByName('id').Value := 9;
    Query.ExecSQL;

    AssertEqualInt(1, Conn.ExecCalls, 'FireDAC exec calls');
    AssertContains('before_prepare', Conn.LastSql, 'FireDAC uses prepared SQL snapshot');
    AssertContains('$1::INTEGER', Conn.LastSql, 'FireDAC prepared SQL normalization');
    AssertEqualInt(1, Length(Conn.LastParams), 'FireDAC prepared param count');
    AssertEqualInt(7, VarAsType(Conn.LastParams[0].Value, varInteger), 'FireDAC uses prepared params snapshot');
  finally
    Query.Free;
    Conn.Free;
  end;
end;

procedure TestIBXPrepareCachesSqlAndParamsForExec;
var
  Conn: TIBDatabaseProbe;
  Query: TScratchBirdIBQuery;
begin
  Conn := TIBDatabaseProbe.Create(nil);
  Query := TScratchBirdIBQuery.Create(nil);
  try
    Query.Database := Conn;
    Query.SQL.Text := 'SELECT :id::INTEGER AS before_prepare';
    Query.ParamByName('id').Value := 7;

    Query.Prepare;

    Query.SQL.Text := 'SELECT :id::INTEGER AS after_prepare';
    Query.ParamByName('id').Value := 9;
    Query.ExecSQL;

    AssertEqualInt(1, Conn.ExecCalls, 'IBX exec calls');
    AssertContains('before_prepare', Conn.LastSql, 'IBX uses prepared SQL snapshot');
    AssertContains('$1::INTEGER', Conn.LastSql, 'IBX prepared SQL normalization');
    AssertEqualInt(1, Length(Conn.LastParams), 'IBX prepared param count');
    AssertEqualInt(7, VarAsType(Conn.LastParams[0].Value, varInteger), 'IBX uses prepared params snapshot');
  finally
    Query.Free;
    Conn.Free;
  end;
end;

procedure TestZeosPrepareCachesSqlAndParamsForExec;
var
  Conn: TZConnectionProbe;
  Query: TScratchBirdZQuery;
begin
  Conn := TZConnectionProbe.Create(nil);
  Query := TScratchBirdZQuery.Create(nil);
  try
    Query.Connection := Conn;
    Query.SQL.Text := 'SELECT :id::INTEGER AS before_prepare';
    Query.ParamByName('id').Value := 7;

    Query.Prepare;

    Query.SQL.Text := 'SELECT :id::INTEGER AS after_prepare';
    Query.ParamByName('id').Value := 9;
    Query.ExecSQL;

    AssertEqualInt(1, Conn.ExecCalls, 'Zeos exec calls');
    AssertContains('before_prepare', Conn.LastSql, 'Zeos uses prepared SQL snapshot');
    AssertContains('$1::INTEGER', Conn.LastSql, 'Zeos prepared SQL normalization');
    AssertEqualInt(1, Length(Conn.LastParams), 'Zeos prepared param count');
    AssertEqualInt(7, VarAsType(Conn.LastParams[0].Value, varInteger), 'Zeos uses prepared params snapshot');
  finally
    Query.Free;
    Conn.Free;
  end;
end;

procedure TestSQLdbPrepareCachesSqlAndParamsForExec;
var
  Conn: TSQLConnectionProbe;
  Query: TScratchBirdSQLQuery;
begin
  Conn := TSQLConnectionProbe.Create(nil);
  Query := TScratchBirdSQLQuery.Create(nil);
  try
    Query.Connection := Conn;
    Query.SQL.Text := 'SELECT :id::INTEGER AS before_prepare';
    Query.ParamByName('id').Value := 7;

    Query.Prepare;

    Query.SQL.Text := 'SELECT :id::INTEGER AS after_prepare';
    Query.ParamByName('id').Value := 9;
    Query.ExecSQL;

    AssertEqualInt(1, Conn.ExecCalls, 'SQLdb exec calls');
    AssertContains('before_prepare', Conn.LastSql, 'SQLdb uses prepared SQL snapshot');
    AssertContains('$1::INTEGER', Conn.LastSql, 'SQLdb prepared SQL normalization');
    AssertEqualInt(1, Length(Conn.LastParams), 'SQLdb prepared param count');
    AssertEqualInt(7, VarAsType(Conn.LastParams[0].Value, varInteger), 'SQLdb uses prepared params snapshot');
  finally
    Query.Free;
    Conn.Free;
  end;
end;

begin
  try
    TestFireDACPrepareRequiresConnection;
    TestIBXPrepareRequiresDatabase;
    TestZeosPrepareRequiresConnection;
    TestSQLdbPrepareRequiresConnection;
    TestFireDACPrepareCachesSqlAndParamsForExec;
    TestIBXPrepareCachesSqlAndParamsForExec;
    TestZeosPrepareCachesSqlAndParamsForExec;
    TestSQLdbPrepareCachesSqlAndParamsForExec;
    Writeln('AdapterPrepareLifecycleTests: OK');
  except
    on E: Exception do
    begin
      Writeln('AdapterPrepareLifecycleTests: FAILED - ' + E.Message);
      Halt(1);
    end;
  end;
end.
