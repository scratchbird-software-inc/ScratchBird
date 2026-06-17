// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

program sb_isql_pascal;

{$mode delphi}
{$H+}

uses
  SysUtils, Classes, DateUtils, Variants, ScratchBird.Client;

type
  TStatementResult = record
    StatementId: string;
    GroupName: string;
    Status: string;
    Rows: Integer;
    ElapsedMs: Int64;
    ErrorMessage: string;
  end;

  TStatementResults = array of TStatementResult;

function JsonEscape(const Value: string): string;
var
  I: Integer;
  Ch: Char;
begin
  Result := '';
  for I := 1 to Length(Value) do
  begin
    Ch := Value[I];
    case Ch of
      '\': Result := Result + '\\';
      '"': Result := Result + '\"';
      #8: Result := Result + '\b';
      #9: Result := Result + '\t';
      #10: Result := Result + '\n';
      #12: Result := Result + '\f';
      #13: Result := Result + '\r';
    else
      Result := Result + Ch;
    end;
  end;
end;

function ArgValue(const Name, DefaultValue: string): string;
var
  I: Integer;
begin
  Result := DefaultValue;
  for I := 1 to ParamCount - 1 do
    if ParamStr(I) = Name then
      Exit(ParamStr(I + 1));
end;

function HasArg(const Name: string): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if ParamStr(I) = Name then
      Exit(True);
end;

procedure WriteTextFile(const Path, Text: string);
var
  Lines: TStringList;
begin
  if Path = '' then
    Exit;
  if ExtractFileDir(Path) <> '' then
    ForceDirectories(ExtractFileDir(Path));
  Lines := TStringList.Create;
  try
    Lines.Text := Text;
    Lines.SaveToFile(Path);
  finally
    Lines.Free;
  end;
end;

procedure AppendTextFile(const Path, Text: string);
var
  Existing: TStringList;
begin
  if Path = '' then
    Exit;
  if ExtractFileDir(Path) <> '' then
    ForceDirectories(ExtractFileDir(Path));
  Existing := TStringList.Create;
  try
    if FileExists(Path) then
      Existing.LoadFromFile(Path);
    Existing.Text := Existing.Text + Text;
    Existing.SaveToFile(Path);
  finally
    Existing.Free;
  end;
end;

function ReadTextFile(const Path: string): string;
var
  Lines: TStringList;
begin
  Lines := TStringList.Create;
  try
    Lines.LoadFromFile(Path);
    Result := Lines.Text;
  finally
    Lines.Free;
  end;
end;

function BuildDsn: string;
var
  Route: string;
begin
  Result := Format(
    'scratchbird://%s:%s@%s:%s/%s?sslmode=%s&application_name=sb_isql_pascal',
    [
      ArgValue('--user', 'alice'),
      ArgValue('--password', 'password'),
      ArgValue('--host', '127.0.0.1'),
      ArgValue('--port', '3092'),
      ArgValue('--database', 'default'),
      ArgValue('--sslmode', 'require')
    ]);

  Route := ArgValue('--route', 'listener-parser');
  if Route = 'manager-listener-parser' then
    Result := Result + '&front_door_mode=manager_proxy'
  else if Route = 'ipc_local' then
    Result := Result + '&front_door_mode=direct'
  else if Route = 'embedded' then
    Result := Result + '&front_door_mode=direct';

  if ArgValue('--role', '') <> '' then
    Result := Result + '&role=' + ArgValue('--role', '');
end;

function IsSupportedPageSize(const Value: string): Boolean;
begin
  Result := (Value = '4k') or (Value = '8k') or (Value = '16k') or
    (Value = '32k') or (Value = '64k') or (Value = '128k');
end;

function IsSupportedRoute(const Value: string): Boolean;
begin
  Result := (Value = 'embedded') or (Value = 'ipc_local') or
    (Value = 'listener-parser') or (Value = 'manager-listener-parser');
end;

function IsSupportedParserMode(const Value: string): Boolean;
begin
  Result := (Value = 'server-parser') or (Value = 'standalone-parser') or
    (Value = 'driver-sblr-uuid');
end;

function StatementGroup(const Sql: string): string;
var
  First: string;
  SpacePos: Integer;
begin
  First := Trim(Sql);
  SpacePos := Pos(' ', First);
  if SpacePos > 0 then
    First := Copy(First, 1, SpacePos - 1);
  First := LowerCase(First);
  if (First = 'create') or (First = 'alter') or (First = 'drop') then
    Exit('ddl');
  if (First = 'insert') or (First = 'update') or (First = 'delete') or (First = 'merge') then
    Exit('dml');
  if (First = 'commit') or (First = 'rollback') or (First = 'savepoint') or (First = 'begin') then
    Exit('transaction');
  if (First = 'select') or (First = 'with') or (First = 'values') then
    Exit('query');
  if (First = 'grant') or (First = 'revoke') then
    Exit('security_refusal');
  Result := 'query';
end;

function SplitStatements(const Script: string): TStringList;
var
  I: Integer;
  Current: string;
  Ch: Char;
begin
  Result := TStringList.Create;
  Current := '';
  for I := 1 to Length(Script) do
  begin
    Ch := Script[I];
    if Ch = ';' then
    begin
      if Trim(Current) <> '' then
        Result.Add(Trim(Current));
      Current := '';
    end
    else
      Current := Current + Ch;
  end;
  if Trim(Current) <> '' then
    Result.Add(Trim(Current));
end;

function VariantRowDigest(const Row: array of Variant): string;
var
  I: Integer;
begin
  Result := '';
  for I := 0 to High(Row) do
  begin
    if I > 0 then
      Result := Result + '|';
    if VarIsNull(Row[I]) then
      Result := Result + '<null>'
    else
      Result := Result + VarToStr(Row[I]);
  end;
end;

procedure WriteSummary(const Path: string; const Results: TStatementResults; FailureCount: Integer);
var
  I: Integer;
  Text, SuiteStatus, SslMode, TransportMode: string;
begin
  if FailureCount = 0 then
    SuiteStatus := 'pass'
  else
    SuiteStatus := 'fail';
  SslMode := ArgValue('--sslmode', 'require');
  if LowerCase(SslMode) = 'disable' then
    TransportMode := 'tls_disabled'
  else
    TransportMode := 'tls_required';
  Text := '{' + LineEnding +
    '  "run_id": "' + JsonEscape(ArgValue('--run-id', 'manual')) + '",' + LineEnding +
    '  "driver_name": "pascal",' + LineEnding +
    '  "route": "' + JsonEscape(ArgValue('--route', '')) + '",' + LineEnding +
    '  "parser_mode": "' + JsonEscape(ArgValue('--parser-mode', '')) + '",' + LineEnding +
    '  "page_size": "' + JsonEscape(ArgValue('--page-size', '')) + '",' + LineEnding +
    '  "namespace": "' + JsonEscape(ArgValue('--namespace', '')) + '",' + LineEnding +
    '  "sslmode": "' + JsonEscape(SslMode) + '",' + LineEnding +
    '  "transport_mode": "' + JsonEscape(TransportMode) + '",' + LineEnding +
    '  "status": "' + SuiteStatus + '",' + LineEnding +
    '  "statement_count": ' + IntToStr(Length(Results)) + ',' + LineEnding +
    '  "failure_count": ' + IntToStr(FailureCount) + ',' + LineEnding +
    '  "server_revalidation_required": true,' + LineEnding +
    '  "driver_or_parser_finality": "forbidden",' + LineEnding +
    '  "mga_authority": "engine",' + LineEnding +
    '  "results": [' + LineEnding;
  for I := 0 to High(Results) do
  begin
    Text := Text + '    {"statement_id": "' + JsonEscape(Results[I].StatementId) +
      '", "group": "' + JsonEscape(Results[I].GroupName) +
      '", "status": "' + JsonEscape(Results[I].Status) +
      '", "rows": ' + IntToStr(Results[I].Rows) +
      ', "elapsed_ms": ' + IntToStr(Results[I].ElapsedMs) +
      ', "error": "' + JsonEscape(Results[I].ErrorMessage) + '"}';
    if I < High(Results) then
      Text := Text + ',';
    Text := Text + LineEnding;
  end;
  Text := Text + '  ]' + LineEnding + '}' + LineEnding;
  WriteTextFile(Path, Text);
end;

procedure WriteJunit(const Path: string; const Results: TStatementResults; FailureCount: Integer);
var
  I: Integer;
  Text: string;
begin
  Text := '<?xml version="1.0" encoding="UTF-8"?>' + LineEnding +
    '<testsuite name="sb_isql_pascal" tests="' + IntToStr(Length(Results)) +
    '" failures="' + IntToStr(FailureCount) + '">' + LineEnding;
  for I := 0 to High(Results) do
  begin
    Text := Text + '  <testcase classname="scratchbird.pascal.driver" name="' +
      JsonEscape(Results[I].StatementId) + '" time="' +
      FormatFloat('0.000', Results[I].ElapsedMs / 1000.0) + '">';
    if Results[I].Status <> 'ok' then
      Text := Text + '<failure message="' + JsonEscape(Results[I].ErrorMessage) +
        '">' + JsonEscape(Results[I].ErrorMessage) + '</failure>';
    Text := Text + '</testcase>' + LineEnding;
  end;
  Text := Text + '</testsuite>' + LineEnding;
  WriteTextFile(Path, Text);
end;

function Run: Integer;
var
  PageSize, Route, ParserMode: string;
  Client: TScratchBirdClient;
  Statements: TStringList;
  Results: TStatementResults;
  Stream: TScratchBirdResultStream;
  Row: TArray<Variant>;
  I, FailureCount, RowCount: Integer;
  Started: TDateTime;
  Compiled: TSblrCompiled;
  Sql, LowerSql, TranscriptLine: string;
begin
  PageSize := ArgValue('--page-size', '8k');
  Route := ArgValue('--route', 'listener-parser');
  ParserMode := ArgValue('--parser-mode', 'server-parser');
  if not IsSupportedPageSize(PageSize) then
    raise Exception.Create('unsupported page size: ' + PageSize);
  if not IsSupportedRoute(Route) then
    raise Exception.Create('unsupported route: ' + Route);
  if not IsSupportedParserMode(ParserMode) then
    raise Exception.Create('unsupported parser mode: ' + ParserMode);
  if HasArg('--create-database') then
    raise Exception.Create('Pascal runner requires a pre-created ScratchBird database for this lane.');

  WriteTextFile(ArgValue('--output', 'sb_isql_pascal.out'), '');
  WriteTextFile(ArgValue('--error', 'sb_isql_pascal.err'), '');
  WriteTextFile(ArgValue('--diagnostics', 'sb_isql_pascal.diagnostics.jsonl'), '');
  WriteTextFile(ArgValue('--metrics', 'sb_isql_pascal.metrics.jsonl'), '');
  WriteTextFile(ArgValue('--transcript', 'sb_isql_pascal.transcript.jsonl'), '');

  Statements := SplitStatements(ReadTextFile(ArgValue('--input', '')));
  Client := TScratchBirdClient.Create;
  try
    Client.Connect(BuildDsn);
    SetLength(Results, Statements.Count);
    FailureCount := 0;
    for I := 0 to Statements.Count - 1 do
    begin
      Sql := Statements[I];
      LowerSql := LowerCase(Trim(Sql));
      Results[I].StatementId := 'statement_' + IntToStr(I + 1);
      Results[I].GroupName := StatementGroup(Sql);
      Started := Now;
      RowCount := 0;
      try
        if (LowerSql = 'begin') or (LowerSql = 'start transaction') then
          Client.BeginTransaction
        else if LowerSql = 'commit' then
          Client.Commit
        else if LowerSql = 'rollback' then
          Client.Rollback
        else
        begin
          Stream := Client.ExecuteQuery(Sql);
          try
            while True do
            begin
              Row := Stream.ReadRow;
              if Length(Row) = 0 then
                Break;
              Inc(RowCount);
              AppendTextFile(ArgValue('--output', 'sb_isql_pascal.out'), VariantRowDigest(Row) + LineEnding);
            end;
          finally
            Stream.Free;
          end;
        end;
        if ParserMode = 'driver-sblr-uuid' then
          if Client.GetLastSblr(Compiled) then
            AppendTextFile(ArgValue('--diagnostics', 'sb_isql_pascal.diagnostics.jsonl'),
              '{"event":"sblr_seen","hash":' + IntToStr(Compiled.Hash) + '}' + LineEnding);
        Results[I].Status := 'ok';
      except
        on E: Exception do
        begin
          Inc(FailureCount);
          Results[I].Status := 'error';
          Results[I].ErrorMessage := E.Message;
          AppendTextFile(ArgValue('--error', 'sb_isql_pascal.err'), E.Message + LineEnding);
        end;
      end;
      Results[I].Rows := RowCount;
      Results[I].ElapsedMs := MilliSecondsBetween(Now, Started);
      TranscriptLine := '{"statement_id":"' + Results[I].StatementId +
        '","group":"' + Results[I].GroupName + '","status":"' + Results[I].Status +
        '","elapsed_ms":' + IntToStr(Results[I].ElapsedMs) + '}' + LineEnding;
      AppendTextFile(ArgValue('--transcript', 'sb_isql_pascal.transcript.jsonl'), TranscriptLine);
      AppendTextFile(ArgValue('--metrics', 'sb_isql_pascal.metrics.jsonl'), TranscriptLine);
    end;
    Client.Disconnect;
    WriteSummary(ArgValue('--summary', 'sb_isql_pascal.summary.json'), Results, FailureCount);
    WriteJunit(ExtractFileDir(ArgValue('--summary', 'sb_isql_pascal.summary.json')) + DirectorySeparator + 'junit.xml', Results, FailureCount);
    if FailureCount = 0 then
      Result := 0
    else
      Result := 1;
  finally
    Client.Free;
    Statements.Free;
  end;
end;

begin
  try
    Halt(Run);
  except
    on E: Exception do
    begin
      WriteLn(StdErr, E.Message);
      Halt(2);
    end;
  end;
end.
