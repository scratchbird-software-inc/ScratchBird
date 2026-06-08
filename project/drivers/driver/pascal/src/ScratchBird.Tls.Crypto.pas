// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

unit ScratchBird.Tls.Crypto;

{$mode delphi}
{$H+}

interface

uses
  SysUtils;

const
  SHA256_DIGEST_SIZE = 32;
  SHA256_BLOCK_SIZE = 64;

function Sha256(const Data: TBytes): TBytes;
function HmacSha256(const Key, Data: TBytes): TBytes;
function HkdfExtract(const Salt, InputKeyMaterial: TBytes): TBytes;
function HkdfExpand(const PseudoRandomKey, Info: TBytes; OutputLength: Integer): TBytes;
function BuildTls13HkdfLabel(const LabelText: string; const Context: TBytes; Length: Word): TBytes;
function HkdfExpandLabel(const Secret: TBytes; const LabelText: string; const Context: TBytes;
  OutputLength: Integer): TBytes;
function ConstantTimeEquals(const A, B: TBytes): Boolean;

implementation

const
  SHA256_INIT_STATE: array[0..7] of Cardinal = (
    $6A09E667, $BB67AE85, $3C6EF372, $A54FF53A,
    $510E527F, $9B05688C, $1F83D9AB, $5BE0CD19
  );

  SHA256_K: array[0..63] of Cardinal = (
    $428A2F98, $71374491, $B5C0FBCF, $E9B5DBA5, $3956C25B, $59F111F1, $923F82A4, $AB1C5ED5,
    $D807AA98, $12835B01, $243185BE, $550C7DC3, $72BE5D74, $80DEB1FE, $9BDC06A7, $C19BF174,
    $E49B69C1, $EFBE4786, $0FC19DC6, $240CA1CC, $2DE92C6F, $4A7484AA, $5CB0A9DC, $76F988DA,
    $983E5152, $A831C66D, $B00327C8, $BF597FC7, $C6E00BF3, $D5A79147, $06CA6351, $14292967,
    $27B70A85, $2E1B2138, $4D2C6DFC, $53380D13, $650A7354, $766A0ABB, $81C2C92E, $92722C85,
    $A2BFE8A1, $A81A664B, $C24B8B70, $C76C51A3, $D192E819, $D6990624, $F40E3585, $106AA070,
    $19A4C116, $1E376C08, $2748774C, $34B0BCB5, $391C0CB3, $4ED8AA4A, $5B9CCA4F, $682E6FF3,
    $748F82EE, $78A5636F, $84C87814, $8CC70208, $90BEFFFA, $A4506CEB, $BEF9A3F7, $C67178F2
  );

function RotateRight32(Value: Cardinal; Shift: Byte): Cardinal; inline;
begin
  Result := (Value shr Shift) or (Value shl (32 - Shift));
end;

function Sigma0(Value: Cardinal): Cardinal; inline;
begin
  Result := RotateRight32(Value, 2) xor RotateRight32(Value, 13) xor RotateRight32(Value, 22);
end;

function Sigma1(Value: Cardinal): Cardinal; inline;
begin
  Result := RotateRight32(Value, 6) xor RotateRight32(Value, 11) xor RotateRight32(Value, 25);
end;

function Gamma0(Value: Cardinal): Cardinal; inline;
begin
  Result := RotateRight32(Value, 7) xor RotateRight32(Value, 18) xor (Value shr 3);
end;

function Gamma1(Value: Cardinal): Cardinal; inline;
begin
  Result := RotateRight32(Value, 17) xor RotateRight32(Value, 19) xor (Value shr 10);
end;

procedure WriteUInt64BE(var Buffer: TBytes; Offset: Integer; Value: UInt64);
begin
  Buffer[Offset] := Byte((Value shr 56) and $FF);
  Buffer[Offset + 1] := Byte((Value shr 48) and $FF);
  Buffer[Offset + 2] := Byte((Value shr 40) and $FF);
  Buffer[Offset + 3] := Byte((Value shr 32) and $FF);
  Buffer[Offset + 4] := Byte((Value shr 24) and $FF);
  Buffer[Offset + 5] := Byte((Value shr 16) and $FF);
  Buffer[Offset + 6] := Byte((Value shr 8) and $FF);
  Buffer[Offset + 7] := Byte(Value and $FF);
end;

procedure WriteUInt16BE(var Buffer: TBytes; Offset: Integer; Value: Word);
begin
  Buffer[Offset] := Byte((Value shr 8) and $FF);
  Buffer[Offset + 1] := Byte(Value and $FF);
end;

function ReadUInt32BE(const Buffer: TBytes; Offset: Integer): Cardinal; inline;
begin
  Result := (Cardinal(Buffer[Offset]) shl 24) or
            (Cardinal(Buffer[Offset + 1]) shl 16) or
            (Cardinal(Buffer[Offset + 2]) shl 8) or
             Cardinal(Buffer[Offset + 3]);
end;

procedure Sha256Transform(var State: array of Cardinal; const Block: TBytes; BlockOffset: Integer);
var
  W: array[0..63] of Cardinal;
  A, B, C, D, E, F, G, H: Cardinal;
  T1, T2: Cardinal;
  I: Integer;
begin
  for I := 0 to 15 do
    W[I] := ReadUInt32BE(Block, BlockOffset + (I * 4));
  for I := 16 to 63 do
    W[I] := Gamma1(W[I - 2]) + W[I - 7] + Gamma0(W[I - 15]) + W[I - 16];

  A := State[0];
  B := State[1];
  C := State[2];
  D := State[3];
  E := State[4];
  F := State[5];
  G := State[6];
  H := State[7];

  for I := 0 to 63 do
  begin
    T1 := H + Sigma1(E) + ((E and F) xor ((not E) and G)) + SHA256_K[I] + W[I];
    T2 := Sigma0(A) + ((A and B) xor (A and C) xor (B and C));
    H := G;
    G := F;
    F := E;
    E := D + T1;
    D := C;
    C := B;
    B := A;
    A := T1 + T2;
  end;

  State[0] := State[0] + A;
  State[1] := State[1] + B;
  State[2] := State[2] + C;
  State[3] := State[3] + D;
  State[4] := State[4] + E;
  State[5] := State[5] + F;
  State[6] := State[6] + G;
  State[7] := State[7] + H;
end;

function Sha256(const Data: TBytes): TBytes;
var
  State: array[0..7] of Cardinal;
  Padded: TBytes;
  DataLength: Integer;
  PaddedLength: Integer;
  BitLength: UInt64;
  I: Integer;
begin
  Result := nil;
  for I := 0 to 7 do
    State[I] := SHA256_INIT_STATE[I];

  DataLength := Length(Data);
  BitLength := UInt64(DataLength) * 8;
  PaddedLength := ((DataLength + 9 + 63) div 64) * 64;

  SetLength(Padded, PaddedLength);
  if DataLength > 0 then
    Move(Data[0], Padded[0], DataLength);
  Padded[DataLength] := $80;
  WriteUInt64BE(Padded, PaddedLength - 8, BitLength);

  I := 0;
  while I < PaddedLength do
  begin
    Sha256Transform(State, Padded, I);
    Inc(I, 64);
  end;

  SetLength(Result, SHA256_DIGEST_SIZE);
  for I := 0 to 7 do
  begin
    Result[(I * 4)] := Byte((State[I] shr 24) and $FF);
    Result[(I * 4) + 1] := Byte((State[I] shr 16) and $FF);
    Result[(I * 4) + 2] := Byte((State[I] shr 8) and $FF);
    Result[(I * 4) + 3] := Byte(State[I] and $FF);
  end;
end;

function ConcatBytes(const A, B: TBytes): TBytes;
var
  ALength, BLength: Integer;
begin
  Result := nil;
  ALength := Length(A);
  BLength := Length(B);
  SetLength(Result, ALength + BLength);
  if ALength > 0 then
    Move(A[0], Result[0], ALength);
  if BLength > 0 then
    Move(B[0], Result[ALength], BLength);
end;

function HmacSha256(const Key, Data: TBytes): TBytes;
var
  EffectiveKey: TBytes;
  BlockKey: array[0..SHA256_BLOCK_SIZE - 1] of Byte;
  I: Integer;
  InnerInput: TBytes;
  OuterInput: TBytes;
  InnerHash: TBytes;
begin
  FillChar(BlockKey, SizeOf(BlockKey), 0);
  EffectiveKey := Key;
  if Length(EffectiveKey) > SHA256_BLOCK_SIZE then
    EffectiveKey := Sha256(EffectiveKey);
  if Length(EffectiveKey) > 0 then
    Move(EffectiveKey[0], BlockKey[0], Length(EffectiveKey));

  SetLength(InnerInput, SHA256_BLOCK_SIZE + Length(Data));
  for I := 0 to SHA256_BLOCK_SIZE - 1 do
    InnerInput[I] := BlockKey[I] xor $36;
  if Length(Data) > 0 then
    Move(Data[0], InnerInput[SHA256_BLOCK_SIZE], Length(Data));
  InnerHash := Sha256(InnerInput);

  SetLength(OuterInput, SHA256_BLOCK_SIZE + Length(InnerHash));
  for I := 0 to SHA256_BLOCK_SIZE - 1 do
    OuterInput[I] := BlockKey[I] xor $5C;
  if Length(InnerHash) > 0 then
    Move(InnerHash[0], OuterInput[SHA256_BLOCK_SIZE], Length(InnerHash));
  Result := Sha256(OuterInput);
end;

function HkdfExtract(const Salt, InputKeyMaterial: TBytes): TBytes;
var
  EffectiveSalt: TBytes;
begin
  EffectiveSalt := Salt;
  if Length(EffectiveSalt) = 0 then
  begin
    SetLength(EffectiveSalt, SHA256_DIGEST_SIZE);
    FillChar(EffectiveSalt[0], SHA256_DIGEST_SIZE, 0);
  end;
  Result := HmacSha256(EffectiveSalt, InputKeyMaterial);
end;

function HkdfExpand(const PseudoRandomKey, Info: TBytes; OutputLength: Integer): TBytes;
var
  N: Integer;
  BlockIndex: Integer;
  T: TBytes;
  Temp: TBytes;
  Data: TBytes;
  Remaining: Integer;
  Offset: Integer;
begin
  Result := nil;
  if OutputLength < 0 then
    raise Exception.Create('HKDF output length cannot be negative.');
  if OutputLength = 0 then
  begin
    SetLength(Result, 0);
    Exit;
  end;

  N := (OutputLength + SHA256_DIGEST_SIZE - 1) div SHA256_DIGEST_SIZE;
  if N > 255 then
    raise Exception.Create('HKDF output length is too large for SHA-256.');

  SetLength(Result, OutputLength);
  SetLength(T, 0);
  Offset := 0;

  for BlockIndex := 1 to N do
  begin
    Temp := ConcatBytes(T, Info);
    SetLength(Data, Length(Temp) + 1);
    if Length(Temp) > 0 then
      Move(Temp[0], Data[0], Length(Temp));
    Data[Length(Data) - 1] := Byte(BlockIndex);
    T := HmacSha256(PseudoRandomKey, Data);

    Remaining := OutputLength - Offset;
    if Remaining > SHA256_DIGEST_SIZE then
      Remaining := SHA256_DIGEST_SIZE;
    Move(T[0], Result[Offset], Remaining);
    Inc(Offset, Remaining);
  end;
end;

function BuildTls13HkdfLabel(const LabelText: string; const Context: TBytes; Length: Word): TBytes;
var
  FullLabel: AnsiString;
  LabelLength: Integer;
  ContextLength: Integer;
  Offset: Integer;
begin
  Result := nil;
  FullLabel := AnsiString('tls13 ' + LabelText);
  LabelLength := System.Length(FullLabel);
  ContextLength := System.Length(Context);

  if LabelLength > 255 then
    raise Exception.Create('TLS 1.3 HKDF label is too long.');
  if ContextLength > 255 then
    raise Exception.Create('TLS 1.3 HKDF context is too long.');

  SetLength(Result, 2 + 1 + LabelLength + 1 + ContextLength);
  WriteUInt16BE(Result, 0, Length);
  Result[2] := Byte(LabelLength);
  Offset := 3;
  if LabelLength > 0 then
  begin
    Move(FullLabel[1], Result[Offset], LabelLength);
    Inc(Offset, LabelLength);
  end;
  Result[Offset] := Byte(ContextLength);
  Inc(Offset);
  if ContextLength > 0 then
    Move(Context[0], Result[Offset], ContextLength);
end;

function HkdfExpandLabel(const Secret: TBytes; const LabelText: string; const Context: TBytes;
  OutputLength: Integer): TBytes;
var
  Info: TBytes;
begin
  if (OutputLength < 0) or (OutputLength > 65535) then
    raise Exception.Create('TLS 1.3 HKDF-Expand-Label length must be in range 0..65535.');
  Info := BuildTls13HkdfLabel(LabelText, Context, Word(OutputLength));
  Result := HkdfExpand(Secret, Info, OutputLength);
end;

function ConstantTimeEquals(const A, B: TBytes): Boolean;
var
  ALength, BLength: Integer;
  MaxLength: Integer;
  I: Integer;
  Diff: Integer;
  LeftByte, RightByte: Byte;
begin
  ALength := Length(A);
  BLength := Length(B);
  MaxLength := ALength;
  if BLength > MaxLength then
    MaxLength := BLength;

  Diff := ALength xor BLength;
  for I := 0 to MaxLength - 1 do
  begin
    LeftByte := 0;
    RightByte := 0;
    if I < ALength then
      LeftByte := A[I];
    if I < BLength then
      RightByte := B[I];
    Diff := Diff or (LeftByte xor RightByte);
  end;
  Result := Diff = 0;
end;

end.
