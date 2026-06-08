// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

{ ScratchBird Pascal Driver - Connection Leak Detector
  Copyright (c) 2025-2026 Dalton Calford }

unit SBLeakDetector;

{$mode objfpc}{$H+}

interface

uses
  Classes, SysUtils, DateUtils;

type
  TLeakLogLevel = (llDebug, llWarn, llError);
  
  TLeakDetectionConfig = record
    ThresholdMs: Cardinal;
    CaptureStackTrace: Boolean;
    CheckIntervalMs: Cardinal;
    LogLevel: TLeakLogLevel;
  end;
  
  TCheckoutInfo = class
  public
    CheckoutTime: TDateTime;
    ThreadId: Cardinal;
    StackTrace: string;
    Metadata: TStringList;
    constructor Create(CaptureStackTrace: Boolean; const Meta: array of string);
    destructor Destroy; override;
    function GetHeldDurationMs: Cardinal;
  end;
  
  TLeakDetector = class(TThread)
  private
    FConfig: TLeakDetectionConfig;
    FCheckouts: TThreadList;
    FRunning: Boolean;
    FStarted: Boolean;
    procedure ClearCheckouts;
    procedure CheckLeaks;
    procedure LogLeak(const ConnId: string; Info: TCheckoutInfo);
  protected
    procedure Execute; override;
  public
    constructor Create(const Config: TLeakDetectionConfig);
    destructor Destroy; override;
    procedure Checkout(const ConnectionId: string; const Metadata: array of string);
    procedure Checkin(const ConnectionId: string);
    function GetActiveCount: Integer;
    procedure Start;
    procedure Stop;
  end;

function DefaultLeakDetectionConfig: TLeakDetectionConfig;

implementation

type
  TLeakCheckoutEntry = class
  public
    ConnectionId: string;
    Info: TCheckoutInfo;
    constructor Create(const AConnectionId: string; AInfo: TCheckoutInfo);
    destructor Destroy; override;
  end;

{$IFDEF FPC}
function GetStackTraceInfo: string;
begin
  Result := '';
end;
{$ENDIF}

function DefaultLeakDetectionConfig: TLeakDetectionConfig;
begin
  Result.ThresholdMs := 30000;      // 30 seconds
  Result.CaptureStackTrace := False;
  Result.CheckIntervalMs := 10000;  // 10 seconds
  Result.LogLevel := llWarn;
end;

{ TLeakCheckoutEntry }

constructor TLeakCheckoutEntry.Create(const AConnectionId: string; AInfo: TCheckoutInfo);
begin
  ConnectionId := AConnectionId;
  Info := AInfo;
end;

destructor TLeakCheckoutEntry.Destroy;
begin
  Info.Free;
  inherited Destroy;
end;

{ TCheckoutInfo }

constructor TCheckoutInfo.Create(CaptureStackTrace: Boolean; const Meta: array of string);
var
  I: Integer;
begin
  CheckoutTime := Now;
  ThreadId := ThreadID;
  Metadata := TStringList.Create;
  I := 0;
  while I <= High(Meta) do
  begin
    if I = High(Meta) then
      Metadata.Values['meta_' + IntToStr(I div 2)] := Meta[I]
    else
      Metadata.Values[Meta[I]] := Meta[I + 1];
    Inc(I, 2);
  end;
  if CaptureStackTrace then
    StackTrace := GetStackTraceInfo
  else
    StackTrace := '';
end;

destructor TCheckoutInfo.Destroy;
begin
  Metadata.Free;
  inherited Destroy;
end;

function TCheckoutInfo.GetHeldDurationMs: Cardinal;
begin
  Result := Cardinal(MilliSecondsBetween(Now, CheckoutTime));
end;

{ TLeakDetector }

constructor TLeakDetector.Create(const Config: TLeakDetectionConfig);
begin
  inherited Create(True);
  FConfig := Config;
  FCheckouts := TThreadList.Create;
  FRunning := False;
  FStarted := False;
end;

destructor TLeakDetector.Destroy;
begin
  Stop;
  ClearCheckouts;
  FCheckouts.Free;
  inherited Destroy;
end;

procedure TLeakDetector.Start;
begin
  if FStarted then
    Exit;
  FRunning := True;
  FStarted := True;
  inherited Start;
end;

procedure TLeakDetector.Stop;
begin
  if not FStarted then
    Exit;
  FRunning := False;
  Terminate;
  WaitFor;
end;

procedure TLeakDetector.Checkout(const ConnectionId: string; const Metadata: array of string);
var
  Entry: TLeakCheckoutEntry;
  List: TList;
  I: Integer;
begin
  Entry := TLeakCheckoutEntry.Create(ConnectionId, TCheckoutInfo.Create(FConfig.CaptureStackTrace, Metadata));
  List := FCheckouts.LockList;
  try
    for I := List.Count - 1 downto 0 do
      if SameText(TLeakCheckoutEntry(List[I]).ConnectionId, ConnectionId) then
      begin
        TObject(List[I]).Free;
        List.Delete(I);
      end;
    List.Add(Entry);
    Entry := nil;
  finally
    FCheckouts.UnlockList;
    Entry.Free;
  end;
end;

procedure TLeakDetector.Checkin(const ConnectionId: string);
var
  List: TList;
  I: Integer;
  Entry: TLeakCheckoutEntry;
begin
  List := FCheckouts.LockList;
  try
    for I := List.Count - 1 downto 0 do
    begin
      Entry := TLeakCheckoutEntry(List[I]);
      if SameText(Entry.ConnectionId, ConnectionId) then
      begin
        List.Delete(I);
        Entry.Free;
        Break;
      end;
    end;
  finally
    FCheckouts.UnlockList;
  end;
end;

function TLeakDetector.GetActiveCount: Integer;
var
  List: TList;
begin
  List := FCheckouts.LockList;
  try
    Result := List.Count;
  finally
    FCheckouts.UnlockList;
  end;
end;

procedure TLeakDetector.Execute;
begin
  while FRunning and not Terminated do
  begin
    Sleep(Integer(FConfig.CheckIntervalMs));
    if FRunning then
      CheckLeaks;
  end;
end;

procedure TLeakDetector.ClearCheckouts;
var
  List: TList;
  I: Integer;
begin
  List := FCheckouts.LockList;
  try
    for I := List.Count - 1 downto 0 do
      TObject(List[I]).Free;
    List.Clear;
  finally
    FCheckouts.UnlockList;
  end;
end;

procedure TLeakDetector.CheckLeaks;
var
  List: TList;
  I: Integer;
  Entry: TLeakCheckoutEntry;
begin
  List := FCheckouts.LockList;
  try
    for I := 0 to List.Count - 1 do
    begin
      Entry := TLeakCheckoutEntry(List[I]);
      if (Entry = nil) or (Entry.Info = nil) then
        Continue;
      if Entry.Info.GetHeldDurationMs > FConfig.ThresholdMs then
        LogLeak(Entry.ConnectionId, Entry.Info);
    end;
  finally
    FCheckouts.UnlockList;
  end;
end;

procedure TLeakDetector.LogLeak(const ConnId: string; Info: TCheckoutInfo);
begin
  WriteLn('POSSIBLE CONNECTION LEAK: conn=', ConnId, ', held=', Info.GetHeldDurationMs, 'ms');
end;

end.
