// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

program AdapterMetadataApiTests;

{$mode delphi}
{$APPTYPE CONSOLE}

uses
  {$IFDEF UNIX}
  cthreads,
  {$ENDIF}
  SysUtils,
  ScratchBird.Client,
  ScratchBird.FireDAC, ScratchBird.IBX, ScratchBird.Zeos, ScratchBird.SQLdb,
  ScratchBird.Errors, ScratchBird.Metadata;

procedure Fail(const MessageText: string);
begin
  raise Exception.Create(MessageText);
end;

procedure AssertEqualString(const Expected, Actual, MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText + ': expected="' + Expected + '" actual="' + Actual + '"');
end;

procedure TestFireDACMetadataApiGuards;
var
  Connection: TScratchBirdFDConnection;
  Restrictions: TMetadataRow;
  Rows: TMetadataRows;
  Stream: TScratchBirdResultStream;
begin
  Connection := TScratchBirdFDConnection.Create(nil);
  try
    SetLength(Restrictions, 1);
    Restrictions[0].Name := 'TABLE_SCHEMA';
    Restrictions[0].Value := 'public';

    try
      Stream := Connection.QueryMetadata('tables');
      Stream.Free;
      Fail('FireDAC QueryMetadata: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'FireDAC QueryMetadata SQLSTATE');
    end;

    try
      Rows := Connection.QueryMetadataRows('tables');
      if Length(Rows) >= 0 then;
      Fail('FireDAC QueryMetadataRows: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'FireDAC QueryMetadataRows SQLSTATE');
    end;

    try
      Rows := Connection.QueryMetadataRows('tables', Restrictions);
      if Length(Rows) >= 0 then;
      Fail('FireDAC QueryMetadataRows restricted: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'FireDAC QueryMetadataRows restricted SQLSTATE');
    end;

    try
      Rows := Connection.GetSchemaRows('tables');
      if Length(Rows) >= 0 then;
      Fail('FireDAC GetSchemaRows: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'FireDAC GetSchemaRows SQLSTATE');
    end;

    try
      Stream := Connection.GetCatalogs;
      Stream.Free;
      Fail('FireDAC GetCatalogs: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'FireDAC GetCatalogs SQLSTATE');
    end;

    try
      Stream := Connection.GetRoutines;
      Stream.Free;
      Fail('FireDAC GetRoutines: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'FireDAC GetRoutines SQLSTATE');
    end;

    try
      Stream := Connection.GetIndexColumns;
      Stream.Free;
      Fail('FireDAC GetIndexColumns: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'FireDAC GetIndexColumns SQLSTATE');
    end;

    try
      Stream := Connection.GetTypeInfo;
      Stream.Free;
      Fail('FireDAC GetTypeInfo: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'FireDAC GetTypeInfo SQLSTATE');
    end;

    try
      Stream := Connection.GetSchema('not_supported');
      Stream.Free;
      Fail('FireDAC GetSchema unsupported: expected not supported error');
    except
      on E: EScratchbirdNotSupported do
        AssertEqualString('0A000', E.SQLState, 'FireDAC GetSchema unsupported SQLSTATE');
    end;
  finally
    Connection.Free;
  end;
end;

procedure TestIBXMetadataApiGuards;
var
  Database: TScratchBirdIBDatabase;
  Restrictions: TMetadataRow;
  Rows: TMetadataRows;
  Stream: TScratchBirdResultStream;
begin
  Database := TScratchBirdIBDatabase.Create(nil);
  try
    SetLength(Restrictions, 1);
    Restrictions[0].Name := 'TABLE_SCHEMA';
    Restrictions[0].Value := 'public';

    try
      Stream := Database.QueryMetadata('tables');
      Stream.Free;
      Fail('IBX QueryMetadata: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'IBX QueryMetadata SQLSTATE');
    end;

    try
      Rows := Database.QueryMetadataRows('tables');
      if Length(Rows) >= 0 then;
      Fail('IBX QueryMetadataRows: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'IBX QueryMetadataRows SQLSTATE');
    end;

    try
      Rows := Database.QueryMetadataRows('tables', Restrictions);
      if Length(Rows) >= 0 then;
      Fail('IBX QueryMetadataRows restricted: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'IBX QueryMetadataRows restricted SQLSTATE');
    end;

    try
      Rows := Database.GetSchemaRows('tables');
      if Length(Rows) >= 0 then;
      Fail('IBX GetSchemaRows: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'IBX GetSchemaRows SQLSTATE');
    end;

    try
      Stream := Database.GetCatalogs;
      Stream.Free;
      Fail('IBX GetCatalogs: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'IBX GetCatalogs SQLSTATE');
    end;

    try
      Stream := Database.GetRoutines;
      Stream.Free;
      Fail('IBX GetRoutines: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'IBX GetRoutines SQLSTATE');
    end;

    try
      Stream := Database.GetIndexColumns;
      Stream.Free;
      Fail('IBX GetIndexColumns: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'IBX GetIndexColumns SQLSTATE');
    end;

    try
      Stream := Database.GetTypeInfo;
      Stream.Free;
      Fail('IBX GetTypeInfo: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'IBX GetTypeInfo SQLSTATE');
    end;

    try
      Stream := Database.GetSchema('not_supported');
      Stream.Free;
      Fail('IBX GetSchema unsupported: expected not supported error');
    except
      on E: EScratchbirdNotSupported do
        AssertEqualString('0A000', E.SQLState, 'IBX GetSchema unsupported SQLSTATE');
    end;
  finally
    Database.Free;
  end;
end;

procedure TestZeosMetadataApiGuards;
var
  Connection: TScratchBirdZConnection;
  Restrictions: TMetadataRow;
  Rows: TMetadataRows;
  Stream: TScratchBirdResultStream;
begin
  Connection := TScratchBirdZConnection.Create(nil);
  try
    SetLength(Restrictions, 1);
    Restrictions[0].Name := 'TABLE_SCHEMA';
    Restrictions[0].Value := 'public';

    try
      Stream := Connection.QueryMetadata('tables');
      Stream.Free;
      Fail('Zeos QueryMetadata: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'Zeos QueryMetadata SQLSTATE');
    end;

    try
      Rows := Connection.QueryMetadataRows('tables');
      if Length(Rows) >= 0 then;
      Fail('Zeos QueryMetadataRows: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'Zeos QueryMetadataRows SQLSTATE');
    end;

    try
      Rows := Connection.QueryMetadataRows('tables', Restrictions);
      if Length(Rows) >= 0 then;
      Fail('Zeos QueryMetadataRows restricted: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'Zeos QueryMetadataRows restricted SQLSTATE');
    end;

    try
      Rows := Connection.GetSchemaRows('tables');
      if Length(Rows) >= 0 then;
      Fail('Zeos GetSchemaRows: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'Zeos GetSchemaRows SQLSTATE');
    end;

    try
      Stream := Connection.GetCatalogs;
      Stream.Free;
      Fail('Zeos GetCatalogs: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'Zeos GetCatalogs SQLSTATE');
    end;

    try
      Stream := Connection.GetRoutines;
      Stream.Free;
      Fail('Zeos GetRoutines: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'Zeos GetRoutines SQLSTATE');
    end;

    try
      Stream := Connection.GetIndexColumns;
      Stream.Free;
      Fail('Zeos GetIndexColumns: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'Zeos GetIndexColumns SQLSTATE');
    end;

    try
      Stream := Connection.GetTypeInfo;
      Stream.Free;
      Fail('Zeos GetTypeInfo: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'Zeos GetTypeInfo SQLSTATE');
    end;

    try
      Stream := Connection.GetSchema('not_supported');
      Stream.Free;
      Fail('Zeos GetSchema unsupported: expected not supported error');
    except
      on E: EScratchbirdNotSupported do
        AssertEqualString('0A000', E.SQLState, 'Zeos GetSchema unsupported SQLSTATE');
    end;
  finally
    Connection.Free;
  end;
end;

procedure TestSQLdbMetadataApiGuards;
var
  Connection: TScratchBirdSQLConnection;
  Restrictions: TMetadataRow;
  Rows: TMetadataRows;
  Stream: TScratchBirdResultStream;
begin
  Connection := TScratchBirdSQLConnection.Create(nil);
  try
    SetLength(Restrictions, 1);
    Restrictions[0].Name := 'TABLE_SCHEMA';
    Restrictions[0].Value := 'public';

    try
      Stream := Connection.QueryMetadata('tables');
      Stream.Free;
      Fail('SQLdb QueryMetadata: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'SQLdb QueryMetadata SQLSTATE');
    end;

    try
      Rows := Connection.QueryMetadataRows('tables');
      if Length(Rows) >= 0 then;
      Fail('SQLdb QueryMetadataRows: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'SQLdb QueryMetadataRows SQLSTATE');
    end;

    try
      Rows := Connection.QueryMetadataRows('tables', Restrictions);
      if Length(Rows) >= 0 then;
      Fail('SQLdb QueryMetadataRows restricted: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'SQLdb QueryMetadataRows restricted SQLSTATE');
    end;

    try
      Rows := Connection.GetSchemaRows('tables');
      if Length(Rows) >= 0 then;
      Fail('SQLdb GetSchemaRows: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'SQLdb GetSchemaRows SQLSTATE');
    end;

    try
      Stream := Connection.GetCatalogs;
      Stream.Free;
      Fail('SQLdb GetCatalogs: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'SQLdb GetCatalogs SQLSTATE');
    end;

    try
      Stream := Connection.GetRoutines;
      Stream.Free;
      Fail('SQLdb GetRoutines: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'SQLdb GetRoutines SQLSTATE');
    end;

    try
      Stream := Connection.GetIndexColumns;
      Stream.Free;
      Fail('SQLdb GetIndexColumns: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'SQLdb GetIndexColumns SQLSTATE');
    end;

    try
      Stream := Connection.GetTypeInfo;
      Stream.Free;
      Fail('SQLdb GetTypeInfo: expected disconnected connection error');
    except
      on E: EScratchbirdConnectionError do
        AssertEqualString('08003', E.SQLState, 'SQLdb GetTypeInfo SQLSTATE');
    end;

    try
      Stream := Connection.GetSchema('not_supported');
      Stream.Free;
      Fail('SQLdb GetSchema unsupported: expected not supported error');
    except
      on E: EScratchbirdNotSupported do
        AssertEqualString('0A000', E.SQLState, 'SQLdb GetSchema unsupported SQLSTATE');
    end;
  finally
    Connection.Free;
  end;
end;

begin
  try
    TestFireDACMetadataApiGuards;
    TestIBXMetadataApiGuards;
    TestZeosMetadataApiGuards;
    TestSQLdbMetadataApiGuards;
    Writeln('AdapterMetadataApiTests: OK');
  except
    on E: Exception do
    begin
      Writeln('AdapterMetadataApiTests: FAILED - ' + E.Message);
      Halt(1);
    end;
  end;
end.
