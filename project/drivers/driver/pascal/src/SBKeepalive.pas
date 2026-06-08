// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

{ ScratchBird Pascal Driver
  Keepalive Manager - Prevents connection timeouts
  Copyright (c) 2025-2026 Dalton Calford }

unit SBKeepalive;

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
  TKeepaliveConfig = record
    IntervalMs: Cardinal;
    MaxIdleBeforeCheckMs: Cardinal;
    ValidationTimeoutMs: Cardinal;
  end;
  
  TKeepaliveTracker = class
  private
    FConfig: TKeepaliveConfig;
    FLastActivity: TDateTime;
    FLock: TCriticalSection;
  public
    constructor Create(const Config: TKeepaliveConfig);
    destructor Destroy; override;
    procedure MarkActive;
    function NeedsValidation: Boolean;
    function GetIdleDurationMs: Cardinal;
  end;
  
  TPingerFunction = function: Boolean of object;
  
  TKeepaliveManager = class(TThread)
  private
    FConfig: TKeepaliveConfig;
    FTrackers: TThreadList;
    FRunning: Boolean;
    FStarted: Boolean;
    procedure ClearEntries;
    procedure CheckConnections;
  protected
    procedure Execute; override;
  public
    constructor Create(const Config: TKeepaliveConfig);
    destructor Destroy; override;
    function Register(const ConnectionId: string; Pinger: TPingerFunction): TKeepaliveTracker;
    procedure Unregister(const ConnectionId: string);
    function GetMonitoredCount: Integer;
    procedure Start;
    procedure Stop;
  end;

function DefaultKeepaliveConfig: TKeepaliveConfig;

implementation

type
  TKeepaliveEntry = class
  public
    ConnectionId: string;
    Tracker: TKeepaliveTracker;
    Pinger: TPingerFunction;
    constructor Create(const AConnectionId: string; ATracker: TKeepaliveTracker; APinger: TPingerFunction);
    destructor Destroy; override;
  end;

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

function DefaultKeepaliveConfig: TKeepaliveConfig;
begin
  Result.IntervalMs := 120000;         // 2 minutes
  Result.MaxIdleBeforeCheckMs := 600000; // 10 minutes
  Result.ValidationTimeoutMs := 5000;    // 5 seconds
end;

{ TKeepaliveEntry }

constructor TKeepaliveEntry.Create(const AConnectionId: string; ATracker: TKeepaliveTracker; APinger: TPingerFunction);
begin
  ConnectionId := AConnectionId;
  Tracker := ATracker;
  Pinger := APinger;
end;

destructor TKeepaliveEntry.Destroy;
begin
  Tracker.Free;
  inherited Destroy;
end;

{ TKeepaliveTracker }

constructor TKeepaliveTracker.Create(const Config: TKeepaliveConfig);
begin
  FConfig := Config;
  FLastActivity := Now;
  FLock := TCriticalSection.Create;
end;

destructor TKeepaliveTracker.Destroy;
begin
  FLock.Free;
  inherited Destroy;
end;

procedure TKeepaliveTracker.MarkActive;
begin
  FLock.Enter;
  try
    FLastActivity := Now;
  finally
    FLock.Leave;
  end;
end;

function TKeepaliveTracker.NeedsValidation: Boolean;
begin
  Result := GetIdleDurationMs > FConfig.MaxIdleBeforeCheckMs;
end;

function TKeepaliveTracker.GetIdleDurationMs: Cardinal;
begin
  FLock.Enter;
  try
    Result := Cardinal(MilliSecondsBetween(Now, FLastActivity));
  finally
    FLock.Leave;
  end;
end;

{ TKeepaliveManager }

constructor TKeepaliveManager.Create(const Config: TKeepaliveConfig);
begin
  inherited Create(True);
  FConfig := Config;
  FTrackers := TThreadList.Create;
  FRunning := False;
  FStarted := False;
end;

destructor TKeepaliveManager.Destroy;
begin
  Stop;
  ClearEntries;
  FTrackers.Free;
  inherited Destroy;
end;

procedure TKeepaliveManager.Start;
begin
  if FStarted then
    Exit;
  FRunning := True;
  FStarted := True;
  inherited Start;
end;

procedure TKeepaliveManager.Stop;
begin
  if not FStarted then
    Exit;
  FRunning := False;
  Terminate;
  WaitFor;
end;

function TKeepaliveManager.Register(const ConnectionId: string; Pinger: TPingerFunction): TKeepaliveTracker;
var
  List: TList;
  I: Integer;
  Entry: TKeepaliveEntry;
begin
  List := FTrackers.LockList;
  try
    for I := 0 to List.Count - 1 do
    begin
      Entry := TKeepaliveEntry(List[I]);
      if SameText(Entry.ConnectionId, ConnectionId) then
      begin
        Entry.Pinger := Pinger;
        Entry.Tracker.MarkActive;
        Exit(Entry.Tracker);
      end;
    end;
    Result := TKeepaliveTracker.Create(FConfig);
    Entry := TKeepaliveEntry.Create(ConnectionId, Result, Pinger);
    List.Add(Entry);
  finally
    FTrackers.UnlockList;
  end;
end;

procedure TKeepaliveManager.Unregister(const ConnectionId: string);
var
  List: TList;
  I: Integer;
  Entry: TKeepaliveEntry;
begin
  List := FTrackers.LockList;
  try
    for I := List.Count - 1 downto 0 do
    begin
      Entry := TKeepaliveEntry(List[I]);
      if SameText(Entry.ConnectionId, ConnectionId) then
      begin
        List.Delete(I);
        Entry.Free;
        Break;
      end;
    end;
  finally
    FTrackers.UnlockList;
  end;
end;

function TKeepaliveManager.GetMonitoredCount: Integer;
var
  List: TList;
begin
  List := FTrackers.LockList;
  try
    Result := List.Count;
  finally
    FTrackers.UnlockList;
  end;
end;

procedure TKeepaliveManager.Execute;
begin
  while FRunning and not Terminated do
  begin
    Sleep(Integer(FConfig.IntervalMs));
    if FRunning then
      CheckConnections;
  end;
end;

procedure TKeepaliveManager.ClearEntries;
var
  List: TList;
  I: Integer;
begin
  List := FTrackers.LockList;
  try
    for I := List.Count - 1 downto 0 do
      TObject(List[I]).Free;
    List.Clear;
  finally
    FTrackers.UnlockList;
  end;
end;

procedure TKeepaliveManager.CheckConnections;
var
  List: TList;
  I: Integer;
  Entry: TKeepaliveEntry;
  IsHealthy: Boolean;
begin
  List := FTrackers.LockList;
  try
    for I := 0 to List.Count - 1 do
    begin
      Entry := TKeepaliveEntry(List[I]);
      if (Entry = nil) or (Entry.Tracker = nil) then
        Continue;
      if not Entry.Tracker.NeedsValidation then
        Continue;
      IsHealthy := False;
      try
        if Assigned(Entry.Pinger) then
          IsHealthy := Entry.Pinger();
      except
        IsHealthy := False;
      end;
      if IsHealthy then
        Entry.Tracker.MarkActive;
    end;
  finally
    FTrackers.UnlockList;
  end;
end;

end.
