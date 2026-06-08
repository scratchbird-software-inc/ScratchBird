// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

unit ScratchBird.Types;

{$mode delphi}
{$H+}

interface

uses
  SysUtils, Classes, Variants, DateUtils, ScratchBird.Protocol;

const
  FORMAT_TEXT = 0;
  FORMAT_BINARY = 1;

  OID_BOOL = 16;
  OID_BYTEA = 17;
  OID_CHAR = 18;
  OID_INT8 = 20;
  OID_INT2 = 21;
  OID_INT4 = 23;
  OID_TEXT = 25;
  OID_JSON = 114;
  OID_XML = 142;
  OID_POINT = 600;
  OID_LSEG = 601;
  OID_PATH = 602;
  OID_BOX = 603;
  OID_POLYGON = 604;
  OID_LINE = 628;
  OID_FLOAT4 = 700;
  OID_FLOAT8 = 701;
  OID_CIRCLE = 718;
  OID_MONEY = 790;
  OID_MACADDR = 829;
  OID_CIDR = 650;
  OID_INET = 869;
  OID_MACADDR8 = 774;
  OID_BPCHAR = 1042;
  OID_VARCHAR = 1043;
  OID_DATE = 1082;
  OID_TIME = 1083;
  OID_TIMESTAMP = 1114;
  OID_TIMESTAMPTZ = 1184;
  OID_INTERVAL = 1186;
  OID_TIMETZ = 1266;
  OID_NUMERIC = 1700;
  OID_UUID = 2950;
  OID_JSONB = 3802;
  OID_RECORD = 2249;
  OID_INT4RANGE = 3904;
  OID_NUMRANGE = 3906;
  OID_TSRANGE = 3908;
  OID_TSTZRANGE = 3910;
  OID_DATERANGE = 3912;
  OID_INT8RANGE = 3926;
  OID_TSVECTOR = 3614;
  OID_TSQUERY = 3615;
  OID_SB_VECTOR = 16386;

const
  RANGE_EMPTY = $01;
  RANGE_LB_INC = $02;
  RANGE_UB_INC = $04;
  RANGE_LB_INF = $08;
  RANGE_UB_INF = $10;

type
  TScratchBirdInterval = record
    Months: Integer;
    Days: Integer;
    Micros: Int64;
  end;

  IScratchBirdJsonb = interface
    ['{60F0F7C3-75E1-4A5A-9E25-48F1A64B0D0D}']
    function GetRaw: TBytes;
    function GetText: string;
  end;

  IScratchBirdGeometry = interface
    ['{2E3E8EE5-7A34-4E05-A70D-0D9FD5E4A8C1}']
    function GetWkb: TBytes;
    function GetSrid: Integer;
    function GetWkt: string;
    function GetGeometryOid: Cardinal;
  end;

  IScratchBirdRange = interface
    ['{4EEA7DE1-2E2B-45B8-9F97-4D0AFC7F4B1E}']
    function GetLower: Variant;
    function GetUpper: Variant;
    function GetLowerInclusive: Boolean;
    function GetUpperInclusive: Boolean;
    function GetLowerInfinite: Boolean;
    function GetUpperInfinite: Boolean;
    function GetEmpty: Boolean;
    function GetRangeOid: Cardinal;
  end;

  IScratchBirdComposite = interface
    ['{B2E9A9B5-9CC1-4C1E-9BA2-6C2C92C3B83B}']
    function GetFieldCount: Integer;
    function GetFieldOid(Index: Integer): Cardinal;
    function GetFieldValue(Index: Integer): Variant;
  end;

  TScratchBirdJsonb = class(TInterfacedObject, IScratchBirdJsonb)
  private
    FRaw: TBytes;
    FText: string;
  public
    constructor CreateRaw(const ARaw: TBytes);
    constructor CreateText(const AText: string);
    function GetRaw: TBytes;
    function GetText: string;
    property Raw: TBytes read FRaw;
    property Text: string read FText;
  end;

  TScratchBirdGeometry = class(TInterfacedObject, IScratchBirdGeometry)
  private
    FWkb: TBytes;
    FSrid: Integer;
    FWkt: string;
    FGeometryOid: Cardinal;
  public
    constructor Create(const AWkb: TBytes; ASrid: Integer = 0; const AWkt: string = ''; AGeometryOid: Cardinal = OID_POINT);
    function GetWkb: TBytes;
    function GetSrid: Integer;
    function GetWkt: string;
    function GetGeometryOid: Cardinal;
    property Wkb: TBytes read FWkb;
    property Srid: Integer read FSrid;
    property Wkt: string read FWkt;
    property GeometryOid: Cardinal read FGeometryOid;
  end;

  TScratchBirdRange = class(TInterfacedObject, IScratchBirdRange)
  private
    FLower: Variant;
    FUpper: Variant;
    FLowerInclusive: Boolean;
    FUpperInclusive: Boolean;
    FLowerInfinite: Boolean;
    FUpperInfinite: Boolean;
    FEmpty: Boolean;
    FRangeOid: Cardinal;
  public
    function GetLower: Variant;
    function GetUpper: Variant;
    function GetLowerInclusive: Boolean;
    function GetUpperInclusive: Boolean;
    function GetLowerInfinite: Boolean;
    function GetUpperInfinite: Boolean;
    function GetEmpty: Boolean;
    function GetRangeOid: Cardinal;
    property Lower: Variant read FLower write FLower;
    property Upper: Variant read FUpper write FUpper;
    property LowerInclusive: Boolean read FLowerInclusive write FLowerInclusive;
    property UpperInclusive: Boolean read FUpperInclusive write FUpperInclusive;
    property LowerInfinite: Boolean read FLowerInfinite write FLowerInfinite;
    property UpperInfinite: Boolean read FUpperInfinite write FUpperInfinite;
    property Empty: Boolean read FEmpty write FEmpty;
    property RangeOid: Cardinal read FRangeOid write FRangeOid;
  end;

  TScratchBirdComposite = class(TInterfacedObject, IScratchBirdComposite)
  private
    FFieldOids: TArray<Cardinal>;
    FFieldValues: TArray<Variant>;
  public
    constructor Create(const AOids: array of Cardinal; const AValues: array of Variant);
    function GetFieldCount: Integer;
    function GetFieldOid(Index: Integer): Cardinal;
    function GetFieldValue(Index: Integer): Variant;
    property FieldOids: TArray<Cardinal> read FFieldOids;
    property FieldValues: TArray<Variant> read FFieldValues;
  end;

function EncodeParam(const Value: Variant; Obj: TObject; out Param: TParamValue; out Oid: Cardinal): Boolean;
function DecodeValue(TypeOid: Cardinal; const Data: TBytes; Format: Word): Variant;

implementation

function ConcatBytes(const Left, Right: TBytes): TBytes;
begin
  SetLength(Result, Length(Left) + Length(Right));
  if Length(Left) > 0 then
    Move(Left[0], Result[0], Length(Left));
  if Length(Right) > 0 then
    Move(Right[0], Result[Length(Left)], Length(Right));
end;

function BytesOf(const Values: array of Byte): TBytes;
var
  I: Integer;
begin
  SetLength(Result, Length(Values));
  for I := 0 to High(Values) do
    Result[I] := Values[I];
end;

function WriteUInt32LE(Value: Cardinal): TBytes;
begin
  SetLength(Result, 4);
  Result[0] := Byte(Value and $FF);
  Result[1] := Byte((Value shr 8) and $FF);
  Result[2] := Byte((Value shr 16) and $FF);
  Result[3] := Byte((Value shr 24) and $FF);
end;

function WriteInt32LE(Value: Integer): TBytes;
begin
  SetLength(Result, 4);
  Result[0] := Byte(Value and $FF);
  Result[1] := Byte((Value shr 8) and $FF);
  Result[2] := Byte((Value shr 16) and $FF);
  Result[3] := Byte((Value shr 24) and $FF);
end;

function WriteInt64LE(Value: Int64): TBytes;
begin
  SetLength(Result, 8);
  Move(Value, Result[0], 8);
end;

function WriteDoubleLE(Value: Double): TBytes;
begin
  SetLength(Result, 8);
  Move(Value, Result[0], 8);
end;

function WriteSingleLE(Value: Single): TBytes;
begin
  SetLength(Result, 4);
  Move(Value, Result[0], 4);
end;

function HasBytes(const Data: TBytes; Offset, Count: Integer): Boolean;
begin
  Result := (Offset >= 0) and (Count >= 0) and
    (Offset <= System.Length(Data)) and
    (Count <= (System.Length(Data) - Offset));
end;

function IsGeometryOid(Value: Cardinal): Boolean;
begin
  Result := (Value = OID_POINT) or (Value = OID_LSEG) or (Value = OID_PATH) or
    (Value = OID_BOX) or (Value = OID_POLYGON) or (Value = OID_LINE) or
    (Value = OID_CIRCLE);
end;

function ReadUInt32LE(const Data: TBytes; Offset: Integer): Cardinal;
begin
  Result := Cardinal(Data[Offset]) or (Cardinal(Data[Offset + 1]) shl 8) or
    (Cardinal(Data[Offset + 2]) shl 16) or (Cardinal(Data[Offset + 3]) shl 24);
end;

function ReadInt32LE(const Data: TBytes; Offset: Integer): Integer;
begin
  Result := Integer(ReadUInt32LE(Data, Offset));
end;

function ReadUInt64LE(const Data: TBytes; Offset: Integer): UInt64;
begin
  Result := UInt64(Data[Offset]) or (UInt64(Data[Offset + 1]) shl 8) or
    (UInt64(Data[Offset + 2]) shl 16) or (UInt64(Data[Offset + 3]) shl 24) or
    (UInt64(Data[Offset + 4]) shl 32) or (UInt64(Data[Offset + 5]) shl 40) or
    (UInt64(Data[Offset + 6]) shl 48) or (UInt64(Data[Offset + 7]) shl 56);
end;

function ReadInt16LE(const Data: TBytes; Offset: Integer): SmallInt;
begin
  Result := SmallInt(Word(Data[Offset]) or (Word(Data[Offset + 1]) shl 8));
end;

function ReadInt64LE(const Data: TBytes; Offset: Integer): Int64;
begin
  Result := Int64(ReadUInt64LE(Data, Offset));
end;

function ReadSingleLE(const Data: TBytes; Offset: Integer): Single;
begin
  Move(Data[Offset], Result, SizeOf(Result));
end;

function ReadDoubleLE(const Data: TBytes; Offset: Integer): Double;
begin
  Move(Data[Offset], Result, SizeOf(Result));
end;

function IsHexChar(Ch: Char): Boolean;
begin
  Result := (Ch >= '0') and (Ch <= '9') or (Ch >= 'a') and (Ch <= 'f') or
    (Ch >= 'A') and (Ch <= 'F');
end;

function IsUuidString(const Value: string): Boolean;
var
  I: Integer;
begin
  Result := False;
  if Length(Value) <> 36 then
    Exit;
  if (Value[9] <> '-') or (Value[14] <> '-') or (Value[19] <> '-') or (Value[24] <> '-') then
    Exit;
  for I := 1 to Length(Value) do
  begin
    if (I = 9) or (I = 14) or (I = 19) or (I = 24) then
      Continue;
    if not IsHexChar(Value[I]) then
      Exit;
  end;
  Result := True;
end;

function HexToBytes(const Hex: string): TBytes;
var
  I, J: Integer;
  Hi, Lo: Integer;
  ByteVal: Byte;
  function HexVal(Ch: Char): Integer;
  begin
    if (Ch >= '0') and (Ch <= '9') then
      Exit(Ord(Ch) - Ord('0'));
    if (Ch >= 'a') and (Ch <= 'f') then
      Exit(Ord(Ch) - Ord('a') + 10);
    if (Ch >= 'A') and (Ch <= 'F') then
      Exit(Ord(Ch) - Ord('A') + 10);
    Result := 0;
  end;
begin
  SetLength(Result, Length(Hex) div 2);
  J := 0;
  for I := 1 to Length(Hex) div 2 do
  begin
    Hi := HexVal(Hex[(I - 1) * 2 + 1]);
    Lo := HexVal(Hex[(I - 1) * 2 + 2]);
    ByteVal := Byte((Hi shl 4) or Lo);
    Result[J] := ByteVal;
    Inc(J);
  end;
end;

constructor TScratchBirdJsonb.CreateRaw(const ARaw: TBytes);
begin
  inherited Create;
  FRaw := ARaw;
  FText := '';
end;

constructor TScratchBirdJsonb.CreateText(const AText: string);
begin
  inherited Create;
  FText := AText;
  FRaw := TEncoding.UTF8.GetBytes(AText);
end;

function TScratchBirdJsonb.GetRaw: TBytes;
begin
  Result := FRaw;
end;

function TScratchBirdJsonb.GetText: string;
begin
  Result := FText;
end;

constructor TScratchBirdGeometry.Create(const AWkb: TBytes; ASrid: Integer; const AWkt: string; AGeometryOid: Cardinal);
begin
  inherited Create;
  FWkb := AWkb;
  FSrid := ASrid;
  FWkt := AWkt;
  if IsGeometryOid(AGeometryOid) then
    FGeometryOid := AGeometryOid
  else
    FGeometryOid := OID_POINT;
end;

function TScratchBirdGeometry.GetWkb: TBytes;
begin
  Result := FWkb;
end;

function TScratchBirdGeometry.GetSrid: Integer;
begin
  Result := FSrid;
end;

function TScratchBirdGeometry.GetWkt: string;
begin
  Result := FWkt;
end;

function TScratchBirdGeometry.GetGeometryOid: Cardinal;
begin
  Result := FGeometryOid;
end;

function TScratchBirdRange.GetLower: Variant;
begin
  Result := FLower;
end;

function TScratchBirdRange.GetUpper: Variant;
begin
  Result := FUpper;
end;

function TScratchBirdRange.GetLowerInclusive: Boolean;
begin
  Result := FLowerInclusive;
end;

function TScratchBirdRange.GetUpperInclusive: Boolean;
begin
  Result := FUpperInclusive;
end;

function TScratchBirdRange.GetLowerInfinite: Boolean;
begin
  Result := FLowerInfinite;
end;

function TScratchBirdRange.GetUpperInfinite: Boolean;
begin
  Result := FUpperInfinite;
end;

function TScratchBirdRange.GetEmpty: Boolean;
begin
  Result := FEmpty;
end;

function TScratchBirdRange.GetRangeOid: Cardinal;
begin
  Result := FRangeOid;
end;

constructor TScratchBirdComposite.Create(const AOids: array of Cardinal; const AValues: array of Variant);
var
  I: Integer;
begin
  SetLength(FFieldOids, Length(AOids));
  for I := 0 to High(AOids) do
    FFieldOids[I] := AOids[I];
  SetLength(FFieldValues, Length(AValues));
  for I := 0 to High(AValues) do
    FFieldValues[I] := AValues[I];
end;

function TScratchBirdComposite.GetFieldCount: Integer;
begin
  Result := Length(FFieldValues);
end;

function TScratchBirdComposite.GetFieldOid(Index: Integer): Cardinal;
begin
  if (Index >= 0) and (Index < Length(FFieldOids)) then
    Result := FFieldOids[Index]
  else
    Result := 0;
end;

function TScratchBirdComposite.GetFieldValue(Index: Integer): Variant;
begin
  if (Index >= 0) and (Index < Length(FFieldValues)) then
    Result := FFieldValues[Index]
  else
    Result := Null;
end;

function EncodeLengthPrefixed(const Data: TBytes): TBytes;
begin
  Result := ConcatBytes(WriteUInt32LE(System.Length(Data)), Data);
end;

function StripLengthPrefixed(const Data: TBytes): TBytes;
var
  Len: Integer;
begin
  if System.Length(Data) < 4 then
    Exit(Data);
  Len := Integer(ReadUInt32LE(Data, 0));
  if Len <= System.Length(Data) - 4 then
  begin
    SetLength(Result, Len);
    Move(Data[4], Result[0], Len);
  end
  else
    Result := Data;
end;

function BytesToUuid(const Data: TBytes): string;
var
  Hex: string;
  I: Integer;
const
  HexChars: array[0..15] of Char = '0123456789abcdef';
begin
  SetLength(Hex, Length(Data) * 2);
  for I := 0 to Length(Data) - 1 do
  begin
    Hex[I * 2 + 1] := HexChars[Data[I] shr 4];
    Hex[I * 2 + 2] := HexChars[Data[I] and $F];
  end;
  if Length(Hex) <> 32 then
    Exit(Hex);
  Result := Copy(Hex, 1, 8) + '-' + Copy(Hex, 9, 4) + '-' + Copy(Hex, 13, 4) + '-' +
    Copy(Hex, 17, 4) + '-' + Copy(Hex, 21, 12);
end;

function EncodeDateValue(const Value: TDateTime): TBytes;
var
  Base: TDateTime;
  Days: Integer;
begin
  Base := EncodeDate(2000, 1, 1);
  Days := Trunc(Value) - Trunc(Base);
  Result := WriteInt32LE(Days);
end;

function EncodeTimestampValue(const Value: TDateTime): TBytes;
var
  Base: TDateTime;
  Micros: Int64;
begin
  Base := EncodeDate(2000, 1, 1);
  Micros := Trunc((Value - Base) * 86400 * 1000000);
  Result := WriteInt64LE(Micros);
end;

function EncodeTimeValue(const Value: TDateTime): TBytes;
var
  Micros: Int64;
begin
  Micros := Trunc(Frac(Value) * 86400 * 1000000);
  Result := WriteInt64LE(Micros);
end;

function NormalizeMicrosOfDay(Value: Int64): Int64;
const
  MICROS_PER_DAY = Int64(24 * 60 * 60 * 1000000);
begin
  Result := Value mod MICROS_PER_DAY;
  if Result < 0 then
    Inc(Result, MICROS_PER_DAY);
end;

function EncodeTimeTzValue(const TimeValue: TDateTime; OffsetSecondsEast: Integer): TBytes;
var
  Micros: Int64;
  ZoneSecondsWest: Integer;
begin
  Micros := Trunc(Frac(TimeValue) * 86400 * 1000000);
  ZoneSecondsWest := -OffsetSecondsEast;
  Result := ConcatBytes(WriteInt64LE(Micros), WriteInt32LE(ZoneSecondsWest));
end;

function FormatArrayLiteral(const Values: array of Variant): string;
var
  I: Integer;
  Item: string;
begin
  Result := '{';
  for I := 0 to High(Values) do
  begin
    if VarIsNull(Values[I]) then
      Item := 'NULL'
    else if VarType(Values[I]) in [varSmallint, varInteger, varInt64, varSingle, varDouble, varCurrency] then
      Item := VarToStr(Values[I])
    else if VarType(Values[I]) = varBoolean then
      Item := LowerCase(VarToStr(Values[I]))
    else
      Item := '"' + StringReplace(VarToStr(Values[I]), '"', '\"', [rfReplaceAll]) + '"';
    if I > 0 then
      Result := Result + ',';
    Result := Result + Item;
  end;
  Result := Result + '}';
end;

function FormatVectorLiteral(const Values: array of Variant): string;
var
  I: Integer;
begin
  Result := '[';
  for I := 0 to High(Values) do
  begin
    if I > 0 then
      Result := Result + ',';
    Result := Result + VarToStr(Values[I]);
  end;
  Result := Result + ']';
end;

function TryGetVariantArray(const Value: Variant; out Items: TArray<Variant>): Boolean;
var
  L, H, I: Integer;
begin
  Result := False;
  if not VarIsArray(Value) then
    Exit;
  L := VarArrayLowBound(Value, 1);
  H := VarArrayHighBound(Value, 1);
  SetLength(Items, H - L + 1);
  for I := L to H do
    Items[I - L] := Value[I];
  Result := True;
end;

function EncodeRangeBound(RangeOid: Cardinal; const Bound: Variant): TBytes;
begin
  case RangeOid of
    OID_INT4RANGE:
      Result := WriteInt32LE(VarAsType(Bound, varInteger));
    OID_INT8RANGE:
      Result := WriteInt64LE(VarAsType(Bound, varInt64));
    OID_NUMRANGE:
      Result := EncodeLengthPrefixed(TEncoding.UTF8.GetBytes(VarToStr(Bound)));
    OID_TSRANGE, OID_TSTZRANGE:
      Result := EncodeLengthPrefixed(EncodeTimestampValue(VarToDateTime(Bound)));
    OID_DATERANGE:
      Result := EncodeLengthPrefixed(EncodeDateValue(VarToDateTime(Bound)));
  else
    Result := EncodeLengthPrefixed(TEncoding.UTF8.GetBytes(VarToStr(Bound)));
  end;
end;

function ResolveRangeOid(const Range: TScratchBirdRange): Cardinal;
begin
  if Range.RangeOid <> 0 then
    Exit(Range.RangeOid);
  if not VarIsNull(Range.Lower) then
  begin
    if VarType(Range.Lower) = varDate then
      Exit(OID_DATERANGE);
    if VarType(Range.Lower) in [varInteger, varSmallint, varInt64] then
      Exit(OID_INT8RANGE);
  end;
  if not VarIsNull(Range.Upper) then
  begin
    if VarType(Range.Upper) = varDate then
      Exit(OID_DATERANGE);
  end;
  Result := OID_NUMRANGE;
end;

function EncodeRange(const Range: TScratchBirdRange; out RangeOid: Cardinal): TBytes;
var
  Flags: Byte;
  Parts: TBytes;
  Bound: TBytes;
begin
  RangeOid := ResolveRangeOid(Range);
  Flags := 0;
  if Range.Empty then
    Flags := Flags or RANGE_EMPTY;
  if Range.LowerInclusive then
    Flags := Flags or RANGE_LB_INC;
  if Range.UpperInclusive then
    Flags := Flags or RANGE_UB_INC;
  if Range.LowerInfinite then
    Flags := Flags or RANGE_LB_INF;
  if Range.UpperInfinite then
    Flags := Flags or RANGE_UB_INF;
  Parts := BytesOf([Flags, 0, 0, 0]);
  if not Range.Empty and not Range.LowerInfinite then
  begin
    Bound := EncodeRangeBound(RangeOid, Range.Lower);
    Parts := ConcatBytes(Parts, WriteUInt32LE(System.Length(Bound)));
    Parts := ConcatBytes(Parts, Bound);
  end;
  if not Range.Empty and not Range.UpperInfinite then
  begin
    Bound := EncodeRangeBound(RangeOid, Range.Upper);
    Parts := ConcatBytes(Parts, WriteUInt32LE(System.Length(Bound)));
    Parts := ConcatBytes(Parts, Bound);
  end;
  Result := Parts;
end;

function EncodeComposite(const Comp: TScratchBirdComposite; out TypeOid: Cardinal): TBytes;
var
  FieldCount: Integer;
  I: Integer;
  FieldOid: Cardinal;
  EncodedOid: Cardinal;
  FieldParam: TParamValue;
  FieldBytes: TBytes;
begin
  FieldCount := Length(Comp.FieldValues);
  TypeOid := OID_RECORD;
  Result := WriteInt32LE(FieldCount);
  for I := 0 to FieldCount - 1 do
  begin
    FieldOid := 0;
    if I < Length(Comp.FieldOids) then
      FieldOid := Comp.FieldOids[I];

    if VarIsNull(Comp.FieldValues[I]) or VarIsEmpty(Comp.FieldValues[I]) then
    begin
      Result := ConcatBytes(Result, WriteUInt32LE(FieldOid));
      Result := ConcatBytes(Result, WriteInt32LE(-1));
      Continue;
    end;

    if not EncodeParam(Comp.FieldValues[I], nil, FieldParam, EncodedOid) then
      raise Exception.Create('Failed to encode composite field');

    if FieldOid = 0 then
      FieldOid := EncodedOid;
    if FieldOid = 0 then
      raise Exception.Create('Composite field OID is required');

    Result := ConcatBytes(Result, WriteUInt32LE(FieldOid));
    if FieldParam.IsNull or (FieldParam.Data = nil) then
    begin
      Result := ConcatBytes(Result, WriteInt32LE(-1));
      Continue;
    end;
    FieldBytes := FieldParam.Data;
    Result := ConcatBytes(Result, WriteInt32LE(System.Length(FieldBytes)));
    Result := ConcatBytes(Result, FieldBytes);
  end;
end;

function DecodeComposite(const Data: TBytes): IInterface;
var
  Count: Integer;
  Offset: Integer;
  I: Integer;
  FieldOid: Cardinal;
  FieldLen: Integer;
  FieldData: TBytes;
  FieldOids: TArray<Cardinal>;
  FieldValues: TArray<Variant>;
begin
  if System.Length(Data) < 4 then
    Exit(nil);
  Count := ReadInt32LE(Data, 0);
  if Count < 0 then
    Exit(nil);
  Offset := 4;
  SetLength(FieldOids, Count);
  SetLength(FieldValues, Count);
  for I := 0 to Count - 1 do
  begin
    if Offset + 8 > System.Length(Data) then
      Exit(nil);
    FieldOid := ReadUInt32LE(Data, Offset);
    Inc(Offset, 4);
    FieldLen := ReadInt32LE(Data, Offset);
    Inc(Offset, 4);
    FieldOids[I] := FieldOid;
    if FieldLen < 0 then
    begin
      FieldValues[I] := Null;
      Continue;
    end;
    if Offset + FieldLen > System.Length(Data) then
      Exit(nil);
    SetLength(FieldData, FieldLen);
    if FieldLen > 0 then
      Move(Data[Offset], FieldData[0], FieldLen);
    Inc(Offset, FieldLen);
    FieldValues[I] := DecodeValue(FieldOid, FieldData, FORMAT_BINARY);
  end;
  Result := IInterface(TScratchBirdComposite.Create(FieldOids, FieldValues));
end;

function EncodeParam(const Value: Variant; Obj: TObject; out Param: TParamValue; out Oid: Cardinal): Boolean;
var
  Items: TArray<Variant>;
  AllNumeric: Boolean;
  I: Integer;
  TextValue: string;
  Range: TScratchBirdRange;
  Jsonb: TScratchBirdJsonb;
  Geometry: TScratchBirdGeometry;
  Composite: TScratchBirdComposite;
  JsonbIntf: IScratchBirdJsonb;
  GeometryIntf: IScratchBirdGeometry;
  RangeIntf: IScratchBirdRange;
  CompositeIntf: IScratchBirdComposite;
  FieldOids: TArray<Cardinal>;
  FieldValues: TArray<Variant>;
  FieldCount: Integer;
  IntValue64: Int64;
begin
  Param.Format := FORMAT_BINARY;
  Param.IsNull := False;
  Param.Data := nil;
  Oid := 0;
  Result := True;

  if Obj is TScratchBirdJsonb then
  begin
    Jsonb := TScratchBirdJsonb(Obj);
    if System.Length(Jsonb.Raw) = 0 then
      raise Exception.Create('JSONB requires raw payload');
    Param.Data := EncodeLengthPrefixed(Jsonb.Raw);
    Oid := OID_JSONB;
    Exit;
  end;

  if Obj is TScratchBirdGeometry then
  begin
    Geometry := TScratchBirdGeometry(Obj);
    if System.Length(Geometry.Wkb) = 0 then
      raise Exception.Create('geometry requires WKB payload');
    Param.Data := EncodeLengthPrefixed(Geometry.Wkb);
    Oid := Geometry.GeometryOid;
    Exit;
  end;

  if Obj is TScratchBirdRange then
  begin
    Range := TScratchBirdRange(Obj);
    Param.Data := EncodeRange(Range, Oid);
    Exit;
  end;

  if Obj is TScratchBirdComposite then
  begin
    Composite := TScratchBirdComposite(Obj);
    Param.Data := EncodeComposite(Composite, Oid);
    Exit;
  end;

  if (VarType(Value) = varUnknown) and Supports(IInterface(Value), IScratchBirdJsonb, JsonbIntf) then
  begin
    Param.Data := EncodeLengthPrefixed(JsonbIntf.GetRaw);
    Oid := OID_JSONB;
    Exit;
  end;

  if (VarType(Value) = varUnknown) and Supports(IInterface(Value), IScratchBirdGeometry, GeometryIntf) then
  begin
    Param.Data := EncodeLengthPrefixed(GeometryIntf.GetWkb);
    Oid := GeometryIntf.GetGeometryOid;
    if not IsGeometryOid(Oid) then
      Oid := OID_POINT;
    Exit;
  end;

  if (VarType(Value) = varUnknown) and Supports(IInterface(Value), IScratchBirdRange, RangeIntf) then
  begin
    Range := TScratchBirdRange.Create;
    Range.Lower := RangeIntf.GetLower;
    Range.Upper := RangeIntf.GetUpper;
    Range.LowerInclusive := RangeIntf.GetLowerInclusive;
    Range.UpperInclusive := RangeIntf.GetUpperInclusive;
    Range.LowerInfinite := RangeIntf.GetLowerInfinite;
    Range.UpperInfinite := RangeIntf.GetUpperInfinite;
    Range.Empty := RangeIntf.GetEmpty;
    Range.RangeOid := RangeIntf.GetRangeOid;
    Param.Data := EncodeRange(Range, Oid);
    Range.Free;
    Exit;
  end;

  if (VarType(Value) = varUnknown) and Supports(IInterface(Value), IScratchBirdComposite, CompositeIntf) then
  begin
    FieldCount := CompositeIntf.GetFieldCount;
    SetLength(FieldOids, FieldCount);
    SetLength(FieldValues, FieldCount);
    for I := 0 to FieldCount - 1 do
    begin
      FieldOids[I] := CompositeIntf.GetFieldOid(I);
      FieldValues[I] := CompositeIntf.GetFieldValue(I);
    end;
    Composite := TScratchBirdComposite.Create(FieldOids, FieldValues);
    Param.Data := EncodeComposite(Composite, Oid);
    Composite.Free;
    Exit;
  end;

  if VarIsNull(Value) or VarIsEmpty(Value) then
  begin
    Param.IsNull := True;
    Oid := 0;
    Exit;
  end;

  case VarType(Value) of
    varBoolean:
      begin
        Param.Data := BytesOf([Ord(Boolean(Value))]);
        Oid := OID_BOOL;
      end;
    varByte, varShortInt, varWord, varLongWord, varSmallint, varInteger:
      begin
        IntValue64 := VarAsType(Value, varInt64);
        if (IntValue64 >= Low(Integer)) and (IntValue64 <= High(Integer)) then
        begin
          Param.Data := WriteInt32LE(Integer(IntValue64));
          Oid := OID_INT4;
        end
        else
        begin
          Param.Data := WriteInt64LE(IntValue64);
          Oid := OID_INT8;
        end;
      end;
    varInt64:
      begin
        Param.Data := WriteInt64LE(VarAsType(Value, varInt64));
        Oid := OID_INT8;
      end;
    varSingle:
      begin
        Param.Data := WriteSingleLE(VarAsType(Value, varSingle));
        Oid := OID_FLOAT4;
      end;
    varDouble:
      begin
        Param.Data := WriteDoubleLE(VarAsType(Value, varDouble));
        Oid := OID_FLOAT8;
      end;
    varCurrency:
      begin
        Param.Data := EncodeLengthPrefixed(TEncoding.UTF8.GetBytes(VarToStr(Value)));
        Oid := OID_NUMERIC;
      end;
    varString, varUString, varOleStr:
      begin
        TextValue := VarToStr(Value);
        if IsUuidString(TextValue) then
        begin
          TextValue := StringReplace(TextValue, '-', '', [rfReplaceAll]);
          Param.Data := HexToBytes(TextValue);
          Oid := OID_UUID;
          Exit;
        end;
        Param.Data := EncodeLengthPrefixed(TEncoding.UTF8.GetBytes(TextValue));
        Oid := OID_TEXT;
      end;
    varDate:
      begin
        Param.Data := EncodeTimestampValue(VarToDateTime(Value));
        Oid := OID_TIMESTAMPTZ;
      end;
  else
    if TryGetVariantArray(Value, Items) then
    begin
      if (System.Length(Items) = 2) and
         (VarType(Items[0]) = varDate) and
         (VarType(Items[1]) in [varByte, varShortInt, varWord, varLongWord, varSmallint, varInteger, varInt64, varSingle, varDouble, varCurrency]) then
      begin
        Param.Data := EncodeTimeTzValue(VarToDateTime(Items[0]), VarAsType(Items[1], varInteger));
        Oid := OID_TIMETZ;
      end
      else
      begin
        AllNumeric := True;
        for I := 0 to High(Items) do
        begin
          if not (VarType(Items[I]) in [varByte, varShortInt, varWord, varLongWord, varSmallint, varInteger, varInt64, varSingle, varDouble, varCurrency]) then
          begin
            AllNumeric := False;
            Break;
          end;
        end;
        if AllNumeric then
        begin
          Param.Data := EncodeLengthPrefixed(TEncoding.UTF8.GetBytes(FormatVectorLiteral(Items)));
          Oid := OID_SB_VECTOR;
        end
        else
        begin
          Param.Data := EncodeLengthPrefixed(TEncoding.UTF8.GetBytes(FormatArrayLiteral(Items)));
          Oid := 0;
        end;
      end;
    end
    else
    begin
      Param.Data := EncodeLengthPrefixed(TEncoding.UTF8.GetBytes(VarToStr(Value)));
      Oid := OID_TEXT;
    end;
  end;
end;

function DecodeInterval(const Data: TBytes): Variant;
var
  Months: Integer;
  Days: Integer;
  Micros: Int64;
begin
  if not HasBytes(Data, 0, 16) then
    Exit(Null);
  Micros := Int64(ReadUInt64LE(Data, 0));
  Days := ReadInt32LE(Data, 8);
  Months := ReadInt32LE(Data, 12);
  Result := VarArrayOf([Months, Days, Micros]);
end;

function DecodeDateValue(const Data: TBytes): TDateTime;
var
  Days: Integer;
begin
  Days := ReadInt32LE(Data, 0);
  Result := EncodeDate(2000, 1, 1) + Days;
end;

function DecodeTimeValue(const Data: TBytes): TDateTime;
var
  Micros: Int64;
begin
  Micros := Int64(ReadUInt64LE(Data, 0));
  Result := EncodeTime(0, 0, 0, 0) + (Micros / 86400 / 1000000);
end;

function DecodeTimeTzValue(const Data: TBytes): Variant;
var
  Micros: Int64;
  ZoneSecondsWest: Integer;
  TimeValue: TDateTime;
begin
  if System.Length(Data) < 8 then
    Exit(Null);
  Micros := NormalizeMicrosOfDay(ReadInt64LE(Data, 0));
  TimeValue := EncodeTime(0, 0, 0, 0) + (Micros / 86400 / 1000000);
  if System.Length(Data) >= 12 then
    ZoneSecondsWest := ReadInt32LE(Data, 8)
  else
    ZoneSecondsWest := 0;
  Result := VarArrayOf([TimeValue, -ZoneSecondsWest]);
end;

function DecodeTimestampValue(const Data: TBytes): TDateTime;
var
  Micros: Int64;
  Base: TDateTime;
begin
  Micros := Int64(ReadUInt64LE(Data, 0));
  Base := EncodeDate(2000, 1, 1);
  Result := Base + (Micros / 86400 / 1000000);
end;

function DecodeRangeBound(RangeOid: Cardinal; const Data: TBytes): Variant;
var
  Payload: TBytes;
begin
  case RangeOid of
    OID_INT4RANGE:
      if HasBytes(Data, 0, 4) then
        Result := ReadInt32LE(Data, 0)
      else
        Result := Null;
    OID_INT8RANGE:
      if HasBytes(Data, 0, 8) then
        Result := Int64(ReadUInt64LE(Data, 0))
      else
        Result := Null;
    OID_NUMRANGE:
      Result := TEncoding.UTF8.GetString(StripLengthPrefixed(Data));
    OID_TSRANGE, OID_TSTZRANGE:
      begin
        Payload := StripLengthPrefixed(Data);
        if not HasBytes(Payload, 0, 8) then
          Exit(Null);
        Result := DecodeTimestampValue(Payload);
      end;
    OID_DATERANGE:
      begin
        Payload := StripLengthPrefixed(Data);
        if not HasBytes(Payload, 0, 4) then
          Exit(Null);
        Result := DecodeDateValue(Payload);
      end;
  else
    Result := TEncoding.UTF8.GetString(StripLengthPrefixed(Data));
  end;
end;

function DecodeRange(const Data: TBytes; RangeOid: Cardinal): Variant;
var
  Flags: Byte;
  Offset: Integer;
  Len: Integer;
  Range: TScratchBirdRange;
  Bound: TBytes;
  BoundValue: Variant;
begin
  if not HasBytes(Data, 0, 4) then
    Exit(Null);
  Flags := Data[0];
  Offset := 4;
  Range := TScratchBirdRange.Create;
  Range.RangeOid := RangeOid;
  Range.Empty := (Flags and RANGE_EMPTY) <> 0;
  Range.LowerInclusive := (Flags and RANGE_LB_INC) <> 0;
  Range.UpperInclusive := (Flags and RANGE_UB_INC) <> 0;
  Range.LowerInfinite := (Flags and RANGE_LB_INF) <> 0;
  Range.UpperInfinite := (Flags and RANGE_UB_INF) <> 0;

  if not Range.Empty and not Range.LowerInfinite then
  begin
    if not HasBytes(Data, Offset, 4) then
      Exit(Null);
    Len := ReadInt32LE(Data, Offset);
    if (Len < 0) or (not HasBytes(Data, Offset + 4, Len)) then
      Exit(Null);
    Offset := Offset + 4;
    SetLength(Bound, Len);
    if Len > 0 then
      Move(Data[Offset], Bound[0], Len);
    Offset := Offset + Len;
    BoundValue := DecodeRangeBound(RangeOid, Bound);
    if VarIsNull(BoundValue) or VarIsEmpty(BoundValue) then
      Exit(Null);
    Range.Lower := BoundValue;
  end;

  if not Range.Empty and not Range.UpperInfinite then
  begin
    if not HasBytes(Data, Offset, 4) then
      Exit(Null);
    Len := ReadInt32LE(Data, Offset);
    if (Len < 0) or (not HasBytes(Data, Offset + 4, Len)) then
      Exit(Null);
    Offset := Offset + 4;
    SetLength(Bound, Len);
    if Len > 0 then
      Move(Data[Offset], Bound[0], Len);
    Offset := Offset + Len;
    BoundValue := DecodeRangeBound(RangeOid, Bound);
    if VarIsNull(BoundValue) or VarIsEmpty(BoundValue) then
      Exit(Null);
    Range.Upper := BoundValue;
  end;

  Result := IInterface(Range);
end;

function ParseVectorLiteral(const Text: string): Variant;
var
  Trimmed: string;
  Parts: TStringList;
  I: Integer;
  NumericValue: Double;
begin
  Trimmed := Trim(Text);
  if (Length(Trimmed) >= 2) and (Trimmed[1] = '[') and (Trimmed[Length(Trimmed)] = ']') then
    Trimmed := Copy(Trimmed, 2, Length(Trimmed) - 2);
  Trimmed := Trim(Trimmed);
  if Trimmed = '' then
    Exit(Null);
  if (Trimmed[1] = ',') or (Trimmed[Length(Trimmed)] = ',') or (Pos(',,', Trimmed) > 0) then
    Exit(Null);
  Parts := TStringList.Create;
  try
    ExtractStrings([','], [], PChar(Trimmed), Parts);
    if Parts.Count = 0 then
      Exit(Null);
    Result := VarArrayCreate([0, Parts.Count - 1], varDouble);
    for I := 0 to Parts.Count - 1 do
    begin
      if not TryStrToFloat(Trim(Parts[I]), NumericValue) then
        Exit(Null);
      Result[I] := NumericValue;
    end;
  finally
    Parts.Free;
  end;
end;

function ParseUnknownText(const Text: string): Variant; forward;
function DecodeUnknownBinary(const Data: TBytes): Variant; forward;

function DecodeValue(TypeOid: Cardinal; const Data: TBytes; Format: Word): Variant;
var
  Jsonb: TScratchBirdJsonb;
  Geometry: TScratchBirdGeometry;
  Composite: IInterface;
begin
  if System.Length(Data) = 0 then
    Exit(Null);
  if TypeOid = 0 then
  begin
    if Format = FORMAT_TEXT then
      Exit(ParseUnknownText(TEncoding.UTF8.GetString(Data)));
    Exit(DecodeUnknownBinary(Data));
  end;
  if Format = FORMAT_TEXT then
    Exit(TEncoding.UTF8.GetString(Data));

  case TypeOid of
    OID_BOOL: Result := Data[0] = 1;
    OID_INT2:
      if HasBytes(Data, 0, 2) then
        Result := ReadInt16LE(Data, 0)
      else
        Result := Null;
    OID_INT4:
      if HasBytes(Data, 0, 4) then
        Result := Integer(ReadUInt32LE(Data, 0))
      else
        Result := Null;
    OID_INT8:
      if HasBytes(Data, 0, 8) then
        Result := Int64(ReadUInt64LE(Data, 0))
      else
        Result := Null;
    OID_FLOAT4:
      if HasBytes(Data, 0, 4) then
        Result := ReadSingleLE(Data, 0)
      else
        Result := Null;
    OID_FLOAT8:
      if HasBytes(Data, 0, 8) then
        Result := ReadDoubleLE(Data, 0)
      else
        Result := Null;
    OID_NUMERIC: Result := TEncoding.UTF8.GetString(StripLengthPrefixed(Data));
    OID_MONEY:
      if HasBytes(Data, 0, 8) then
        Result := ReadInt64LE(Data, 0) / 100
      else
        Result := Null;
    OID_TEXT, OID_VARCHAR, OID_CHAR, OID_BPCHAR, OID_JSON, OID_XML, OID_TSVECTOR, OID_TSQUERY,
    OID_INET, OID_CIDR, OID_MACADDR, OID_MACADDR8:
      Result := TEncoding.UTF8.GetString(StripLengthPrefixed(Data));
    OID_JSONB:
      begin
        Jsonb := TScratchBirdJsonb.CreateRaw(StripLengthPrefixed(Data));
        Result := IInterface(Jsonb);
      end;
    OID_BYTEA:
      Result := StripLengthPrefixed(Data);
    OID_DATE:
      if HasBytes(Data, 0, 4) then
        Result := DecodeDateValue(Data)
      else
        Result := Null;
    OID_TIME:
      if HasBytes(Data, 0, 8) then
        Result := DecodeTimeValue(Data)
      else
        Result := Null;
    OID_TIMETZ:
      Result := DecodeTimeTzValue(Data);
    OID_TIMESTAMP, OID_TIMESTAMPTZ:
      if HasBytes(Data, 0, 8) then
        Result := DecodeTimestampValue(Data)
      else
        Result := Null;
    OID_INTERVAL:
      Result := DecodeInterval(Data);
    OID_UUID:
      Result := BytesToUuid(Data);
    OID_INT4RANGE, OID_INT8RANGE, OID_NUMRANGE, OID_TSRANGE, OID_TSTZRANGE, OID_DATERANGE:
      Result := DecodeRange(Data, TypeOid);
    OID_SB_VECTOR:
      Result := ParseVectorLiteral(TEncoding.UTF8.GetString(StripLengthPrefixed(Data)));
    OID_POINT, OID_LSEG, OID_PATH, OID_BOX, OID_POLYGON, OID_LINE, OID_CIRCLE:
      begin
        Geometry := TScratchBirdGeometry.Create(StripLengthPrefixed(Data), 0, '', TypeOid);
        Result := IInterface(Geometry);
      end;
    OID_RECORD:
      begin
        Composite := DecodeComposite(Data);
        if Composite = nil then
          Result := Null
        else
          Result := Composite;
      end;
  else
    Result := Data;
  end;
end;

function StripTrailingNulls(const Data: TBytes): TBytes;
var
  EndPos: Integer;
begin
  EndPos := System.Length(Data);
  while (EndPos > 0) and (Data[EndPos - 1] = 0) do
    Dec(EndPos);
  SetLength(Result, EndPos);
  if EndPos > 0 then
    Move(Data[0], Result[0], EndPos);
end;

function LooksLikeText(const Data: TBytes): Boolean;
var
  I: Integer;
  ByteVal: Byte;
begin
  Result := True;
  for I := 0 to System.Length(Data) - 1 do
  begin
    ByteVal := Data[I];
    if (ByteVal = $09) or (ByteVal = $0A) or (ByteVal = $0D) then
      Continue;
    if (ByteVal < $20) or (ByteVal > $7E) then
      Exit(False);
  end;
end;

function ParseUnknownText(const Text: string): Variant;
var
  Trimmed: string;
  Lowered: string;
  IntVal: Int64;
  FloatVal: Double;
begin
  Trimmed := Trim(Text);
  if Trimmed = '' then
    Exit(Text);
  Lowered := LowerCase(Trimmed);
  if Lowered = 'true' then
    Exit(True);
  if Lowered = 'false' then
    Exit(False);
  if TryStrToInt64(Trimmed, IntVal) then
  begin
    if (IntVal >= Low(Integer)) and (IntVal <= High(Integer)) then
      Exit(Integer(IntVal));
    Exit(IntVal);
  end;
  if TryStrToFloat(Trimmed, FloatVal) then
    Exit(FloatVal);
  Result := Text;
end;

function DecodeUnknownBinary(const Data: TBytes): Variant;
var
  Trimmed: TBytes;
begin
  Trimmed := StripTrailingNulls(Data);
  if (System.Length(Trimmed) > 0) and LooksLikeText(Trimmed) then
    Exit(ParseUnknownText(TEncoding.UTF8.GetString(Trimmed)));
  case System.Length(Data) of
    1: Result := Integer(Data[0]);
    2: Result := ReadInt16LE(Data, 0);
    4: Result := Integer(ReadUInt32LE(Data, 0));
    8: Result := Int64(ReadUInt64LE(Data, 0));
    16: Result := BytesToUuid(Data);
  else
    Result := Data;
  end;
end;

end.
