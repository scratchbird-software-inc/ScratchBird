// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

program TypesCodecTests;

{$mode delphi}
{$APPTYPE CONSOLE}

uses
  {$IFDEF UNIX}
  cthreads,
  {$ENDIF}
  SysUtils, Classes, Variants,
  ScratchBird.Protocol, ScratchBird.Types;

procedure Fail(const MessageText: string);
begin
  raise Exception.Create(MessageText);
end;

procedure AssertTrue(Value: Boolean; const MessageText: string);
begin
  if not Value then
    Fail(MessageText);
end;

procedure AssertEqualInt(Expected, Actual: Integer; const MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText + ': expected=' + IntToStr(Expected) + ' actual=' + IntToStr(Actual));
end;

procedure AssertEqualInt64(Expected, Actual: Int64; const MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText + ': expected=' + IntToStr(Expected) + ' actual=' + IntToStr(Actual));
end;

procedure AssertEqualCardinal(Expected, Actual: Cardinal; const MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText + ': expected=' + IntToStr(Expected) + ' actual=' + IntToStr(Actual));
end;

procedure AssertEqualString(const Expected, Actual, MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText + ': expected="' + Expected + '" actual="' + Actual + '"');
end;

procedure AssertEqualDoubleNear(Expected, Actual, Tolerance: Double; const MessageText: string);
begin
  if Abs(Expected - Actual) > Tolerance then
    Fail(MessageText + ': expected=' + FloatToStr(Expected) + ' actual=' + FloatToStr(Actual));
end;

procedure AssertEqualBytes(const Expected, Actual: TBytes; const MessageText: string);
var
  I: Integer;
begin
  if Length(Expected) <> Length(Actual) then
    Fail(MessageText + ': expected length=' + IntToStr(Length(Expected)) + ' actual length=' + IntToStr(Length(Actual)));
  for I := 0 to High(Expected) do
    if Expected[I] <> Actual[I] then
      Fail(MessageText + ': mismatch at index ' + IntToStr(I));
end;

procedure AssertVariantIsNullOrEmpty(const Value: Variant; const MessageText: string);
begin
  if not (VarIsNull(Value) or VarIsEmpty(Value)) then
    Fail(MessageText + ': expected null/empty variant');
end;

function ConcatBytes(const Left, Right: TBytes): TBytes;
begin
  SetLength(Result, Length(Left) + Length(Right));
  if Length(Left) > 0 then
    Move(Left[0], Result[0], Length(Left));
  if Length(Right) > 0 then
    Move(Right[0], Result[Length(Left)], Length(Right));
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

function WriteSingleLE(Value: Single): TBytes;
begin
  SetLength(Result, 4);
  Move(Value, Result[0], 4);
end;

function WriteDoubleLE(Value: Double): TBytes;
begin
  SetLength(Result, 8);
  Move(Value, Result[0], 8);
end;

function ReadInt32LEAt(const Data: TBytes; Offset: Integer): Integer;
begin
  Result := Integer(Cardinal(Data[Offset]) or (Cardinal(Data[Offset + 1]) shl 8) or
    (Cardinal(Data[Offset + 2]) shl 16) or (Cardinal(Data[Offset + 3]) shl 24));
end;

function ReadInt64LEAt(const Data: TBytes; Offset: Integer): Int64;
var
  Raw: UInt64;
begin
  Raw := UInt64(Data[Offset]) or (UInt64(Data[Offset + 1]) shl 8) or
    (UInt64(Data[Offset + 2]) shl 16) or (UInt64(Data[Offset + 3]) shl 24) or
    (UInt64(Data[Offset + 4]) shl 32) or (UInt64(Data[Offset + 5]) shl 40) or
    (UInt64(Data[Offset + 6]) shl 48) or (UInt64(Data[Offset + 7]) shl 56);
  Result := Int64(Raw);
end;

function HexToBytes(const Hex: string): TBytes;
var
  I: Integer;
  PairText: string;
begin
  if (Length(Hex) mod 2) <> 0 then
    Fail('hex length must be even');
  SetLength(Result, Length(Hex) div 2);
  for I := 0 to High(Result) do
  begin
    PairText := '$' + Copy(Hex, I * 2 + 1, 2);
    Result[I] := StrToInt(PairText);
  end;
end;

function WithLengthPrefix(const Payload: TBytes): TBytes;
begin
  Result := ConcatBytes(WriteInt32LE(Length(Payload)), Payload);
end;

function TimeMicros(const Value: TDateTime): Int64;
begin
  Result := Trunc(Frac(Value) * 86400 * 1000000);
end;

procedure AssertTimeMicrosNear(Expected, Actual: TDateTime; ToleranceMicros: Int64; const MessageText: string);
var
  Diff: Int64;
begin
  Diff := Abs(TimeMicros(Expected) - TimeMicros(Actual));
  if Diff > ToleranceMicros then
    Fail(MessageText + ': expectedMicros=' + IntToStr(TimeMicros(Expected)) +
      ' actualMicros=' + IntToStr(TimeMicros(Actual)) + ' diff=' + IntToStr(Diff));
end;

procedure TestEncodeBooleanParamUsesBoolOid;
var
  Param: TParamValue;
  Oid: Cardinal;
begin
  AssertTrue(EncodeParam(True, nil, Param, Oid), 'bool encode should succeed');
  AssertEqualInt(FORMAT_BINARY, Param.Format, 'bool encode format');
  AssertEqualCardinal(OID_BOOL, Oid, 'bool encode oid');
  AssertTrue(not Param.IsNull, 'bool encode null flag');
  AssertEqualInt(1, Length(Param.Data), 'bool payload length');
  AssertEqualInt(1, Param.Data[0], 'bool payload value');
end;

procedure TestEncodeUuidStringParamUsesUuidOid;
var
  Param: TParamValue;
  Oid: Cardinal;
  Decoded: Variant;
begin
  AssertTrue(EncodeParam('11111111-2222-3333-4444-555555555555', nil, Param, Oid), 'uuid string encode should succeed');
  AssertEqualCardinal(OID_UUID, Oid, 'uuid encode oid');
  AssertEqualInt(16, Length(Param.Data), 'uuid payload length');
  Decoded := DecodeValue(OID_UUID, Param.Data, FORMAT_BINARY);
  AssertEqualString('11111111-2222-3333-4444-555555555555', VarToStr(Decoded), 'uuid round-trip decode');
end;

procedure TestEncodeNumericArrayStillUsesVectorOid;
var
  Param: TParamValue;
  Oid: Cardinal;
  Value: Variant;
begin
  Value := VarArrayCreate([0, 2], varInteger);
  Value[0] := 1;
  Value[1] := 2;
  Value[2] := 3;
  AssertTrue(EncodeParam(Value, nil, Param, Oid), 'numeric array encode should succeed');
  AssertEqualCardinal(OID_SB_VECTOR, Oid, 'numeric array oid');
  AssertTrue(Length(Param.Data) > 4, 'numeric array payload should be length-prefixed text');
end;

procedure TestDecodeUuidBinaryCanonicalizesHyphenFormat;
var
  Decoded: Variant;
begin
  Decoded := DecodeValue(OID_UUID, HexToBytes('123456789abcdef0123456789abcdef0'), FORMAT_BINARY);
  AssertEqualString('12345678-9abc-def0-1234-56789abcdef0', VarToStr(Decoded), 'uuid decode canonical text');
end;

procedure TestDecodeVectorBinaryReturnsNumericVariantArray;
var
  Decoded: Variant;
begin
  Decoded := DecodeValue(OID_SB_VECTOR, WithLengthPrefix(TEncoding.UTF8.GetBytes('[1.5,2,3.25]')), FORMAT_BINARY);
  AssertTrue(VarIsArray(Decoded), 'vector decode should return array');
  AssertEqualInt(3, VarArrayHighBound(Decoded, 1) - VarArrayLowBound(Decoded, 1) + 1, 'vector decode element count');
  AssertTrue(Abs(VarAsType(Decoded[0], varDouble) - 1.5) < 0.000001, 'vector value 0');
  AssertTrue(Abs(VarAsType(Decoded[1], varDouble) - 2.0) < 0.000001, 'vector value 1');
  AssertTrue(Abs(VarAsType(Decoded[2], varDouble) - 3.25) < 0.000001, 'vector value 2');
end;

procedure TestDecodeVectorBinaryInvalidLiteralReturnsNull;
var
  Decoded: Variant;
begin
  Decoded := DecodeValue(OID_SB_VECTOR, WithLengthPrefix(TEncoding.UTF8.GetBytes('[1.5,abc,3.25]')), FORMAT_BINARY);
  AssertVariantIsNullOrEmpty(Decoded, 'vector invalid token should decode as null');

  Decoded := DecodeValue(OID_SB_VECTOR, WithLengthPrefix(TEncoding.UTF8.GetBytes('[1.5,]')), FORMAT_BINARY);
  AssertVariantIsNullOrEmpty(Decoded, 'vector trailing separator should decode as null');
end;

procedure TestDecodeJsonbBinaryReturnsWrapper;
var
  Decoded: Variant;
  Jsonb: IScratchBirdJsonb;
begin
  Decoded := DecodeValue(OID_JSONB, WithLengthPrefix(TEncoding.UTF8.GetBytes('{"k":1}')), FORMAT_BINARY);
  AssertTrue((VarType(Decoded) = varUnknown) and Supports(IInterface(Decoded), IScratchBirdJsonb, Jsonb),
    'jsonb decode should return IScratchBirdJsonb');
  AssertEqualString('{"k":1}', TEncoding.UTF8.GetString(Jsonb.GetRaw), 'jsonb raw payload');
end;

procedure TestDecodeCompositeRoundTripReturnsFields;
var
  Composite: TScratchBirdComposite;
  Param: TParamValue;
  Oid: Cardinal;
  Decoded: Variant;
  DecodedComposite: IScratchBirdComposite;
begin
  Composite := TScratchBirdComposite.Create([OID_INT4], [77]);
  try
    AssertTrue(EncodeParam(Null, Composite, Param, Oid), 'composite encode should succeed');
  finally
    Composite.Free;
  end;
  AssertEqualCardinal(OID_RECORD, Oid, 'composite encode oid');
  Decoded := DecodeValue(OID_RECORD, Param.Data, FORMAT_BINARY);
  AssertTrue((VarType(Decoded) = varUnknown) and Supports(IInterface(Decoded), IScratchBirdComposite, DecodedComposite),
    'composite decode should return IScratchBirdComposite');
  AssertEqualInt(1, DecodedComposite.GetFieldCount, 'composite field count');
  AssertEqualCardinal(OID_INT4, DecodedComposite.GetFieldOid(0), 'composite field oid');
  AssertEqualInt(77, VarAsType(DecodedComposite.GetFieldValue(0), varInteger), 'composite field value');
end;

procedure TestDecodeMalformedCompositePayloadReturnsNull;
var
  Decoded: Variant;
begin
  Decoded := DecodeValue(OID_RECORD, WriteInt32LE(-1), FORMAT_BINARY);
  AssertVariantIsNullOrEmpty(Decoded, 'negative composite field count should decode as null');

  Decoded := DecodeValue(OID_RECORD, ConcatBytes(WriteInt32LE(1), WriteInt32LE(OID_INT4)), FORMAT_BINARY);
  AssertVariantIsNullOrEmpty(Decoded, 'truncated composite field header should decode as null');

  Decoded := DecodeValue(
    OID_RECORD,
    ConcatBytes(
      WriteInt32LE(1),
      ConcatBytes(
        WriteInt32LE(OID_INT4),
        ConcatBytes(WriteInt32LE(4), TBytes.Create($01, $02)))),
    FORMAT_BINARY);
  AssertVariantIsNullOrEmpty(Decoded, 'truncated composite field payload should decode as null');
end;

procedure TestDecodeUnknownUsesTextHeuristics;
var
  BoolText: Variant;
  IntText: Variant;
begin
  BoolText := DecodeValue(0, TEncoding.UTF8.GetBytes('true'), FORMAT_TEXT);
  AssertTrue(Boolean(BoolText), 'unknown text bool decode');
  IntText := DecodeValue(0, TBytes.Create(Ord('4'), Ord('2'), 0), FORMAT_BINARY);
  AssertEqualInt(42, VarAsType(IntText, varInteger), 'unknown binary trailing-null int decode');
end;

procedure TestDecodeByteaPayloadReturnsVariantByteArray;
var
  Decoded: Variant;
begin
  Decoded := DecodeValue(OID_BYTEA, WithLengthPrefix(TBytes.Create($AA, $BB, $CC)), FORMAT_BINARY);
  AssertTrue(VarIsArray(Decoded), 'bytea decode should return variant array');
  AssertEqualInt(0, VarArrayLowBound(Decoded, 1), 'bytea decode low bound');
  AssertEqualInt(2, VarArrayHighBound(Decoded, 1), 'bytea decode high bound');
  AssertEqualInt($AA, VarAsType(Decoded[0], varInteger), 'bytea decode byte 0');
  AssertEqualInt($BB, VarAsType(Decoded[1], varInteger), 'bytea decode byte 1');
  AssertEqualInt($CC, VarAsType(Decoded[2], varInteger), 'bytea decode byte 2');
end;

procedure TestDecodeUnknownBinaryFixedWidthFallbacks;
var
  ByteValue: Variant;
  Int16Value: Variant;
  IntValue: Variant;
  Int64Value: Variant;
  UuidValue: Variant;
begin
  ByteValue := DecodeValue(0, TBytes.Create($FF), FORMAT_BINARY);
  AssertEqualInt(255, VarAsType(ByteValue, varInteger), 'unknown binary 1-byte fallback');

  Int16Value := DecodeValue(0, TBytes.Create($D2, $04), FORMAT_BINARY);
  AssertEqualInt(1234, VarAsType(Int16Value, varInteger), 'unknown binary 2-byte fallback');

  IntValue := DecodeValue(0, WriteInt32LE(123456), FORMAT_BINARY);
  AssertEqualInt(123456, VarAsType(IntValue, varInteger), 'unknown binary 4-byte fallback');

  Int64Value := DecodeValue(0, WriteInt64LE(1234567890123), FORMAT_BINARY);
  AssertEqualInt64(1234567890123, VarAsType(Int64Value, varInt64), 'unknown binary 8-byte fallback');

  UuidValue := DecodeValue(0, HexToBytes('00112233445566778899aabbccddeeff'), FORMAT_BINARY);
  AssertEqualString('00112233-4455-6677-8899-aabbccddeeff', VarToStr(UuidValue), 'unknown binary 16-byte fallback');
end;

procedure TestDecodeNullAndLimitPayloadShapes;
var
  Decoded: Variant;
  RangeIntf: IScratchBirdRange;
begin
  Decoded := DecodeValue(OID_INT4, nil, FORMAT_BINARY);
  AssertVariantIsNullOrEmpty(Decoded, 'empty payload should decode as null');

  Decoded := DecodeValue(OID_TIMETZ, TBytes.Create($01, $02, $03), FORMAT_BINARY);
  AssertVariantIsNullOrEmpty(Decoded, 'timetz short payload should decode as null');

  Decoded := DecodeValue(OID_SB_VECTOR, WithLengthPrefix(TEncoding.UTF8.GetBytes('[]')), FORMAT_BINARY);
  AssertVariantIsNullOrEmpty(Decoded, 'empty vector literal should decode as null');

  Decoded := DecodeValue(OID_INT4RANGE, TBytes.Create(RANGE_EMPTY, 0, 0, 0), FORMAT_BINARY);
  AssertTrue((VarType(Decoded) = varUnknown) and Supports(IInterface(Decoded), IScratchBirdRange, RangeIntf),
    'empty range should decode to range wrapper');
  AssertTrue(RangeIntf.GetEmpty, 'empty range flag should be preserved');

  Decoded := DecodeValue(OID_INT8RANGE, TBytes.Create(RANGE_LB_INF or RANGE_UB_INF, 0, 0, 0), FORMAT_BINARY);
  AssertTrue((VarType(Decoded) = varUnknown) and Supports(IInterface(Decoded), IScratchBirdRange, RangeIntf),
    'infinite-bound range should decode to range wrapper');
  AssertTrue(RangeIntf.GetLowerInfinite, 'infinite lower bound should be preserved');
  AssertTrue(RangeIntf.GetUpperInfinite, 'infinite upper bound should be preserved');
end;

procedure TestDecodeMalformedPayloadsReturnNull;
var
  Decoded: Variant;
begin
  Decoded := DecodeValue(OID_INT4, TBytes.Create($01, $02), FORMAT_BINARY);
  AssertVariantIsNullOrEmpty(Decoded, 'short int4 payload should decode as null');

  Decoded := DecodeValue(OID_FLOAT8, TBytes.Create($01, $02, $03, $04), FORMAT_BINARY);
  AssertVariantIsNullOrEmpty(Decoded, 'short float8 payload should decode as null');

  Decoded := DecodeValue(OID_DATE, TBytes.Create($01, $02, $03), FORMAT_BINARY);
  AssertVariantIsNullOrEmpty(Decoded, 'short date payload should decode as null');

  Decoded := DecodeValue(OID_TIME, TBytes.Create($01, $02, $03, $04), FORMAT_BINARY);
  AssertVariantIsNullOrEmpty(Decoded, 'short time payload should decode as null');

  Decoded := DecodeValue(OID_TIMESTAMP, TBytes.Create($01, $02, $03, $04, $05), FORMAT_BINARY);
  AssertVariantIsNullOrEmpty(Decoded, 'short timestamp payload should decode as null');

  Decoded := DecodeValue(OID_INTERVAL, ConcatBytes(WriteInt64LE(1), WriteInt32LE(2)), FORMAT_BINARY);
  AssertVariantIsNullOrEmpty(Decoded, 'short interval payload should decode as null');

  Decoded := DecodeValue(OID_INT4RANGE, TBytes.Create(RANGE_LB_INC, 0, 0, 0), FORMAT_BINARY);
  AssertVariantIsNullOrEmpty(Decoded, 'truncated range payload without lower bound length should decode as null');

  Decoded := DecodeValue(OID_INT4RANGE, TBytes.Create(RANGE_LB_INC, 0, 0, 0, 4, 0, 0, 0, 1, 0), FORMAT_BINARY);
  AssertVariantIsNullOrEmpty(Decoded, 'truncated range payload with short lower bound should decode as null');
end;

procedure TestDecodeScalarAndTextOidMatrix;
const
  TEXT_OIDS: array[0..11] of Cardinal = (
    OID_TEXT, OID_VARCHAR, OID_CHAR, OID_BPCHAR, OID_JSON, OID_XML,
    OID_TSVECTOR, OID_TSQUERY, OID_INET, OID_CIDR, OID_MACADDR, OID_MACADDR8);
var
  Decoded: Variant;
  I: Integer;
  Payload: TBytes;
  TextValue: string;
begin
  Decoded := DecodeValue(OID_INT2, TBytes.Create($2E, $FB), FORMAT_BINARY);
  AssertEqualInt(-1234, VarAsType(Decoded, varInteger), 'int2 decode');

  Decoded := DecodeValue(OID_INT4, WriteInt32LE(-54321), FORMAT_BINARY);
  AssertEqualInt(-54321, VarAsType(Decoded, varInteger), 'int4 decode');

  Decoded := DecodeValue(OID_INT8, WriteInt64LE(-9876543210), FORMAT_BINARY);
  AssertEqualInt64(-9876543210, VarAsType(Decoded, varInt64), 'int8 decode');

  Decoded := DecodeValue(OID_FLOAT4, WriteSingleLE(1.5), FORMAT_BINARY);
  AssertEqualDoubleNear(1.5, VarAsType(Decoded, varDouble), 0.000001, 'float4 decode');

  Decoded := DecodeValue(OID_FLOAT8, WriteDoubleLE(-2.25), FORMAT_BINARY);
  AssertEqualDoubleNear(-2.25, VarAsType(Decoded, varDouble), 0.0000000001, 'float8 decode');

  Decoded := DecodeValue(OID_NUMERIC, WithLengthPrefix(TEncoding.UTF8.GetBytes('123.7500')), FORMAT_BINARY);
  AssertEqualString('123.7500', VarToStr(Decoded), 'numeric decode');

  Decoded := DecodeValue(OID_MONEY, WriteInt64LE(12345), FORMAT_BINARY);
  AssertEqualDoubleNear(123.45, VarAsType(Decoded, varDouble), 0.000001, 'money decode');

  for I := 0 to High(TEXT_OIDS) do
  begin
    TextValue := 'value-' + IntToStr(TEXT_OIDS[I]);
    Payload := WithLengthPrefix(TEncoding.UTF8.GetBytes(TextValue));
    Decoded := DecodeValue(TEXT_OIDS[I], Payload, FORMAT_BINARY);
    AssertEqualString(TextValue, VarToStr(Decoded), 'text family decode ' + IntToStr(TEXT_OIDS[I]));
  end;
end;

procedure TestDecodeTemporalAndIntervalOidMatrix;
var
  Decoded: Variant;
  Payload: TBytes;
  TimeMicrosValue: Int64;
  ExpectedTimestamp: TDateTime;
  ExpectedTimestampMicros: Int64;
  BaseDate: TDateTime;
  ExpectedDate: TDateTime;
begin
  ExpectedDate := EncodeDate(2000, 1, 3);
  Decoded := DecodeValue(OID_DATE, WriteInt32LE(2), FORMAT_BINARY);
  AssertEqualInt(Trunc(ExpectedDate), Trunc(VarToDateTime(Decoded)), 'date decode');

  TimeMicrosValue := Int64((12 * 60 * 60 + 34 * 60 + 56) * 1000000) + 789000;
  Decoded := DecodeValue(OID_TIME, WriteInt64LE(TimeMicrosValue), FORMAT_BINARY);
  AssertTimeMicrosNear(EncodeTime(12, 34, 56, 789), VarToDateTime(Decoded), 8, 'time decode');

  BaseDate := EncodeDate(2000, 1, 1);
  ExpectedTimestamp := EncodeDate(2004, 5, 6) + EncodeTime(7, 8, 9, 123);
  ExpectedTimestampMicros := Trunc((ExpectedTimestamp - BaseDate) * 86400 * 1000000);

  Decoded := DecodeValue(OID_TIMESTAMP, WriteInt64LE(ExpectedTimestampMicros), FORMAT_BINARY);
  AssertTimeMicrosNear(ExpectedTimestamp, VarToDateTime(Decoded), 32, 'timestamp decode');

  Decoded := DecodeValue(OID_TIMESTAMPTZ, WriteInt64LE(ExpectedTimestampMicros), FORMAT_BINARY);
  AssertTimeMicrosNear(ExpectedTimestamp, VarToDateTime(Decoded), 32, 'timestamptz decode');

  Payload := ConcatBytes(WriteInt64LE(5000000), ConcatBytes(WriteInt32LE(3), WriteInt32LE(2)));
  Decoded := DecodeValue(OID_INTERVAL, Payload, FORMAT_BINARY);
  AssertTrue(VarIsArray(Decoded), 'interval decode should return array');
  AssertEqualInt(2, VarAsType(Decoded[0], varInteger), 'interval months');
  AssertEqualInt(3, VarAsType(Decoded[1], varInteger), 'interval days');
  AssertEqualInt64(5000000, VarAsType(Decoded[2], varInt64), 'interval micros');
end;

procedure TestEncodePrimitiveOidMatrix;
var
  Param: TParamValue;
  Oid: Cardinal;
  Decoded: Variant;
  ExpectedTimestamp: TDateTime;
  MixedArray: Variant;
begin
  AssertTrue(EncodeParam(321, nil, Param, Oid), 'int encode should succeed');
  AssertEqualCardinal(OID_INT4, Oid, 'int encode oid');
  Decoded := DecodeValue(OID_INT4, Param.Data, FORMAT_BINARY);
  AssertEqualInt(321, VarAsType(Decoded, varInteger), 'int encode/decode');

  AssertTrue(EncodeParam(Int64(3000000000), nil, Param, Oid), 'int64 encode should succeed');
  AssertEqualCardinal(OID_INT8, Oid, 'int64 encode oid');
  Decoded := DecodeValue(OID_INT8, Param.Data, FORMAT_BINARY);
  AssertEqualInt64(3000000000, VarAsType(Decoded, varInt64), 'int64 encode/decode');

  AssertTrue(EncodeParam(VarAsType(1.25, varSingle), nil, Param, Oid), 'single encode should succeed');
  AssertEqualCardinal(OID_FLOAT4, Oid, 'single encode oid');
  Decoded := DecodeValue(OID_FLOAT4, Param.Data, FORMAT_BINARY);
  AssertEqualDoubleNear(1.25, VarAsType(Decoded, varDouble), 0.000001, 'single encode/decode');

  AssertTrue(EncodeParam(Double(-9.5), nil, Param, Oid), 'double encode should succeed');
  AssertEqualCardinal(OID_FLOAT8, Oid, 'double encode oid');
  Decoded := DecodeValue(OID_FLOAT8, Param.Data, FORMAT_BINARY);
  AssertEqualDoubleNear(-9.5, VarAsType(Decoded, varDouble), 0.0000000001, 'double encode/decode');

  AssertTrue(EncodeParam('plain-text', nil, Param, Oid), 'text encode should succeed');
  AssertEqualCardinal(OID_TEXT, Oid, 'text encode oid');
  Decoded := DecodeValue(OID_TEXT, Param.Data, FORMAT_BINARY);
  AssertEqualString('plain-text', VarToStr(Decoded), 'text encode/decode');

  ExpectedTimestamp := EncodeDate(2020, 1, 2) + EncodeTime(3, 4, 5, 6);
  AssertTrue(EncodeParam(VarFromDateTime(ExpectedTimestamp), nil, Param, Oid), 'date variant encode should succeed');
  AssertEqualCardinal(OID_TIMESTAMPTZ, Oid, 'date variant encode oid');
  Decoded := DecodeValue(OID_TIMESTAMPTZ, Param.Data, FORMAT_BINARY);
  AssertTimeMicrosNear(ExpectedTimestamp, VarToDateTime(Decoded), 32, 'date variant encode/decode');

  MixedArray := VarArrayOf([1, 'two']);
  AssertTrue(EncodeParam(MixedArray, nil, Param, Oid), 'mixed array encode should succeed');
  AssertEqualCardinal(0, Oid, 'mixed array encode oid');
  AssertTrue(Length(Param.Data) > 4, 'mixed array payload should be length-prefixed text');
  AssertEqualInt(Ord('{'), Param.Data[4], 'mixed array literal should start with "{"');
end;

procedure TestEncodeObjectWrappersAndRangeMatrix;
var
  JsonbObj: TScratchBirdJsonb;
  GeometryObj: TScratchBirdGeometry;
  RangeObj: TScratchBirdRange;
  AutoRange: TScratchBirdRange;
  TsRange: TScratchBirdRange;
  Param: TParamValue;
  Oid: Cardinal;
  Decoded: Variant;
  JsonbIntf: IScratchBirdJsonb;
  GeometryIntf: IScratchBirdGeometry;
  RangeIntf: IScratchBirdRange;
  ExpectedLower, ExpectedUpper: TDateTime;
begin
  JsonbObj := TScratchBirdJsonb.CreateText('{"ok":true}');
  try
    AssertTrue(EncodeParam(Null, JsonbObj, Param, Oid), 'jsonb object encode should succeed');
  finally
    JsonbObj.Free;
  end;
  AssertEqualCardinal(OID_JSONB, Oid, 'jsonb object encode oid');
  Decoded := DecodeValue(OID_JSONB, Param.Data, FORMAT_BINARY);
  AssertTrue((VarType(Decoded) = varUnknown) and Supports(IInterface(Decoded), IScratchBirdJsonb, JsonbIntf),
    'jsonb object decode should return wrapper');
  AssertEqualString('{"ok":true}', TEncoding.UTF8.GetString(JsonbIntf.GetRaw), 'jsonb object raw payload');

  GeometryObj := TScratchBirdGeometry.Create(TBytes.Create($01, $02, $03, $04));
  try
    AssertTrue(EncodeParam(Null, GeometryObj, Param, Oid), 'geometry object encode should succeed');
  finally
    GeometryObj.Free;
  end;
  AssertEqualCardinal(OID_POINT, Oid, 'geometry object encode oid');
  Decoded := DecodeValue(OID_POINT, Param.Data, FORMAT_BINARY);
  AssertTrue((VarType(Decoded) = varUnknown) and Supports(IInterface(Decoded), IScratchBirdGeometry, GeometryIntf),
    'geometry object decode should return wrapper');
  AssertEqualBytes(TBytes.Create($01, $02, $03, $04), GeometryIntf.GetWkb, 'geometry object WKB');
  AssertEqualCardinal(OID_POINT, GeometryIntf.GetGeometryOid, 'geometry object default oid');

  GeometryObj := TScratchBirdGeometry.Create(TBytes.Create($05, $06, $07, $08), 0, '', OID_LINE);
  try
    AssertTrue(EncodeParam(Null, GeometryObj, Param, Oid), 'geometry object custom oid encode should succeed');
  finally
    GeometryObj.Free;
  end;
  AssertEqualCardinal(OID_LINE, Oid, 'geometry object custom oid encode oid');
  Decoded := DecodeValue(OID_LINE, Param.Data, FORMAT_BINARY);
  AssertTrue((VarType(Decoded) = varUnknown) and Supports(IInterface(Decoded), IScratchBirdGeometry, GeometryIntf),
    'geometry object custom oid decode should return wrapper');
  AssertEqualBytes(TBytes.Create($05, $06, $07, $08), GeometryIntf.GetWkb, 'geometry object custom oid WKB');
  AssertEqualCardinal(OID_LINE, GeometryIntf.GetGeometryOid, 'geometry object custom oid');

  RangeObj := TScratchBirdRange.Create;
  try
    RangeObj.RangeOid := OID_INT4RANGE;
    RangeObj.Lower := 1;
    RangeObj.Upper := 10;
    RangeObj.LowerInclusive := True;
    RangeObj.UpperInclusive := False;
    RangeObj.LowerInfinite := False;
    RangeObj.UpperInfinite := False;
    RangeObj.Empty := False;
    AssertTrue(EncodeParam(Null, RangeObj, Param, Oid), 'int4 range encode should succeed');
  finally
    RangeObj.Free;
  end;
  AssertEqualCardinal(OID_INT4RANGE, Oid, 'int4 range oid');
  Decoded := DecodeValue(OID_INT4RANGE, Param.Data, FORMAT_BINARY);
  AssertTrue((VarType(Decoded) = varUnknown) and Supports(IInterface(Decoded), IScratchBirdRange, RangeIntf),
    'int4 range decode should return wrapper');
  AssertEqualCardinal(OID_INT4RANGE, RangeIntf.GetRangeOid, 'int4 range decoded oid');
  AssertTrue(RangeIntf.GetLowerInclusive, 'int4 range lower inclusive');
  AssertTrue(not RangeIntf.GetUpperInclusive, 'int4 range upper exclusive');
  AssertEqualInt(1, VarAsType(RangeIntf.GetLower, varInteger), 'int4 range lower');
  AssertEqualInt(10, VarAsType(RangeIntf.GetUpper, varInteger), 'int4 range upper');

  AutoRange := TScratchBirdRange.Create;
  try
    AutoRange.Lower := VarAsType(1, varInteger);
    AutoRange.Upper := VarAsType(2, varInteger);
    AutoRange.LowerInclusive := True;
    AutoRange.UpperInclusive := True;
    AssertTrue(EncodeParam(Null, AutoRange, Param, Oid), 'auto range encode should succeed');
  finally
    AutoRange.Free;
  end;
  AssertEqualCardinal(OID_INT8RANGE, Oid, 'auto range should resolve to int8 range');

  ExpectedLower := EncodeDate(2020, 1, 1) + EncodeTime(1, 2, 3, 4);
  ExpectedUpper := EncodeDate(2020, 1, 2) + EncodeTime(5, 6, 7, 8);
  TsRange := TScratchBirdRange.Create;
  try
    TsRange.RangeOid := OID_TSRANGE;
    TsRange.Lower := VarFromDateTime(ExpectedLower);
    TsRange.Upper := VarFromDateTime(ExpectedUpper);
    TsRange.LowerInclusive := True;
    TsRange.UpperInclusive := True;
    AssertTrue(EncodeParam(Null, TsRange, Param, Oid), 'ts range encode should succeed');
  finally
    TsRange.Free;
  end;
  AssertEqualCardinal(OID_TSRANGE, Oid, 'ts range oid');
  Decoded := DecodeValue(OID_TSRANGE, Param.Data, FORMAT_BINARY);
  AssertTrue((VarType(Decoded) = varUnknown) and Supports(IInterface(Decoded), IScratchBirdRange, RangeIntf),
    'ts range decode should return wrapper');
  AssertTimeMicrosNear(ExpectedLower, VarToDateTime(RangeIntf.GetLower), 32, 'ts range lower');
  AssertTimeMicrosNear(ExpectedUpper, VarToDateTime(RangeIntf.GetUpper), 32, 'ts range upper');
end;

procedure TestDecodeGeometryFamilyOidMatrix;
const
  GEOM_OIDS: array[0..6] of Cardinal = (OID_POINT, OID_LSEG, OID_PATH, OID_BOX, OID_POLYGON, OID_LINE, OID_CIRCLE);
var
  I: Integer;
  Payload: TBytes;
  Decoded: Variant;
  GeometryIntf: IScratchBirdGeometry;
  Wkb: TBytes;
begin
  Wkb := TBytes.Create($10, $20, $30, $40, $50);
  Payload := WithLengthPrefix(Wkb);
  for I := 0 to High(GEOM_OIDS) do
  begin
    Decoded := DecodeValue(GEOM_OIDS[I], Payload, FORMAT_BINARY);
    AssertTrue((VarType(Decoded) = varUnknown) and Supports(IInterface(Decoded), IScratchBirdGeometry, GeometryIntf),
      'geometry family decode should return wrapper for oid ' + IntToStr(GEOM_OIDS[I]));
    AssertEqualBytes(Wkb, GeometryIntf.GetWkb, 'geometry family WKB for oid ' + IntToStr(GEOM_OIDS[I]));
    AssertEqualCardinal(GEOM_OIDS[I], GeometryIntf.GetGeometryOid,
      'geometry family oid preservation for oid ' + IntToStr(GEOM_OIDS[I]));
  end;
end;

procedure TestDecodeTimeTzTwelveBytePayloadPreservesOffsetAndNormalizesDay;
var
  Payload: TBytes;
  Decoded: Variant;
  TimeValue: TDateTime;
  OffsetSecondsEast: Integer;
  Expected: TDateTime;
begin
  Payload := ConcatBytes(WriteInt64LE(Int64(25 * 60 * 60 * 1000000) + 123456), WriteInt32LE(18000));
  Decoded := DecodeValue(OID_TIMETZ, Payload, FORMAT_BINARY);
  AssertTrue(VarIsArray(Decoded), 'timetz decode (12-byte) should return array');
  TimeValue := VarToDateTime(Decoded[0]);
  OffsetSecondsEast := VarAsType(Decoded[1], varInteger);
  Expected := EncodeTime(1, 0, 0, 0) + (123456 / 86400 / 1000000);
  AssertTimeMicrosNear(Expected, TimeValue, 32, 'timetz decode normalized time');
  AssertEqualInt(-18000, OffsetSecondsEast, 'timetz decode offset seconds east');
end;

procedure TestDecodeTimeTzEightBytePayloadDefaultsToUtc;
var
  Payload: TBytes;
  Decoded: Variant;
  TimeValue: TDateTime;
  OffsetSecondsEast: Integer;
begin
  Payload := WriteInt64LE(Int64((1 * 60 * 60 + 1 * 60 + 1) * 1000000));
  Decoded := DecodeValue(OID_TIMETZ, Payload, FORMAT_BINARY);
  AssertTrue(VarIsArray(Decoded), 'timetz decode (8-byte) should return array');
  TimeValue := VarToDateTime(Decoded[0]);
  OffsetSecondsEast := VarAsType(Decoded[1], varInteger);
  AssertTimeMicrosNear(EncodeTime(1, 1, 1, 0), TimeValue, 8, 'timetz 8-byte time');
  AssertEqualInt(0, OffsetSecondsEast, 'timetz 8-byte default offset');
end;

procedure TestEncodeTimeTzVariantArrayUsesTimetzOidAndPayloadShape;
var
  Param: TParamValue;
  Oid: Cardinal;
  Value: Variant;
  Micros: Int64;
  ZoneSecondsWest: Integer;
  ExpectedMicros: Int64;
begin
  Value := VarArrayOf([VarFromDateTime(EncodeTime(14, 30, 15, 250)), 3600]);
  AssertTrue(EncodeParam(Value, nil, Param, Oid), 'timetz encode should succeed');
  AssertEqualCardinal(OID_TIMETZ, Oid, 'timetz encode oid');
  AssertEqualInt(12, Length(Param.Data), 'timetz payload length');

  Micros := ReadInt64LEAt(Param.Data, 0);
  ZoneSecondsWest := ReadInt32LEAt(Param.Data, 8);
  ExpectedMicros := TimeMicros(EncodeTime(14, 30, 15, 250));

  AssertTrue(Abs(Micros - ExpectedMicros) <= 2, 'timetz encoded micros');
  AssertEqualInt(-3600, ZoneSecondsWest, 'timetz encoded zone seconds west');
end;

begin
  try
    TestEncodeBooleanParamUsesBoolOid;
    TestEncodeUuidStringParamUsesUuidOid;
    TestEncodeNumericArrayStillUsesVectorOid;
    TestDecodeUuidBinaryCanonicalizesHyphenFormat;
    TestDecodeVectorBinaryReturnsNumericVariantArray;
    TestDecodeVectorBinaryInvalidLiteralReturnsNull;
    TestDecodeJsonbBinaryReturnsWrapper;
    TestDecodeCompositeRoundTripReturnsFields;
    TestDecodeMalformedCompositePayloadReturnsNull;
    TestDecodeUnknownUsesTextHeuristics;
    TestDecodeByteaPayloadReturnsVariantByteArray;
    TestDecodeUnknownBinaryFixedWidthFallbacks;
    TestDecodeNullAndLimitPayloadShapes;
    TestDecodeMalformedPayloadsReturnNull;
    TestDecodeScalarAndTextOidMatrix;
    TestDecodeTemporalAndIntervalOidMatrix;
    TestEncodePrimitiveOidMatrix;
    TestEncodeObjectWrappersAndRangeMatrix;
    TestDecodeGeometryFamilyOidMatrix;
    TestDecodeTimeTzTwelveBytePayloadPreservesOffsetAndNormalizesDay;
    TestDecodeTimeTzEightBytePayloadDefaultsToUtc;
    TestEncodeTimeTzVariantArrayUsesTimetzOidAndPayloadShape;
    Writeln('TypesCodecTests: OK');
  except
    on E: Exception do
    begin
      Writeln('TypesCodecTests: FAILED - ' + E.Message);
      Halt(1);
    end;
  end;
end.
