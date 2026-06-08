// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

program AdapterTransactionOptionsTests;

{$mode delphi}
{$APPTYPE CONSOLE}

uses
  {$IFDEF UNIX}
  cthreads,
  {$ENDIF}
  SysUtils,
  ScratchBird.Protocol, ScratchBird.Errors,
  ScratchBird.FireDAC, ScratchBird.IBX, ScratchBird.Zeos, ScratchBird.SQLdb;

procedure Fail(const MessageText: string);
begin
  raise Exception.Create(MessageText);
end;

procedure AssertEqualString(const Expected, Actual, MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText + ': expected="' + Expected + '" actual="' + Actual + '"');
end;

procedure TestFireDACStartTransactionExDisconnected;
var
  Connection: TScratchBirdFDConnection;
begin
  Connection := TScratchBirdFDConnection.Create(nil);
  try
    try
      Connection.StartTransactionEx(ISOLATION_READ_COMMITTED, 1, True, False, 250, 1, 2,
        READ_COMMITTED_MODE_READ_CONSISTENCY);
      Fail('FireDAC StartTransactionEx: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'FireDAC StartTransactionEx SQLSTATE');
    end;
  finally
    Connection.Free;
  end;
end;

procedure TestIBXStartTransactionExDisconnected;
var
  Database: TScratchBirdIBDatabase;
begin
  Database := TScratchBirdIBDatabase.Create(nil);
  try
    try
      Database.StartTransactionEx(ISOLATION_READ_COMMITTED, 1, True, False, 250, 1, 2,
        READ_COMMITTED_MODE_READ_CONSISTENCY);
      Fail('IBX StartTransactionEx: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'IBX StartTransactionEx SQLSTATE');
    end;
  finally
    Database.Free;
  end;
end;

procedure TestZeosStartTransactionExDisconnected;
var
  Connection: TScratchBirdZConnection;
begin
  Connection := TScratchBirdZConnection.Create(nil);
  try
    try
      Connection.StartTransactionEx(ISOLATION_READ_COMMITTED, 1, True, False, 250, 1, 2,
        READ_COMMITTED_MODE_READ_CONSISTENCY);
      Fail('Zeos StartTransactionEx: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'Zeos StartTransactionEx SQLSTATE');
    end;
  finally
    Connection.Free;
  end;
end;

procedure TestSQLdbStartTransactionExDisconnected;
var
  Connection: TScratchBirdSQLConnection;
begin
  Connection := TScratchBirdSQLConnection.Create(nil);
  try
    try
      Connection.StartTransactionEx(ISOLATION_READ_COMMITTED, 1, True, False, 250, 1, 2,
        READ_COMMITTED_MODE_READ_CONSISTENCY);
      Fail('SQLdb StartTransactionEx: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'SQLdb StartTransactionEx SQLSTATE');
    end;
  finally
    Connection.Free;
  end;
end;

begin
  try
    TestFireDACStartTransactionExDisconnected;
    TestIBXStartTransactionExDisconnected;
    TestZeosStartTransactionExDisconnected;
    TestSQLdbStartTransactionExDisconnected;
    Writeln('AdapterTransactionOptionsTests: OK');
  except
    on E: Exception do
    begin
      Writeln('AdapterTransactionOptionsTests: FAILED - ' + E.Message);
      Halt(1);
    end;
  end;
end.
