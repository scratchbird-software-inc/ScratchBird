// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

{ ScratchBird Pascal Driver - Circuit Breaker
  Copyright (c) 2025-2026 Dalton Calford }

unit SBCircuitBreaker;

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
  TCircuitState = (csClosed, csOpen, csHalfOpen);
  
  TCircuitBreakerConfig = record
    FailureThreshold: Cardinal;
    RecoveryTimeoutMs: Cardinal;
    SuccessThreshold: Cardinal;
    HalfOpenMaxRequests: Cardinal;
  end;

  TCircuitBreakerStats = record
    State: TCircuitState;
    FailureCount: Cardinal;
    SuccessCount: Cardinal;
  end;
  
  ECircuitBreakerOpen = class(Exception);
  
  TCircuitBreaker = class
  private
    FConfig: TCircuitBreakerConfig;
    FName: string;
    FState: TCircuitState;
    FFailureCount: Cardinal;
    FSuccessCount: Cardinal;
    FHalfOpenRequests: Cardinal;
    FLastFailureTime: TDateTime;
    FLock: TCriticalSection;
    function AllowHalfOpenRequest: Boolean;
  public
    constructor Create(const Config: TCircuitBreakerConfig; const Name: string = 'default');
    destructor Destroy; override;
    function GetState: TCircuitState;
    function AllowRequest: Boolean;
    procedure RecordSuccess;
    procedure RecordFailure;
    procedure Reset;
    {$IFNDEF FPC}
    function Execute<T>(Func: TFunc<T>): T;
    {$ENDIF}
    function GetStats: TCircuitBreakerStats;
  end;

function DefaultCircuitBreakerConfig: TCircuitBreakerConfig;

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

function DefaultCircuitBreakerConfig: TCircuitBreakerConfig;
begin
  Result.FailureThreshold := 5;
  Result.RecoveryTimeoutMs := 30000;
  Result.SuccessThreshold := 3;
  Result.HalfOpenMaxRequests := 10;
end;

{ TCircuitBreaker }

constructor TCircuitBreaker.Create(const Config: TCircuitBreakerConfig; const Name: string);
begin
  FConfig := Config;
  FName := Name;
  FState := csClosed;
  FFailureCount := 0;
  FSuccessCount := 0;
  FHalfOpenRequests := 0;
  FLastFailureTime := 0;
  FLock := TCriticalSection.Create;
end;

destructor TCircuitBreaker.Destroy;
begin
  FLock.Free;
  inherited Destroy;
end;

function TCircuitBreaker.GetState: TCircuitState;
begin
  FLock.Enter;
  try
    Result := FState;
  finally
    FLock.Leave;
  end;
end;

function TCircuitBreaker.AllowRequest: Boolean;
begin
  FLock.Enter;
  try
    case FState of
      csClosed:
        Result := True;
      csOpen:
        begin
          if (FLastFailureTime > 0) and (MilliSecondsBetween(Now, FLastFailureTime) >= FConfig.RecoveryTimeoutMs) then
          begin
            FState := csHalfOpen;
            FFailureCount := 0;
            FSuccessCount := 0;
            FHalfOpenRequests := 0;
            Result := AllowHalfOpenRequest;
          end
          else
            Result := False;
        end;
      csHalfOpen:
        Result := AllowHalfOpenRequest;
    else
      Result := False;
    end;
  finally
    FLock.Leave;
  end;
end;

function TCircuitBreaker.AllowHalfOpenRequest: Boolean;
begin
  Result := FHalfOpenRequests < FConfig.HalfOpenMaxRequests;
  if Result then
    Inc(FHalfOpenRequests);
end;

procedure TCircuitBreaker.RecordSuccess;
begin
  FLock.Enter;
  try
    case FState of
      csClosed:
        FFailureCount := 0;
      csHalfOpen:
        begin
          Dec(FHalfOpenRequests);
          Inc(FSuccessCount);
          if FSuccessCount >= FConfig.SuccessThreshold then
          begin
            FState := csClosed;
            FFailureCount := 0;
            FSuccessCount := 0;
          end;
        end;
    end;
  finally
    FLock.Leave;
  end;
end;

procedure TCircuitBreaker.RecordFailure;
begin
  FLock.Enter;
  try
    case FState of
      csClosed:
        begin
          Inc(FFailureCount);
          if FFailureCount >= FConfig.FailureThreshold then
          begin
            FState := csOpen;
            FLastFailureTime := Now;
          end;
        end;
      csHalfOpen:
        begin
          Dec(FHalfOpenRequests);
          FState := csOpen;
          FLastFailureTime := Now;
        end;
      csOpen:
        FLastFailureTime := Now;
    end;
  finally
    FLock.Leave;
  end;
end;

procedure TCircuitBreaker.Reset;
begin
  FLock.Enter;
  try
    FState := csClosed;
    FFailureCount := 0;
    FSuccessCount := 0;
    FHalfOpenRequests := 0;
    FLastFailureTime := 0;
  finally
    FLock.Leave;
  end;
end;

{$IFNDEF FPC}
function TCircuitBreaker.Execute<T>(Func: TFunc<T>): T;
begin
  if not AllowRequest then
    raise ECircuitBreakerOpen.Create('Circuit breaker is OPEN');

  try
    Result := Func();
    RecordSuccess;
  except
    RecordFailure;
    raise;
  end;
end;
{$ENDIF}

function TCircuitBreaker.GetStats: TCircuitBreakerStats;
begin
  FLock.Enter;
  try
    Result.State := FState;
    Result.FailureCount := FFailureCount;
    Result.SuccessCount := FSuccessCount;
  finally
    FLock.Leave;
  end;
end;

end.
