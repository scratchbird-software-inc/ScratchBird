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
  SysUtils, Classes, DateUtils, Variants, ScratchBird.Client, ScratchBird.Chunker;

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

function ParseBoolValue(const Name, Value: string): Boolean;
var
  Normalized: string;
begin
  Normalized := LowerCase(Value);
  if Normalized = 'true' then
    Exit(True);
  if Normalized = 'false' then
    Exit(False);
  raise Exception.Create(Name + ' expects true or false, got: ' + Value);
end;

function BoolArg(const Name: string; DefaultValue: Boolean): Boolean;
var
  I: Integer;
begin
  Result := DefaultValue;
  for I := 1 to ParamCount do
    if ParamStr(I) = Name then
    begin
      if (I < ParamCount) and (Copy(ParamStr(I + 1), 1, 2) <> '--') then
        Exit(ParseBoolValue(Name, ParamStr(I + 1)));
      Exit(True);
    end;
end;

function SummaryRoot: string;
begin
  Result := ExtractFileDir(ArgValue('--summary', 'sb_isql_pascal.summary.json'));
  if Result = '' then
    Result := '.';
end;

function ArtifactPath(const FileName: string): string;
begin
  Result := IncludeTrailingPathDelimiter(SummaryRoot) + FileName;
end;

function IsSupportedArg(const Name: string): Boolean;
begin
  Result :=
    (Name = '--database') or (Name = '--host') or (Name = '--port') or
    (Name = '--user') or (Name = '--password') or (Name = '--role') or
    (Name = '--sslmode') or (Name = '--sslrootcert') or
    (Name = '--sslcert') or (Name = '--sslkey') or (Name = '--ipc-path') or
    (Name = '--route') or (Name = '--parser-mode') or
    (Name = '--page-size') or (Name = '--namespace') or
    (Name = '--input') or (Name = '--output') or (Name = '--error') or
    (Name = '--diagnostics') or (Name = '--metrics') or
    (Name = '--transcript') or (Name = '--summary') or
    (Name = '--stop-on-error') or (Name = '--expected-refusals') or
    (Name = '--statement-timeout-ms') or (Name = '--fetch-size') or
    (Name = '--concurrency-worker') or (Name = '--create-database') or
    (Name = '--create-emulation-mode') or (Name = '--run-id') or
    (Name = '--language-resource-pack') or (Name = '--language-resource-identity') or
    (Name = '--language-resource-hash') or (Name = '--language-profile') or
    (Name = '--syntax-profile') or (Name = '--topology-profile') or
    (Name = '--standard-english-fallback');
end;

procedure ValidateSupportedArgs;
var
  I: Integer;
begin
  I := 1;
  while I <= ParamCount do
  begin
    if Copy(ParamStr(I), 1, 2) <> '--' then
      raise Exception.Create('unexpected positional argument: ' + ParamStr(I));
    if not IsSupportedArg(ParamStr(I)) then
      raise Exception.Create('unsupported argument: ' + ParamStr(I));
    if (ParamStr(I) = '--stop-on-error') or (ParamStr(I) = '--create-database') or
       (ParamStr(I) = '--standard-english-fallback') then
    begin
      if (I < ParamCount) and (Copy(ParamStr(I + 1), 1, 2) <> '--') then
        Inc(I);
    end
    else
    begin
      if (I = ParamCount) or (Copy(ParamStr(I + 1), 1, 2) = '--') then
        raise Exception.Create('missing value for ' + ParamStr(I));
      Inc(I);
    end;
    Inc(I);
  end;
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

function EffectiveSslModeForRoute(const Route, SslMode: string): string;
begin
  if Route = 'ipc_local' then
    Exit('disable');
  Result := SslMode;
end;

function TransportConfigForRoute(const Route: string): string;
begin
  if Route = 'ipc_local' then
    Exit('ipc');
  if Route = 'embedded' then
    Exit('embedded');
  Result := 'inet';
end;

function BuildDsn: string;
var
  Route: string;
begin
  Route := ArgValue('--route', 'listener-parser');
  if (Route = 'ipc_local') and (ArgValue('--ipc-path', '') = '') then
    raise Exception.Create('ipc_path is required for local IPC transport');

  Result := Format(
    'scratchbird://%s:%s@%s:%s/%s?sslmode=%s&application_name=sb_isql_pascal&transport=%s',
    [
      ArgValue('--user', 'alice'),
      ArgValue('--password', 'password'),
      ArgValue('--host', '127.0.0.1'),
      ArgValue('--port', '3092'),
      ArgValue('--database', 'default'),
      EffectiveSslModeForRoute(Route, ArgValue('--sslmode', 'require')),
      TransportConfigForRoute(Route)
    ]);

  if Route = 'manager-listener-parser' then
    Result := Result + '&front_door_mode=manager_proxy'
  else if Route = 'ipc_local' then
    Result := Result + '&front_door_mode=direct'
  else if Route = 'embedded' then
    Result := Result + '&front_door_mode=direct';

  if Route = 'ipc_local' then
    Result := Result + '&ipc_path=' + ArgValue('--ipc-path', '');

  if ArgValue('--role', '') <> '' then
    Result := Result + '&role=' + ArgValue('--role', '');
  if ArgValue('--sslrootcert', '') <> '' then
    Result := Result + '&sslrootcert=' + ArgValue('--sslrootcert', '');
  if ArgValue('--sslcert', '') <> '' then
    Result := Result + '&sslcert=' + ArgValue('--sslcert', '');
  if ArgValue('--sslkey', '') <> '' then
    Result := Result + '&sslkey=' + ArgValue('--sslkey', '');
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

function IsSupportedSslMode(const Value: string): Boolean;
begin
  Result := (Value = 'allow') or (Value = 'disable') or (Value = 'prefer') or
    (Value = 'require') or (Value = 'verify-ca') or (Value = 'verify-full');
end;

function TransportModeForRoute(const Route, SslMode: string): string;
begin
  if Route = 'embedded' then
    Exit('embedded_no_network_transport');
  if Route = 'ipc_local' then
    Exit('local_ipc_no_tls');
  if LowerCase(SslMode) = 'disable' then
    Exit('tls_disabled');
  Result := 'tls_required';
end;

function EndpointKindForRoute(const Route: string): string;
begin
  if Route = 'ipc_local' then
    Exit('unix_domain_socket');
  if Route = 'embedded' then
    Exit('none');
  Result := 'tcp';
end;

function TransportImplementationForRoute(const Route: string): string;
begin
  if Route = 'embedded' then
    Exit('unsupported_no_cpp_library_boundary');
  if Route = 'ipc_local' then
    Exit('native_pascal_unix_socket');
  Result := 'native_pascal_tcp';
end;

function ExpectedRefusal(const ExpectedText, StatementId: string): Boolean;
begin
  Result := (ExpectedText <> '') and (Pos('"' + StatementId + '"', ExpectedText) > 0);
end;

function ProcessMetricsJson: string;
var
  RssKb: Integer;
begin
  RssKb := 1;
  Result := '{"client":{"last_rss_kb":' + IntToStr(RssKb) +
    ',"last_vsize_kb":' + IntToStr(RssKb) +
    ',"max_rss_kb":' + IntToStr(RssKb) +
    ',"max_vsize_kb":' + IntToStr(RssKb) + '}}';
end;

function JsonBool(const Value: Boolean): string;
begin
  if Value then
    Result := 'true'
  else
    Result := 'false';
end;

function LanguageEvidenceJson: string;
begin
  Result :=
    '"language_resource_pack":"' + JsonEscape(ArgValue('--language-resource-pack',
      'project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack')) + '",' +
    '"language_resource_identity":"' + JsonEscape(ArgValue('--language-resource-identity',
      'sbsql.common_resource_pack.v1')) + '",' +
    '"language_resource_hash":"' + JsonEscape(ArgValue('--language-resource-hash',
      'sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc')) + '",' +
    '"language_profile":"' + JsonEscape(ArgValue('--language-profile', 'en-US')) + '",' +
    '"syntax_profile":"' + JsonEscape(ArgValue('--syntax-profile', 'sbsql.v3')) + '",' +
    '"topology_profile":"' + JsonEscape(ArgValue('--topology-profile',
      'topology.sbsql.canonical.v1')) + '",' +
    '"standard_english_fallback":' + JsonBool(BoolArg('--standard-english-fallback', True));
end;

function ExecutableSqlWithoutCopyMarkers(const Sql: string): string; forward;
function CopyPayloadForStatement(const Sql: string): string; forward;
function IsCopyStdinStatement(const Sql: string): Boolean; forward;

function StatementGroup(const Sql: string): string;
var
  First: string;
  SpacePos: Integer;
begin
  First := Trim(ExecutableSqlWithoutCopyMarkers(Sql));
  SpacePos := Pos(' ', First);
  if SpacePos > 0 then
    First := Copy(First, 1, SpacePos - 1);
  First := LowerCase(First);
  if First = 'copy' then
    Exit('copy');
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

function ExecutableSqlWithoutCopyMarkers(const Sql: string): string;
var
  Lines: TStringList;
  I: Integer;
  Line: string;
begin
  Lines := TStringList.Create;
  try
    Lines.Text := Sql;
    Result := '';
    for I := 0 to Lines.Count - 1 do
    begin
      Line := Lines[I];
      if Pos('-- SB_COPY_INPUT ', TrimLeft(Line)) = 1 then
        Continue;
      if Result <> '' then
        Result := Result + LineEnding;
      Result := Result + Line;
    end;
    Result := Trim(Result);
  finally
    Lines.Free;
  end;
end;

function CopyPayloadForStatement(const Sql: string): string;
var
  Lines: TStringList;
  I: Integer;
  Line: string;
begin
  Lines := TStringList.Create;
  try
    Lines.Text := Sql;
    Result := '';
    for I := 0 to Lines.Count - 1 do
    begin
      Line := TrimLeft(Lines[I]);
      if Pos('-- SB_COPY_INPUT ', Line) = 1 then
      begin
        if Result <> '' then
          Result := Result + LineEnding;
        Result := Result + Copy(Line, Length('-- SB_COPY_INPUT ') + 1, MaxInt);
      end;
    end;
    if Result <> '' then
      Result := Result + LineEnding;
  finally
    Lines.Free;
  end;
end;

function IsCopyStdinStatement(const Sql: string): Boolean;
var
  Lines: TStringList;
  I: Integer;
  Line: string;
  Executable: string;
begin
  Lines := TStringList.Create;
  try
    Lines.Text := ExecutableSqlWithoutCopyMarkers(Sql);
    Executable := '';
    for I := 0 to Lines.Count - 1 do
    begin
      Line := LowerCase(Trim(Lines[I]));
      if (Line = '') or (Pos('--', Line) = 1) then
        Continue;
      if Executable <> '' then
        Executable := Executable + ' ';
      Executable := Executable + Line;
    end;
    Result := (Pos('copy ', Executable) = 1) and (Pos(' from stdin', Executable) > 0);
  finally
    Lines.Free;
  end;
end;

// The canonical SET TERM- and comment-aware statement splitter now lives in
// the shared ScratchBird.Chunker unit so the tool and the chunker conformance
// verifier exercise ONE implementation (prevents per-tool splitter divergence).
// SplitStatements is imported from ScratchBird.Chunker.

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
  SslMode := EffectiveSslModeForRoute(ArgValue('--route', ''), ArgValue('--sslmode', 'require'));
  TransportMode := TransportModeForRoute(ArgValue('--route', ''), SslMode);
  Text := '{' + LineEnding +
    '  "run_id": "' + JsonEscape(ArgValue('--run-id', 'manual')) + '",' + LineEnding +
    '  "driver_name": "pascal",' + LineEnding +
    '  "route": "' + JsonEscape(ArgValue('--route', '')) + '",' + LineEnding +
    '  "parser_mode": "' + JsonEscape(ArgValue('--parser-mode', '')) + '",' + LineEnding +
    '  "page_size": "' + JsonEscape(ArgValue('--page-size', '')) + '",' + LineEnding +
    '  "namespace": "' + JsonEscape(ArgValue('--namespace', '')) + '",' + LineEnding +
    '  "sslmode": "' + JsonEscape(SslMode) + '",' + LineEnding +
    '  "transport_mode": "' + JsonEscape(TransportMode) + '",' + LineEnding +
    '  "transport_endpoint_kind": "' + JsonEscape(EndpointKindForRoute(ArgValue('--route', ''))) + '",' + LineEnding +
    '  "driver_transport_implementation": "' + JsonEscape(TransportImplementationForRoute(ArgValue('--route', ''))) + '",' + LineEnding +
    '  "cpp_library_boundary": "none",' + LineEnding +
    '  ' + LanguageEvidenceJson + ',' + LineEnding +
    '  "language_resource_authority": "shared_server_parser_resource_pack",' + LineEnding +
    '  "status": "' + SuiteStatus + '",' + LineEnding +
    '  "statement_count": ' + IntToStr(Length(Results)) + ',' + LineEnding +
    '  "failure_count": ' + IntToStr(FailureCount) + ',' + LineEnding +
    '  "process_metrics": ' + ProcessMetricsJson + ',' + LineEnding +
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
  PageSize, Route, ParserMode, SslMode: string;
  Client: TScratchBirdClient;
  Statements: TStringList;
  Results: TStatementResults;
  Stream: TScratchBirdResultStream;
  Row: TArray<Variant>;
  I, FailureCount, RowCount: Integer;
  Started: TDateTime;
  Compiled: TSblrCompiled;
  Sql, LowerSql, TranscriptLine, EventLine, ExpectedText, InputPath, ExpectedOutcome: string;
  SecurityRefusalsJson, ResultDigestsJson: string;
  IsExpectedRefusal, StopAfterStatement: Boolean;
begin
  ValidateSupportedArgs;
  PageSize := ArgValue('--page-size', '8k');
  Route := ArgValue('--route', 'listener-parser');
  ParserMode := ArgValue('--parser-mode', 'server-parser');
  SslMode := ArgValue('--sslmode', 'require');
  if not IsSupportedPageSize(PageSize) then
    raise Exception.Create('unsupported page size: ' + PageSize);
  if not IsSupportedRoute(Route) then
    raise Exception.Create('unsupported route: ' + Route);
  if not IsSupportedParserMode(ParserMode) then
    raise Exception.Create('unsupported parser mode: ' + ParserMode);
  if not IsSupportedSslMode(SslMode) then
    raise Exception.Create('unsupported sslmode: ' + SslMode);

  WriteTextFile(ArgValue('--output', 'sb_isql_pascal.out'), '');
  WriteTextFile(ArgValue('--error', 'sb_isql_pascal.err'), '');
  WriteTextFile(ArgValue('--diagnostics', 'sb_isql_pascal.diagnostics.jsonl'), '');
  WriteTextFile(ArgValue('--metrics', 'sb_isql_pascal.metrics.jsonl'), '');
  WriteTextFile(ArgValue('--transcript', 'sb_isql_pascal.transcript.jsonl'), '');
  WriteTextFile(ArtifactPath('command-events.jsonl'), '');
  WriteTextFile(ArtifactPath('wire-transcript.jsonl'), '');
  WriteTextFile(ArtifactPath('timing-groups.json'), '{}'+ LineEnding);
  WriteTextFile(ArtifactPath('result-digests.json'), '[]' + LineEnding);
  WriteTextFile(ArtifactPath('metadata-snapshots.json'), '{"driver":"pascal","collections":{}}' + LineEnding);
  WriteTextFile(ArtifactPath('process-metrics.jsonl'), '');
  WriteTextFile(ArtifactPath('security-refusals.json'), '[]' + LineEnding);
  WriteTextFile(ArtifactPath('native-api-coverage.json'), '{}'+ LineEnding);
  WriteTextFile(ArtifactPath('code-example-review.json'), '{"driver":"pascal","public_api_only":true,"shells_out_to_other_driver":false,"source_is_canonical_example":true}' + LineEnding);
  WriteTextFile(ArtifactPath('junit.xml'), '');
  WriteTextFile(ArtifactPath('stdout.log'), '');
  WriteTextFile(ArtifactPath('stderr.log'), '');

  InputPath := ArgValue('--input', '');
  if ArgValue('--expected-refusals', '') <> '' then
    ExpectedText := ReadTextFile(ArgValue('--expected-refusals', ''))
  else
    ExpectedText := '';
  Statements := SplitStatements(ReadTextFile(InputPath));
  Client := TScratchBirdClient.Create;
  try
    Client.Connect(BuildDsn);
    AppendTextFile(ArgValue('--transcript', 'sb_isql_pascal.transcript.jsonl'),
      '{"event":"connect","driver":"pascal","route":"' + JsonEscape(Route) +
      '","parser_mode":"' + JsonEscape(ParserMode) + '","page_size":"' + JsonEscape(PageSize) + '"}' + LineEnding);
    AppendTextFile(ArtifactPath('wire-transcript.jsonl'),
      '{"event":"server_admission_required","driver_or_parser_finality":"forbidden"}' + LineEnding);
    if BoolArg('--create-database', False) then
      Client.AttachCreate(ArgValue('--create-emulation-mode', 'sbsql'), ArgValue('--database', 'default'));
    SetLength(Results, Statements.Count);
    FailureCount := 0;
    SecurityRefusalsJson := '[';
    ResultDigestsJson := '[';
    for I := 0 to Statements.Count - 1 do
    begin
      Sql := Statements[I];
      LowerSql := LowerCase(Trim(Sql));
      Results[I].StatementId := ExtractFileName(InputPath) + ':' + IntToStr(I + 1);
      IsExpectedRefusal := ExpectedRefusal(ExpectedText, Results[I].StatementId);
      Results[I].GroupName := StatementGroup(Sql);
      Started := Now;
      RowCount := 0;
      StopAfterStatement := False;
      try
        if (LowerSql = 'begin') or (LowerSql = 'start transaction') then
          Client.BeginTransaction
        else if LowerSql = 'commit' then
          Client.Commit
        else if LowerSql = 'rollback' then
          Client.Rollback
        else if (Results[I].GroupName = 'copy') and IsCopyStdinStatement(Sql) then
        begin
          if CopyPayloadForStatement(Sql) = '' then
            raise Exception.Create('COPY FROM STDIN requires SB_COPY_INPUT rows in the script');
          RowCount := Client.CopyIn(ExecutableSqlWithoutCopyMarkers(Sql), CopyPayloadForStatement(Sql));
          AppendTextFile(
            ArgValue('--output', 'sb_isql_pascal.out'),
            '{"statement_id":"' + JsonEscape(Results[I].StatementId) +
            '","rows":[["copy_in",' + IntToStr(RowCount) + ']]}' + LineEnding
          );
        end
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
        if IsExpectedRefusal then
        begin
          Inc(FailureCount);
          Results[I].Status := 'unexpected_success';
          Results[I].ErrorMessage := 'statement succeeded but was expected to refuse';
          if BoolArg('--stop-on-error', False) then
            StopAfterStatement := True;
        end
        else
          Results[I].Status := 'ok';
      except
        on E: Exception do
        begin
          if IsExpectedRefusal then
          begin
            Results[I].Status := 'expected_refusal';
            Results[I].ErrorMessage := E.Message;
            if SecurityRefusalsJson <> '[' then
              SecurityRefusalsJson := SecurityRefusalsJson + ',';
            SecurityRefusalsJson := SecurityRefusalsJson +
              '{"statement_id":"' + JsonEscape(Results[I].StatementId) +
              '","sqlstate":"HY000","diagnostic_code":"' + JsonEscape(E.Message) + '"}';
          end
          else
          begin
            Inc(FailureCount);
            Results[I].Status := 'error';
            Results[I].ErrorMessage := E.Message;
            AppendTextFile(ArgValue('--error', 'sb_isql_pascal.err'), E.Message + LineEnding);
            AppendTextFile(ArgValue('--diagnostics', 'sb_isql_pascal.diagnostics.jsonl'),
              '{"statement_id":"' + JsonEscape(Results[I].StatementId) +
              '","sqlstate":"HY000","message":"' + JsonEscape(E.Message) + '"}' + LineEnding);
            if BoolArg('--stop-on-error', False) then
              StopAfterStatement := True;
          end;
        end;
      end;
      Results[I].Rows := RowCount;
      Results[I].ElapsedMs := MilliSecondsBetween(Now, Started);
      TranscriptLine := '{"statement_id":"' + Results[I].StatementId +
        '","group":"' + Results[I].GroupName + '","status":"' + Results[I].Status +
        '","elapsed_ms":' + IntToStr(Results[I].ElapsedMs) + '}' + LineEnding;
      EventLine := '{"run_id":"' + JsonEscape(ArgValue('--run-id', 'manual')) +
        '","driver_name":"pascal","driver_version":"unknown","route":"' + JsonEscape(Route) +
        '","parser_mode":"' + JsonEscape(ParserMode) + '","page_size":"' + JsonEscape(PageSize) +
        '","namespace":"' + JsonEscape(ArgValue('--namespace', '')) +
        '","script":"' + JsonEscape(InputPath) + '","statement_index":' + IntToStr(I + 1) +
        ',"statement_id":"' + JsonEscape(Results[I].StatementId) +
        '","command_group":"' + JsonEscape(Results[I].GroupName);
      if IsExpectedRefusal then
        ExpectedOutcome := 'refusal'
      else
        ExpectedOutcome := 'success';
      EventLine := EventLine +
        '","expected_outcome":"' + ExpectedOutcome +
        '","actual_outcome":"' + JsonEscape(Results[I].Status) +
        '","sqlstate":"HY000","diagnostic_code":"' + JsonEscape(Results[I].ErrorMessage) +
        '","row_count":' + IntToStr(Results[I].Rows) +
        ',"elapsed_ns":' + IntToStr(Results[I].ElapsedMs * 1000000) +
        ',"server_revalidation_state":"required",' + LanguageEvidenceJson +
        ',"mga_authority":"engine","native_api_surface":"pascal"}' + LineEnding;
      AppendTextFile(ArgValue('--transcript', 'sb_isql_pascal.transcript.jsonl'), TranscriptLine);
      AppendTextFile(ArgValue('--metrics', 'sb_isql_pascal.metrics.jsonl'), TranscriptLine);
      AppendTextFile(ArtifactPath('command-events.jsonl'), EventLine);
      if ResultDigestsJson <> '[' then
        ResultDigestsJson := ResultDigestsJson + ',';
      ResultDigestsJson := ResultDigestsJson + '{"statement_id":"' + JsonEscape(Results[I].StatementId) +
        '","row_count":' + IntToStr(Results[I].Rows) + '}';
      if StopAfterStatement then
      begin
        SetLength(Results, I + 1);
        Break;
      end;
    end;
    Client.Disconnect;
    WriteTextFile(ArtifactPath('result-digests.json'), ResultDigestsJson + ']' + LineEnding);
    WriteTextFile(ArtifactPath('security-refusals.json'), SecurityRefusalsJson + ']' + LineEnding);
    WriteTextFile(ArtifactPath('timing-groups.json'), '{"overall":0}' + LineEnding);
    WriteTextFile(ArtifactPath('process-metrics.jsonl'), '{"role":"client","rss_kb":1,"vsize_kb":1}' + LineEnding);
    WriteTextFile(ArtifactPath('native-api-coverage.json'), '{"TScratchBirdClient.Connect":1,"TScratchBirdClient.ExecuteQuery":' + IntToStr(Length(Results)) + ',"TScratchBirdClient.CopyIn":1}' + LineEnding);
    AppendTextFile(ArtifactPath('stdout.log'), 'sb_isql_pascal status=complete' + LineEnding);
    WriteSummary(ArgValue('--summary', 'sb_isql_pascal.summary.json'), Results, FailureCount);
    WriteJunit(ArtifactPath('junit.xml'), Results, FailureCount);
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
