// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

{ ScratchBird Pascal Driver - OpenTelemetry Telemetry
  Copyright (c) 2025-2026 Dalton Calford }

unit SBTelemetry;

{$mode objfpc}{$H+}

interface

uses
  Classes, SysUtils, DateUtils, Variants
  {$IFNDEF FPC}, SyncObjs{$ENDIF};

{$IFDEF FPC}
type
  TCriticalSection = class
  private
    FSection: TRTLCriticalSection;
  public
    constructor Create;
    destructor Destroy; override;
    procedure Enter;
    procedure Leave;
  end;
{$ENDIF}

type
  TTelemetryConfig = record
    EnableTracing: Boolean;
    EnableMetrics: Boolean;
    EnableSlowQueryLog: Boolean;
    SlowQueryThresholdMs: Cardinal;
    SanitizeQueries: Boolean;
    SampleRate: Double;
  end;
  
  TSpanContext = class
  private
    class function GenerateTraceID: string; static;
    class function GenerateSpanID: string; static;
  public
    TraceID: string;
    SpanID: string;
    ParentSpanID: string;
    SpanName: string;
    StartTime: TDateTime;
    Attributes: TStringList;
    constructor Create(const Name: string); overload;
    constructor Create(const Name: string; Parent: TSpanContext); overload;
    destructor Destroy; override;
    function WithAttribute(const Key, Value: string): TSpanContext;
    function ElapsedMs: Cardinal;
  end;
  
  TLatencyHistogram = class
  private
    FMs0_10: Cardinal;
    FMs10_100: Cardinal;
    FMs100_1000: Cardinal;
    FMs1000_10000: Cardinal;
    FMsOver10000: Cardinal;
    FLock: TCriticalSection;
  public
    constructor Create;
    destructor Destroy; override;
    procedure RecordSample(DurationMs: Cardinal);
    procedure Reset;
    procedure Snapshot(out Ms0_10, Ms10_100, Ms100_1000, Ms1000_10000, MsOver10000: Cardinal);
    function ToString: string;
  end;
  
  TOperationMetrics = class
  private
    FCount: Cardinal;
    FTotalTimeMs: Cardinal;
    FAvgTimeMs: Cardinal;
    FErrorCount: Cardinal;
    FLock: TCriticalSection;
  public
    constructor Create;
    destructor Destroy; override;
    procedure RecordSample(DurationMs: Cardinal; Success: Boolean);
    procedure Snapshot(out Count, TotalTimeMs, AvgTimeMs, ErrorCount: Cardinal);
    function ToString: string;
  end;
  
  TSlowQueryLog = class
  public
    TraceID: string;
    SpanName: string;
    DurationMs: Cardinal;
    Timestamp: TDateTime;
    Attributes: TStringList;
    constructor Create(const ATraceID, ASpanName: string; ADurationMs: Cardinal);
    destructor Destroy; override;
  end;
  
  TTelemetryCollector = class
  private
    FConfig: TTelemetryConfig;
    FSpans: TThreadList;
    FTotalQueries: Cardinal;
    FSuccessfulQueries: Cardinal;
    FFailedQueries: Cardinal;
    FTotalQueryTimeMs: Cardinal;
    FMetricsLock: TCriticalSection;
    FHistogram: TLatencyHistogram;
    FOperationMetrics: TStringList;  // Name -> TOperationMetrics
    FOpMetricsLock: TCriticalSection;
    FSlowQueries: TThreadList;
    procedure RecordQueryMetrics(const Operation: string; DurationMs: Cardinal; Success: Boolean);
    procedure RecordSlowQuery(Span: TSpanContext; DurationMs: Cardinal);
  public
    constructor Create(const Config: TTelemetryConfig);
    destructor Destroy; override;
    function StartSpan(const Name: string): TSpanContext;
    procedure EndSpan(Span: TSpanContext; Success: Boolean);
    function GetTotalQueries: Cardinal;
    function GetSuccessfulQueries: Cardinal;
    function GetFailedQueries: Cardinal;
    function ExportPrometheusMetrics: string;
    function ExportTelemetrySummaryJson: string;
    function ExportSlowQueriesJson: string;
    procedure Reset;
    class function SanitizeQuery(const SQL: string): string;
  end;

function DefaultTelemetryConfig: TTelemetryConfig;

implementation

{$IFDEF FPC}
constructor TCriticalSection.Create;
begin
  inherited Create;
  InitCriticalSection(FSection);
end;

destructor TCriticalSection.Destroy;
begin
  DoneCriticalSection(FSection);
  inherited Destroy;
end;

procedure TCriticalSection.Enter;
begin
  EnterCriticalSection(FSection);
end;

procedure TCriticalSection.Leave;
begin
  LeaveCriticalSection(FSection);
end;
{$ENDIF}

function DefaultTelemetryConfig: TTelemetryConfig;
begin
  Result.EnableTracing := True;
  Result.EnableMetrics := True;
  Result.EnableSlowQueryLog := True;
  Result.SlowQueryThresholdMs := 1000;
  Result.SanitizeQueries := True;
  Result.SampleRate := 1.0;
end;

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

{ TSpanContext }

constructor TSpanContext.Create(const Name: string);
begin
  TraceID := TSpanContext.GenerateTraceID;
  SpanID := TSpanContext.GenerateSpanID;
  ParentSpanID := '';
  SpanName := Name;
  StartTime := Now;
  Attributes := TStringList.Create;
end;

constructor TSpanContext.Create(const Name: string; Parent: TSpanContext);
begin
  TraceID := Parent.TraceID;
  SpanID := TSpanContext.GenerateSpanID;
  ParentSpanID := Parent.SpanID;
  SpanName := Name;
  StartTime := Now;
  Attributes := TStringList.Create;
end;

destructor TSpanContext.Destroy;
begin
  Attributes.Free;
  inherited Destroy;
end;

function TSpanContext.WithAttribute(const Key, Value: string): TSpanContext;
begin
  Attributes.Values[Key] := Value;
  Result := Self;
end;

function TSpanContext.ElapsedMs: Cardinal;
begin
  Result := Cardinal(MilliSecondsBetween(Now, StartTime));
end;

class function TSpanContext.GenerateTraceID: string;
var
  GUID: TGUID;
begin
  CreateGUID(GUID);
  Result := GUIDToString(GUID);
  Result := StringReplace(Result, '{', '', [rfReplaceAll]);
  Result := StringReplace(Result, '}', '', [rfReplaceAll]);
  Result := StringReplace(Result, '-', '', [rfReplaceAll]);
end;

class function TSpanContext.GenerateSpanID: string;
var
  GUID: TGUID;
begin
  CreateGUID(GUID);
  Result := Copy(GUIDToString(GUID), 1, 18);  // First 16 chars
end;

{ TLatencyHistogram }

constructor TLatencyHistogram.Create;
begin
  FMs0_10 := 0;
  FMs10_100 := 0;
  FMs100_1000 := 0;
  FMs1000_10000 := 0;
  FMsOver10000 := 0;
  FLock := TCriticalSection.Create;
end;

destructor TLatencyHistogram.Destroy;
begin
  FLock.Free;
  inherited Destroy;
end;

procedure TLatencyHistogram.RecordSample(DurationMs: Cardinal);
begin
  FLock.Enter;
  try
    if DurationMs <= 10 then
      Inc(FMs0_10)
    else if DurationMs <= 100 then
      Inc(FMs10_100)
    else if DurationMs <= 1000 then
      Inc(FMs100_1000)
    else if DurationMs <= 10000 then
      Inc(FMs1000_10000)
    else
      Inc(FMsOver10000);
  finally
    FLock.Leave;
  end;
end;

procedure TLatencyHistogram.Reset;
begin
  FLock.Enter;
  try
    FMs0_10 := 0;
    FMs10_100 := 0;
    FMs100_1000 := 0;
    FMs1000_10000 := 0;
    FMsOver10000 := 0;
  finally
    FLock.Leave;
  end;
end;

procedure TLatencyHistogram.Snapshot(out Ms0_10, Ms10_100, Ms100_1000, Ms1000_10000, MsOver10000: Cardinal);
begin
  FLock.Enter;
  try
    Ms0_10 := FMs0_10;
    Ms10_100 := FMs10_100;
    Ms100_1000 := FMs100_1000;
    Ms1000_10000 := FMs1000_10000;
    MsOver10000 := FMsOver10000;
  finally
    FLock.Leave;
  end;
end;

function TLatencyHistogram.ToString: string;
begin
  FLock.Enter;
  try
    Result := Format('0-10ms: %d, 10-100ms: %d, 100-1000ms: %d, 1000-10000ms: %d, >10000ms: %d',
      [FMs0_10, FMs10_100, FMs100_1000, FMs1000_10000, FMsOver10000]);
  finally
    FLock.Leave;
  end;
end;

{ TOperationMetrics }

constructor TOperationMetrics.Create;
begin
  FCount := 0;
  FTotalTimeMs := 0;
  FAvgTimeMs := 0;
  FErrorCount := 0;
  FLock := TCriticalSection.Create;
end;

destructor TOperationMetrics.Destroy;
begin
  FLock.Free;
  inherited Destroy;
end;

procedure TOperationMetrics.RecordSample(DurationMs: Cardinal; Success: Boolean);
begin
  FLock.Enter;
  try
    Inc(FCount);
    FTotalTimeMs := FTotalTimeMs + DurationMs;
    FAvgTimeMs := FTotalTimeMs div FCount;
    if not Success then
      Inc(FErrorCount);
  finally
    FLock.Leave;
  end;
end;

procedure TOperationMetrics.Snapshot(out Count, TotalTimeMs, AvgTimeMs, ErrorCount: Cardinal);
begin
  FLock.Enter;
  try
    Count := FCount;
    TotalTimeMs := FTotalTimeMs;
    AvgTimeMs := FAvgTimeMs;
    ErrorCount := FErrorCount;
  finally
    FLock.Leave;
  end;
end;

function TOperationMetrics.ToString: string;
begin
  FLock.Enter;
  try
    Result := Format('Count: %d, Avg: %dms, Errors: %d', [FCount, FAvgTimeMs, FErrorCount]);
  finally
    FLock.Leave;
  end;
end;

{ TSlowQueryLog }

constructor TSlowQueryLog.Create(const ATraceID, ASpanName: string; ADurationMs: Cardinal);
begin
  TraceID := ATraceID;
  SpanName := ASpanName;
  DurationMs := ADurationMs;
  Timestamp := Now;
  Attributes := TStringList.Create;
end;

destructor TSlowQueryLog.Destroy;
begin
  Attributes.Free;
  inherited Destroy;
end;

{ TTelemetryCollector }

constructor TTelemetryCollector.Create(const Config: TTelemetryConfig);
begin
  FConfig := Config;
  FSpans := TThreadList.Create;
  FTotalQueries := 0;
  FSuccessfulQueries := 0;
  FFailedQueries := 0;
  FTotalQueryTimeMs := 0;
  FMetricsLock := TCriticalSection.Create;
  FHistogram := TLatencyHistogram.Create;
  FOperationMetrics := TStringList.Create;
  FOpMetricsLock := TCriticalSection.Create;
  FSlowQueries := TThreadList.Create;
end;

destructor TTelemetryCollector.Destroy;
begin
  FSpans.Free;
  FMetricsLock.Free;
  FHistogram.Free;
  FOperationMetrics.Free;
  FOpMetricsLock.Free;
  FSlowQueries.Free;
  inherited Destroy;
end;

function TTelemetryCollector.StartSpan(const Name: string): TSpanContext;
var
  Span: TSpanContext;
  Spans: TList;
begin
  if not FConfig.EnableTracing then
  begin
    Result := nil;
    Exit;
  end;
  
  Span := TSpanContext.Create(Name);
  FSpans.Add(Span);
  
  // Keep only last 1000 spans
  Spans := FSpans.LockList;
  try
    if Spans.Count > 1000 then
    begin
      TSpanContext(Spans[0]).Free;
      Spans.Delete(0);
    end;
  finally
    FSpans.UnlockList;
  end;
  
  Result := Span;
end;

procedure TTelemetryCollector.EndSpan(Span: TSpanContext; Success: Boolean);
var
  DurationMs: Cardinal;
begin
  if (Span = nil) or not FConfig.EnableTracing then
    Exit;
  
  DurationMs := Span.ElapsedMs;
  RecordQueryMetrics(Span.SpanName, DurationMs, Success);
  
  if FConfig.EnableSlowQueryLog and (DurationMs > FConfig.SlowQueryThresholdMs) then
    RecordSlowQuery(Span, DurationMs);
end;

procedure TTelemetryCollector.RecordQueryMetrics(const Operation: string; DurationMs: Cardinal; Success: Boolean);
var
  Metrics: TOperationMetrics;
  Index: Integer;
begin
  if not FConfig.EnableMetrics then
    Exit;
  
  FMetricsLock.Enter;
  try
    Inc(FTotalQueries);
    if Success then
      Inc(FSuccessfulQueries)
    else
      Inc(FFailedQueries);
    FTotalQueryTimeMs := FTotalQueryTimeMs + DurationMs;
  finally
    FMetricsLock.Leave;
  end;
  
  FHistogram.RecordSample(DurationMs);
  
  // Per-operation metrics
  FOpMetricsLock.Enter;
  try
    Index := FOperationMetrics.IndexOf(Operation);
    if Index < 0 then
    begin
      Metrics := TOperationMetrics.Create;
      FOperationMetrics.AddObject(Operation, Metrics);
    end
    else
      Metrics := TOperationMetrics(FOperationMetrics.Objects[Index]);
    
    Metrics.RecordSample(DurationMs, Success);
  finally
    FOpMetricsLock.Leave;
  end;
end;

procedure TTelemetryCollector.RecordSlowQuery(Span: TSpanContext; DurationMs: Cardinal);
var
  Log: TSlowQueryLog;
  Logs: TList;
begin
  Log := TSlowQueryLog.Create(Span.TraceID, Span.SpanName, DurationMs);
  FSlowQueries.Add(Log);
  
  Logs := FSlowQueries.LockList;
  try
    if Logs.Count > 100 then
    begin
      TSlowQueryLog(Logs[0]).Free;
      Logs.Delete(0);
    end;
  finally
    FSlowQueries.UnlockList;
  end;
end;

function TTelemetryCollector.GetTotalQueries: Cardinal;
begin
  FMetricsLock.Enter;
  try
    Result := FTotalQueries;
  finally
    FMetricsLock.Leave;
  end;
end;

function TTelemetryCollector.GetSuccessfulQueries: Cardinal;
begin
  FMetricsLock.Enter;
  try
    Result := FSuccessfulQueries;
  finally
    FMetricsLock.Leave;
  end;
end;

function TTelemetryCollector.GetFailedQueries: Cardinal;
begin
  FMetricsLock.Enter;
  try
    Result := FFailedQueries;
  finally
    FMetricsLock.Leave;
  end;
end;

function TTelemetryCollector.ExportPrometheusMetrics: string;
var
  Total: Cardinal;
begin
  Total := GetTotalQueries;
  
  Result := '# HELP scratchbird_queries_total Total number of queries' + LineEnding +
            '# TYPE scratchbird_queries_total counter' + LineEnding +
            Format('scratchbird_queries_total %d', [Total]) + LineEnding +
            '# HELP scratchbird_query_duration_ms Query duration histogram' + LineEnding +
            '# TYPE scratchbird_query_duration_ms histogram' + LineEnding +
            FHistogram.ToString;
end;

function TTelemetryCollector.ExportTelemetrySummaryJson: string;
var
  TotalQueries, TotalSuccesses, TotalFailures, TotalDuration: Cardinal;
  H0_10, H10_100, H100_1000, H1000_10000, HOver10000: Cardinal;
  I: Integer;
  OperationName: string;
  Metrics: TOperationMetrics;
  Count, TotalTimeMs, AvgTimeMs, ErrorCount: Cardinal;
  OperationsJson: string;
begin
  FMetricsLock.Enter;
  try
    TotalQueries := FTotalQueries;
    TotalSuccesses := FSuccessfulQueries;
    TotalFailures := FFailedQueries;
    TotalDuration := FTotalQueryTimeMs;
  finally
    FMetricsLock.Leave;
  end;

  FHistogram.Snapshot(H0_10, H10_100, H100_1000, H1000_10000, HOver10000);

  OperationsJson := '';
  FOpMetricsLock.Enter;
  try
    for I := 0 to FOperationMetrics.Count - 1 do
    begin
      OperationName := FOperationMetrics[I];
      Metrics := TOperationMetrics(FOperationMetrics.Objects[I]);
      if Metrics = nil then
        Continue;
      Metrics.Snapshot(Count, TotalTimeMs, AvgTimeMs, ErrorCount);
      if OperationsJson <> '' then
        OperationsJson := OperationsJson + ',';
      OperationsJson := OperationsJson + '{' +
        '"operation":"' + JsonEscape(OperationName) + '",' +
        '"invocations":' + IntToStr(Count) + ',' +
        '"successes":' + IntToStr(Count - ErrorCount) + ',' +
        '"failures":' + IntToStr(ErrorCount) + ',' +
        '"total_duration_ms":' + IntToStr(TotalTimeMs) + ',' +
        '"average_duration_ms":' + IntToStr(AvgTimeMs) +
        '}';
    end;
  finally
    FOpMetricsLock.Leave;
  end;

  Result := '{' +
    '"total_invocations":' + IntToStr(TotalQueries) + ',' +
    '"total_successes":' + IntToStr(TotalSuccesses) + ',' +
    '"total_failures":' + IntToStr(TotalFailures) + ',' +
    '"total_duration_ms":' + IntToStr(TotalDuration) + ',' +
    '"operations":[' + OperationsJson + '],' +
    '"histogram":{' +
      '"ms_0_10":' + IntToStr(H0_10) + ',' +
      '"ms_10_100":' + IntToStr(H10_100) + ',' +
      '"ms_100_1000":' + IntToStr(H100_1000) + ',' +
      '"ms_1000_10000":' + IntToStr(H1000_10000) + ',' +
      '"ms_over_10000":' + IntToStr(HOver10000) +
    '}' +
  '}';
end;

function TTelemetryCollector.ExportSlowQueriesJson: string;
var
  Logs: TList;
  I: Integer;
  Log: TSlowQueryLog;
  Items: string;
begin
  Items := '';
  Logs := FSlowQueries.LockList;
  try
    for I := 0 to Logs.Count - 1 do
    begin
      Log := TSlowQueryLog(Logs[I]);
      if Log = nil then
        Continue;
      if Items <> '' then
        Items := Items + ',';
      Items := Items + '{' +
        '"trace_id":"' + JsonEscape(Log.TraceID) + '",' +
        '"operation":"' + JsonEscape(Log.SpanName) + '",' +
        '"duration_ms":' + IntToStr(Log.DurationMs) + ',' +
        '"captured_unix_ms":' + IntToStr(DateTimeToUnix(Log.Timestamp, False) * 1000) +
      '}';
    end;
  finally
    FSlowQueries.UnlockList;
  end;
  Result := '[' + Items + ']';
end;

procedure TTelemetryCollector.Reset;
var
  Spans: TList;
  Logs: TList;
  I: Integer;
begin
  FMetricsLock.Enter;
  try
    FTotalQueries := 0;
    FSuccessfulQueries := 0;
    FFailedQueries := 0;
    FTotalQueryTimeMs := 0;
  finally
    FMetricsLock.Leave;
  end;

  FHistogram.Reset;

  FOpMetricsLock.Enter;
  try
    for I := 0 to FOperationMetrics.Count - 1 do
      FOperationMetrics.Objects[I].Free;
    FOperationMetrics.Clear;
  finally
    FOpMetricsLock.Leave;
  end;

  Logs := FSlowQueries.LockList;
  try
    for I := Logs.Count - 1 downto 0 do
      TObject(Logs[I]).Free;
    Logs.Clear;
  finally
    FSlowQueries.UnlockList;
  end;

  Spans := FSpans.LockList;
  try
    for I := Spans.Count - 1 downto 0 do
      TObject(Spans[I]).Free;
    Spans.Clear;
  finally
    FSpans.UnlockList;
  end;
end;

class function TTelemetryCollector.SanitizeQuery(const SQL: string): string;
begin
  // Simple sanitization - replace quoted strings
  Result := StringReplace(SQL, '''', '''?''', [rfReplaceAll]);
end;

end.
