// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

program SqlTests;

{$mode delphi}
{$APPTYPE CONSOLE}

uses
  SysUtils, Variants, ScratchBird.Sql;

procedure AssertEqual(const Expected, Actual, MessageText: string);
begin
  if Expected <> Actual then
    raise Exception.Create(MessageText + ': expected=' + Expected + ' actual=' + Actual);
end;

procedure AssertTrue(Value: Boolean; const MessageText: string);
begin
  if not Value then
    raise Exception.Create(MessageText);
end;

var
  OutSql: string;
  PositionalParams: TArray<TScratchBirdParamInput>;
  PositionalOrdered: TArray<TScratchBirdParamInput>;
  NamedNames: TArray<string>;
  NamedParams: TArray<TScratchBirdParamInput>;
  NamedOrdered: TArray<TScratchBirdParamInput>;
begin
  try
    SetLength(PositionalParams, 2);
    PositionalParams[0].Value := 42;
    PositionalParams[0].Obj := nil;
    PositionalParams[1].Value := 'Ada';
    PositionalParams[1].Obj := nil;
    OutSql := NormalizePositionalSql('SELECT * FROM t WHERE id = ? AND name = ?', PositionalParams, PositionalOrdered);
    AssertEqual('SELECT * FROM t WHERE id = $1 AND name = $2', OutSql, 'positional');
    AssertTrue(Length(PositionalOrdered) = 2, 'positional ordered count');

    SetLength(NamedNames, 2);
    NamedNames[0] := 'name';
    NamedNames[1] := 'active';
    SetLength(NamedParams, 2);
    NamedParams[0].Value := 'Ada';
    NamedParams[0].Obj := nil;
    NamedParams[1].Value := True;
    NamedParams[1].Obj := nil;
    OutSql := NormalizeNamedSql('SELECT * FROM users WHERE name = @name AND active = :active', NamedNames, NamedParams, NamedOrdered);
    AssertEqual('SELECT * FROM users WHERE name = $1 AND active = $2', OutSql, 'named');
    AssertTrue(Length(NamedOrdered) = 2, 'named ordered count');

    SetLength(NamedNames, 1);
    NamedNames[0] := 'id';
    SetLength(NamedParams, 1);
    NamedParams[0].Value := 7;
    NamedParams[0].Obj := nil;
    OutSql := NormalizeNamedSql('SELECT :id::INTEGER AS v', NamedNames, NamedParams, NamedOrdered);
    AssertEqual('SELECT $1::INTEGER AS v', OutSql, 'named cast syntax');
    Writeln('SqlTests: OK');
  except
    on E: Exception do
    begin
      Writeln('SqlTests: FAILED - ' + E.Message);
      Halt(1);
    end;
  end;
end.
