// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

unit ScratchBird.Common;

{$mode delphi}
{$H+}

interface

uses
  SysUtils, Classes, Variants, Contnrs, ScratchBird.Client, ScratchBird.Protocol;

type
  TScratchBirdParam = class
  private
    FName: string;
    FValue: Variant;
    FObjectValue: TObject;
  public
    property Name: string read FName write FName;
    property Value: Variant read FValue write FValue;
    property ObjectValue: TObject read FObjectValue write FObjectValue;
  end;

  TScratchBirdParams = class
  private
    FItems: TObjectList;
    function GetCount: Integer;
    function GetItem(Index: Integer): TScratchBirdParam;
  public
    constructor Create;
    destructor Destroy; override;
    function ParamByName(const Name: string): TScratchBirdParam;
    procedure Clear;
    property Count: Integer read GetCount;
    property Items[Index: Integer]: TScratchBirdParam read GetItem; default;
  end;

  TScratchBirdQueryResult = class
  private
    FStream: TScratchBirdResultStream;
    FCurrentRow: TArray<Variant>;
  public
    constructor Create(Stream: TScratchBirdResultStream);
    function Next: Boolean;
    function Eof: Boolean;
    function FieldByName(const Name: string): Variant;
    function RowsAffected: Int64;
    function ColumnCount: Integer;
    function ColumnName(Index: Integer): string;
  end;

implementation

constructor TScratchBirdParams.Create;
begin
  inherited Create;
  FItems := TObjectList.Create(True);
end;

destructor TScratchBirdParams.Destroy;
begin
  FItems.Free;
  inherited Destroy;
end;

function TScratchBirdParams.GetCount: Integer;
begin
  Result := FItems.Count;
end;

function TScratchBirdParams.GetItem(Index: Integer): TScratchBirdParam;
begin
  Result := TScratchBirdParam(FItems[Index]);
end;

procedure TScratchBirdParams.Clear;
begin
  FItems.Clear;
end;

function TScratchBirdParams.ParamByName(const Name: string): TScratchBirdParam;
var
  I: Integer;
  Item: TScratchBirdParam;
begin
  for I := 0 to FItems.Count - 1 do
  begin
    Item := TScratchBirdParam(FItems[I]);
    if SameText(Item.Name, Name) then
      Exit(Item);
  end;
  Item := TScratchBirdParam.Create;
  Item.Name := Name;
  FItems.Add(Item);
  Result := Item;
end;

constructor TScratchBirdQueryResult.Create(Stream: TScratchBirdResultStream);
begin
  inherited Create;
  FStream := Stream;
end;

function TScratchBirdQueryResult.Next: Boolean;
begin
  FCurrentRow := FStream.ReadRow;
  Result := Length(FCurrentRow) > 0;
end;

function TScratchBirdQueryResult.Eof: Boolean;
begin
  Result := Length(FCurrentRow) = 0;
end;

function TScratchBirdQueryResult.FieldByName(const Name: string): Variant;
var
  I: Integer;
begin
  Result := Null;
  for I := 0 to High(FStream.Columns) do
  begin
    if SameText(FStream.Columns[I].Name, Name) then
      Exit(FCurrentRow[I]);
  end;
end;

function TScratchBirdQueryResult.RowsAffected: Int64;
begin
  Result := FStream.RowsAffected;
end;

function TScratchBirdQueryResult.ColumnCount: Integer;
begin
  Result := Length(FStream.Columns);
end;

function TScratchBirdQueryResult.ColumnName(Index: Integer): string;
begin
  if (Index >= 0) and (Index < Length(FStream.Columns)) then
    Result := FStream.Columns[Index].Name
  else
    Result := '';
end;

end.
