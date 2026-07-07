// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

program ErrorMappingTests;

{$mode delphi}
{$APPTYPE CONSOLE}

uses
  {$IFDEF UNIX}
  cthreads,
  {$ENDIF}
  SysUtils,
  ScratchBird.Errors;

type
  TScratchBirdErrorClass = class of EScratchBirdError;

procedure Fail(const MessageText: string);
begin
  raise Exception.Create(MessageText);
end;

procedure AssertTrue(Value: Boolean; const MessageText: string);
begin
  if not Value then
    Fail(MessageText);
end;

procedure AssertEqualString(const Expected, Actual, MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText + ': expected="' + Expected + '" actual="' + Actual + '"');
end;

procedure AssertEqualRetryScope(Expected, Actual: TScratchBirdRetryScope; const MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText);
end;

procedure AssertMappedClass(const SQLState: string; ExpectedClass: TScratchBirdErrorClass);
var
  Err: EScratchBirdError;
begin
  Err := MapSqlState(SQLState, 'message', 'detail', 'hint');
  try
    AssertTrue(Err is ExpectedClass, 'mapped class mismatch for SQLSTATE ' + SQLState);
    AssertEqualString(SQLState, Err.SQLState, 'sqlstate roundtrip');
    AssertEqualString('detail', Err.Detail, 'detail roundtrip');
    AssertEqualString('hint', Err.Hint, 'hint roundtrip');
  finally
    Err.Free;
  end;
end;

procedure TestMappedCategories;
begin
  AssertMappedClass('01000', EScratchbirdWarning);
  AssertMappedClass('02000', EScratchbirdNoData);
  AssertMappedClass('08006', EScratchbirdConnectionError);
  AssertMappedClass('0A000', EScratchbirdNotSupported);
  AssertMappedClass('22P02', EScratchbirdDataError);
  AssertMappedClass('23505', EScratchbirdIntegrityError);
  AssertMappedClass('28P01', EScratchbirdAuthError);
  AssertMappedClass('40001', EScratchbirdTransactionError);
  AssertMappedClass('42601', EScratchbirdSyntaxError);
  AssertMappedClass('53300', EScratchbirdResourceError);
  AssertMappedClass('54000', EScratchbirdLimitError);
  AssertMappedClass('57014', EScratchbirdOperatorInterventionError);
  AssertMappedClass('58000', EScratchbirdSystemError);
  AssertMappedClass('XX000', EScratchbirdInternalError);
end;

procedure TestFallbackForUnknownSqlState;
begin
  AssertMappedClass('ZZ999', EScratchBirdError);
end;

procedure TestFallbackForInvalidSqlStateLength;
begin
  AssertMappedClass('42P1', EScratchBirdError);
end;

procedure TestRetryScopeClassification;
begin
  AssertEqualRetryScope(rsStatement, RetryScopeForSqlState('40001'), '40001 retry scope');
  AssertEqualRetryScope(rsStatement, RetryScopeForSqlState('40P01'), '40P01 retry scope');
  AssertEqualRetryScope(rsReconnect, RetryScopeForSqlState('08006'), '08006 retry scope');
  AssertEqualRetryScope(rsReconnect, RetryScopeForSqlState('08003'), '08003 retry scope');
  AssertEqualRetryScope(rsNone, RetryScopeForSqlState('57014'), '57014 retry scope');
  AssertEqualRetryScope(rsNone, RetryScopeForSqlState(''), 'empty retry scope');
  AssertTrue(IsRetryableSqlState('40001'), '40001 retryable');
  AssertTrue(IsRetryableSqlState('08006'), '08006 retryable');
  AssertTrue(not IsRetryableSqlState('57014'), '57014 not retryable');
end;

begin
  try
    TestMappedCategories;
    TestFallbackForUnknownSqlState;
    TestFallbackForInvalidSqlStateLength;
    TestRetryScopeClassification;
    Writeln('ErrorMappingTests: OK');
  except
    on E: Exception do
    begin
      Writeln('ErrorMappingTests: FAILED - ' + E.Message);
      Halt(1);
    end;
  end;
end.
