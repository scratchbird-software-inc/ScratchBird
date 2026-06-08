// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

{ ScratchBird Pascal Driver - Query Pipelining
  Copyright (c) 2025-2026 Dalton Calford }

unit SBPipeline;

{$mode objfpc}{$H+}

interface

uses
  Classes, SysUtils, DateUtils
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
  TPipelineConfig = record
    MaxInFlight: Cardinal;
    AutoFlush: Boolean;
    AutoFlushThreshold: Cardinal;
    FlushTimeoutMs: Cardinal;
  end;
  
  TPipelinedRequest = class
  public
    SQL: string;
    Params: array of Variant;
    ResponseEvent: TEvent;
    ErrorEvent: TEvent;
    ResponseData: Variant;
    ErrorData: Exception;
    constructor Create(const ASQL: string; const AParams: array of Variant);
    destructor Destroy; override;
  end;
  
  TQueryPipeline = class(TThread)
  private
    FConfig: TPipelineConfig;
    FQueue: TThreadList;
    FInFlight: Integer;
    FRunning: Boolean;
    FConnection: TObject;  // Generic connection reference
    FLock: TCriticalSection;
    procedure ProcessBatch(const Batch: TList);
    function ExecuteRequest(Request: TPipelinedRequest): Boolean;
  protected
    procedure Execute; override;
  public
    constructor Create(const Config: TPipelineConfig);
    destructor Destroy; override;
    procedure Start(Connection: TObject);
    procedure Stop;
    function Queue(const SQL: string; const Params: array of Variant): TPipelinedRequest;
    function PendingCount: Integer;
    function InFlightCount: Integer;
    function HasCapacity: Boolean;
    procedure Flush;
  end;
  
  TPipelineBuilder = class
  private
    FQueries: TStringList;
  public
    constructor Create;
    destructor Destroy; override;
    function Add(const SQL: string): TPipelineBuilder;
    function Build: TStringList;
  end;

function DefaultPipelineConfig: TPipelineConfig;

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

function DefaultPipelineConfig: TPipelineConfig;
begin
  Result.MaxInFlight := 100;
  Result.AutoFlush := True;
  Result.AutoFlushThreshold := 10;
  Result.FlushTimeoutMs := 5000;
end;

{ TPipelinedRequest }

constructor TPipelinedRequest.Create(const ASQL: string; const AParams: array of Variant);
var
  I: Integer;
begin
  SQL := ASQL;
  SetLength(Params, Length(AParams));
  for I := 0 to High(AParams) do
    Params[I] := AParams[I];
  ResponseEvent := TEvent.Create(nil, False, False, '');
  ErrorEvent := TEvent.Create(nil, False, False, '');
end;

destructor TPipelinedRequest.Destroy;
begin
  ResponseEvent.Free;
  ErrorEvent.Free;
  inherited Destroy;
end;

{ TQueryPipeline }

constructor TQueryPipeline.Create(const Config: TPipelineConfig);
begin
  inherited Create(True);
  FConfig := Config;
  FQueue := TThreadList.Create;
  FInFlight := 0;
  FRunning := False;
  FLock := TCriticalSection.Create;
end;

destructor TQueryPipeline.Destroy;
begin
  Stop;
  FQueue.Free;
  FLock.Free;
  inherited Destroy;
end;

procedure TQueryPipeline.Start(Connection: TObject);
begin
  FConnection := Connection;
  FRunning := True;
  inherited Start;
end;

procedure TQueryPipeline.Stop;
begin
  FRunning := False;
  WaitFor;
end;

function TQueryPipeline.Queue(const SQL: string; const Params: array of Variant): TPipelinedRequest;
begin
  FLock.Enter;
  try
    if FInFlight >= Integer(FConfig.MaxInFlight) then
      raise Exception.Create('Pipeline at capacity');
  finally
    FLock.Leave;
  end;
  
  Result := TPipelinedRequest.Create(SQL, Params);
  FQueue.Add(Result);
  
  // Auto-flush
  if FConfig.AutoFlush and (FQueue.Count >= Integer(FConfig.AutoFlushThreshold)) then
    Flush;
end;

function TQueryPipeline.PendingCount: Integer;
begin
  Result := FQueue.Count;
end;

function TQueryPipeline.InFlightCount: Integer;
begin
  FLock.Enter;
  try
    Result := FInFlight;
  finally
    FLock.Leave;
  end;
end;

function TQueryPipeline.HasCapacity: Boolean;
begin
  Result := InFlightCount < Integer(FConfig.MaxInFlight);
end;

procedure TQueryPipeline.Flush;
begin
  // Signal to process immediately
  // In this implementation, we let the worker thread handle it
end;

procedure TQueryPipeline.Execute;
var
  List: TList;
  Batch: TList;
  I: Integer;
  MaxBatch: Integer;
begin
  while FRunning and not Terminated do
  begin
    List := FQueue.LockList;
    try
      if List.Count = 0 then
      begin
        FQueue.UnlockList;
        Sleep(10);
        Continue;
      end;
      
      // Create batch
      Batch := TList.Create;
      MaxBatch := Integer(FConfig.AutoFlushThreshold);
      
      for I := 0 to Min(List.Count, MaxBatch) - 1 do
        Batch.Add(List[I]);
      
      // Remove from queue
      for I := Batch.Count - 1 downto 0 do
        List.Delete(0);
        
    finally
      FQueue.UnlockList;
    end;
    
    if Batch.Count > 0 then
      ProcessBatch(Batch);
      
    Batch.Free;
  end;
end;

procedure TQueryPipeline.ProcessBatch(const Batch: TList);
var
  I: Integer;
  Request: TPipelinedRequest;
begin
  FLock.Enter;
  try
    FInFlight += Batch.Count;
  finally
    FLock.Leave;
  end;
  
  try
    for I := 0 to Batch.Count - 1 do
    begin
      Request := TPipelinedRequest(Batch[I]);
      try
        if ExecuteRequest(Request) then
          Request.ResponseEvent.SetEvent
        else
          Request.ErrorEvent.SetEvent;
      except
        on E: Exception do
        begin
          Request.ErrorData := E;
          Request.ErrorEvent.SetEvent;
        end;
      end;
    end;
  finally
    FLock.Enter;
    try
      FInFlight -= Batch.Count;
    finally
      FLock.Leave;
    end;
  end;
end;

function TQueryPipeline.ExecuteRequest(Request: TPipelinedRequest): Boolean;
begin
  // Implementation depends on specific database connection
  // Return True on success, False on failure
  Result := True;
end;

{ TPipelineBuilder }

constructor TPipelineBuilder.Create;
begin
  FQueries := TStringList.Create;
end;

destructor TPipelineBuilder.Destroy;
begin
  FQueries.Free;
  inherited Destroy;
end;

function TPipelineBuilder.Add(const SQL: string): TPipelineBuilder;
begin
  FQueries.Add(SQL);
  Result := Self;
end;

function TPipelineBuilder.Build: TStringList;
begin
  Result := TStringList.Create;
  Result.Assign(FQueries);
end;

end.
