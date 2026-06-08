// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

unit ScratchBird.Sql;

{$mode delphi}
{$H+}

interface

uses
  SysUtils, Variants;

type
  TScratchBirdParamInput = record
    Value: Variant;
    Obj: TObject;
  end;

function NormalizePositionalSql(const Sql: string; const Params: array of TScratchBirdParamInput;
  out Ordered: TArray<TScratchBirdParamInput>): string;
function NormalizeNamedSql(const Sql: string; const Names: array of string; const Params: array of TScratchBirdParamInput;
  out Ordered: TArray<TScratchBirdParamInput>): string;

function IndexOf(const Names: array of string; const Name: string): Integer;

implementation

function IndexOf(const Names: array of string; const Name: string): Integer;
var
  I: Integer;
begin
  Result := -1;
  for I := 0 to High(Names) do
    if SameText(Names[I], Name) then
      Exit(I);
end;

function IsIdentChar(Ch: Char): Boolean;
begin
  Result := (Ch >= 'a') and (Ch <= 'z') or (Ch >= 'A') and (Ch <= 'Z') or
    (Ch >= '0') and (Ch <= '9') or (Ch = '_');
end;

function NormalizePositionalSql(const Sql: string; const Params: array of TScratchBirdParamInput;
  out Ordered: TArray<TScratchBirdParamInput>): string;
var
  I: Integer;
  OutSql: string;
  Index: Integer;
begin
  Ordered := nil;
  OutSql := '';
  Index := 0;
  I := 1;
  while I <= Length(Sql) do
  begin
    if Sql[I] = '''' then
    begin
      OutSql := OutSql + Sql[I];
      Inc(I);
      while I <= Length(Sql) do
      begin
        OutSql := OutSql + Sql[I];
        if Sql[I] = '''' then
        begin
          if (I < Length(Sql)) and (Sql[I + 1] = '''') then
          begin
            Inc(I);
            OutSql := OutSql + Sql[I];
          end
          else
          begin
            Inc(I);
            Break;
          end;
        end;
        Inc(I);
      end;
      Continue;
    end;
    if Sql[I] = '?' then
    begin
      if Index > High(Params) then
        raise Exception.Create('not enough parameters');
      SetLength(Ordered, Length(Ordered) + 1);
      Ordered[High(Ordered)] := Params[Index];
      Inc(Index);
      OutSql := OutSql + '$' + IntToStr(Length(Ordered));
      Inc(I);
      Continue;
    end;
    OutSql := OutSql + Sql[I];
    Inc(I);
  end;
  if Index <= High(Params) then
  begin
    if Pos('?', Sql) > 0 then
      raise Exception.Create('too many parameters');
    SetLength(Ordered, Length(Params));
    for I := 0 to High(Params) do
      Ordered[I] := Params[I];
    Result := Sql;
    Exit;
  end;
  Result := OutSql;
end;

function NormalizeNamedSql(const Sql: string; const Names: array of string; const Params: array of TScratchBirdParamInput;
  out Ordered: TArray<TScratchBirdParamInput>): string;
var
  I, J: Integer;
  OutSql: string;
  Token: string;
  Index: Integer;
begin
  Ordered := nil;
  OutSql := '';
  I := 1;
  while I <= Length(Sql) do
  begin
    if Sql[I] = '''' then
    begin
      OutSql := OutSql + Sql[I];
      Inc(I);
      while I <= Length(Sql) do
      begin
        OutSql := OutSql + Sql[I];
        if Sql[I] = '''' then
        begin
          if (I < Length(Sql)) and (Sql[I + 1] = '''') then
          begin
            Inc(I);
            OutSql := OutSql + Sql[I];
          end
          else
          begin
            Inc(I);
            Break;
          end;
        end;
        Inc(I);
      end;
      Continue;
    end;
    if (Sql[I] = ':') and (I < Length(Sql)) and (Sql[I + 1] = ':') then
    begin
      OutSql := OutSql + '::';
      Inc(I, 2);
      Continue;
    end;
    if ((Sql[I] = '@') or (Sql[I] = ':')) and (I < Length(Sql)) and IsIdentChar(Sql[I + 1]) then
    begin
      J := I + 1;
      while (J <= Length(Sql)) and IsIdentChar(Sql[J]) do
        Inc(J);
      Token := Copy(Sql, I + 1, J - I - 1);
      Index := IndexOf(Names, Token);
      if Index < 0 then
        raise Exception.Create('missing named parameter: ' + Token);
      SetLength(Ordered, Length(Ordered) + 1);
      Ordered[High(Ordered)] := Params[Index];
      OutSql := OutSql + '$' + IntToStr(Length(Ordered));
      I := J;
      Continue;
    end;
    OutSql := OutSql + Sql[I];
    Inc(I);
  end;
  Result := OutSql;
end;

end.
