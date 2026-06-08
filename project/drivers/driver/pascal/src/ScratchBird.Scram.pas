// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

unit ScratchBird.Scram;

{$mode delphi}
{$H+}

interface

uses
  SysUtils, Classes, Math;

type
  TScramClient = class
  private
    FUserName: string;
    FAlgorithm: string;
    FClientNonce: string;
    FClientFirstBare: string;
    FServerSignature: TBytes;
    function ConcatBytes(const Left, Right: TBytes): TBytes;
    function BytesFromInt(I: Integer): TBytes;
    function EscapeValue(const Value: string): string;
    function HmacSha256(const Key, Data: TBytes): TBytes;
    function HmacSha512(const Key, Data: TBytes): TBytes;
    function Sha256(const Data: TBytes): TBytes;
    function Sha512(const Data: TBytes): TBytes;
    function Pbkdf2Sha256(const Password, Salt: TBytes; Iterations, KeyLen: Integer): TBytes;
    function Pbkdf2Sha512(const Password, Salt: TBytes; Iterations, KeyLen: Integer): TBytes;
    function HashBytes(const Data: TBytes): TBytes;
    function HmacBytes(const Key, Data: TBytes): TBytes;
    function Pbkdf2Bytes(const Password, Salt: TBytes; Iterations, KeyLen: Integer): TBytes;
    function DerivedKeyLength: Integer;
    function XorBytes(const Left, Right: TBytes): TBytes;
    function Base64Encode(const Data: TBytes): string;
    function Base64Decode(const Value: string): TBytes;
    function ParseAttributes(const Message: string): TStringList;
  public
    constructor Create(const UserName: string; const Algorithm: string = 'sha256');
    function ClientFirstMessage: string;
    function HandleServerFirst(const Password, ServerFirst: string): string;
    procedure VerifyServerFinal(const ServerFinal: string);
  end;

implementation

{$IFDEF FPC}
type
  TSha256State = array[0..7] of Cardinal;
  TSha256Block = array[0..63] of Cardinal;
  TSha512State = array[0..7] of UInt64;
  TSha512Block = array[0..79] of UInt64;

const
  Sha256Init: TSha256State = (
    $6A09E667, $BB67AE85, $3C6EF372, $A54FF53A,
    $510E527F, $9B05688C, $1F83D9AB, $5BE0CD19
  );

  Sha256K: array[0..63] of Cardinal = (
    $428A2F98, $71374491, $B5C0FBCF, $E9B5DBA5,
    $3956C25B, $59F111F1, $923F82A4, $AB1C5ED5,
    $D807AA98, $12835B01, $243185BE, $550C7DC3,
    $72BE5D74, $80DEB1FE, $9BDC06A7, $C19BF174,
    $E49B69C1, $EFBE4786, $0FC19DC6, $240CA1CC,
    $2DE92C6F, $4A7484AA, $5CB0A9DC, $76F988DA,
    $983E5152, $A831C66D, $B00327C8, $BF597FC7,
    $C6E00BF3, $D5A79147, $06CA6351, $14292967,
    $27B70A85, $2E1B2138, $4D2C6DFC, $53380D13,
    $650A7354, $766A0ABB, $81C2C92E, $92722C85,
    $A2BFE8A1, $A81A664B, $C24B8B70, $C76C51A3,
    $D192E819, $D6990624, $F40E3585, $106AA070,
    $19A4C116, $1E376C08, $2748774C, $34B0BCB5,
    $391C0CB3, $4ED8AA4A, $5B9CCA4F, $682E6FF3,
    $748F82EE, $78A5636F, $84C87814, $8CC70208,
    $90BEFFFA, $A4506CEB, $BEF9A3F7, $C67178F2
  );

  Sha512Init: TSha512State = (
    UInt64($6A09E667F3BCC908), UInt64($BB67AE8584CAA73B),
    UInt64($3C6EF372FE94F82B), UInt64($A54FF53A5F1D36F1),
    UInt64($510E527FADE682D1), UInt64($9B05688C2B3E6C1F),
    UInt64($1F83D9ABFB41BD6B), UInt64($5BE0CD19137E2179)
  );

  Sha512K: array[0..79] of UInt64 = (
    UInt64($428A2F98D728AE22), UInt64($7137449123EF65CD),
    UInt64($B5C0FBCFEC4D3B2F), UInt64($E9B5DBA58189DBBC),
    UInt64($3956C25BF348B538), UInt64($59F111F1B605D019),
    UInt64($923F82A4AF194F9B), UInt64($AB1C5ED5DA6D8118),
    UInt64($D807AA98A3030242), UInt64($12835B0145706FBE),
    UInt64($243185BE4EE4B28C), UInt64($550C7DC3D5FFB4E2),
    UInt64($72BE5D74F27B896F), UInt64($80DEB1FE3B1696B1),
    UInt64($9BDC06A725C71235), UInt64($C19BF174CF692694),
    UInt64($E49B69C19EF14AD2), UInt64($EFBE4786384F25E3),
    UInt64($0FC19DC68B8CD5B5), UInt64($240CA1CC77AC9C65),
    UInt64($2DE92C6F592B0275), UInt64($4A7484AA6EA6E483),
    UInt64($5CB0A9DCBD41FBD4), UInt64($76F988DA831153B5),
    UInt64($983E5152EE66DFAB), UInt64($A831C66D2DB43210),
    UInt64($B00327C898FB213F), UInt64($BF597FC7BEEF0EE4),
    UInt64($C6E00BF33DA88FC2), UInt64($D5A79147930AA725),
    UInt64($06CA6351E003826F), UInt64($142929670A0E6E70),
    UInt64($27B70A8546D22FFC), UInt64($2E1B21385C26C926),
    UInt64($4D2C6DFC5AC42AED), UInt64($53380D139D95B3DF),
    UInt64($650A73548BAF63DE), UInt64($766A0ABB3C77B2A8),
    UInt64($81C2C92E47EDAEE6), UInt64($92722C851482353B),
    UInt64($A2BFE8A14CF10364), UInt64($A81A664BBC423001),
    UInt64($C24B8B70D0F89791), UInt64($C76C51A30654BE30),
    UInt64($D192E819D6EF5218), UInt64($D69906245565A910),
    UInt64($F40E35855771202A), UInt64($106AA07032BBD1B8),
    UInt64($19A4C116B8D2D0C8), UInt64($1E376C085141AB53),
    UInt64($2748774CDF8EEB99), UInt64($34B0BCB5E19B48A8),
    UInt64($391C0CB3C5C95A63), UInt64($4ED8AA4AE3418ACB),
    UInt64($5B9CCA4F7763E373), UInt64($682E6FF3D6B2B8A3),
    UInt64($748F82EE5DEFB2FC), UInt64($78A5636F43172F60),
    UInt64($84C87814A1F0AB72), UInt64($8CC702081A6439EC),
    UInt64($90BEFFFA23631E28), UInt64($A4506CEBDE82BDE9),
    UInt64($BEF9A3F7B2C67915), UInt64($C67178F2E372532B),
    UInt64($CA273ECEEA26619C), UInt64($D186B8C721C0C207),
    UInt64($EADA7DD6CDE0EB1E), UInt64($F57D4F7FEE6ED178),
    UInt64($06F067AA72176FBA), UInt64($0A637DC5A2C898A6),
    UInt64($113F9804BEF90DAE), UInt64($1B710B35131C471B),
    UInt64($28DB77F523047D84), UInt64($32CAAB7B40C72493),
    UInt64($3C9EBE0A15C9BEBC), UInt64($431D67C49C100D4C),
    UInt64($4CC5D4BECB3E42B6), UInt64($597F299CFC657E2A),
    UInt64($5FCB6FAB3AD6FAEC), UInt64($6C44198C4A475817)
  );

function RotR32(Value: Cardinal; Bits: Byte): Cardinal;
begin
  Result := (Value shr Bits) or (Value shl (32 - Bits));
end;

function RotR64(Value: UInt64; Bits: Byte): UInt64;
begin
  Result := (Value shr Bits) or (Value shl (64 - Bits));
end;

function Ch(X, Y, Z: Cardinal): Cardinal;
begin
  Result := (X and Y) xor ((not X) and Z);
end;

function Maj(X, Y, Z: Cardinal): Cardinal;
begin
  Result := (X and Y) xor (X and Z) xor (Y and Z);
end;

function BigSigma0(X: Cardinal): Cardinal;
begin
  Result := RotR32(X, 2) xor RotR32(X, 13) xor RotR32(X, 22);
end;

function BigSigma1(X: Cardinal): Cardinal;
begin
  Result := RotR32(X, 6) xor RotR32(X, 11) xor RotR32(X, 25);
end;

function SmallSigma0(X: Cardinal): Cardinal;
begin
  Result := RotR32(X, 7) xor RotR32(X, 18) xor (X shr 3);
end;

function SmallSigma1(X: Cardinal): Cardinal;
begin
  Result := RotR32(X, 17) xor RotR32(X, 19) xor (X shr 10);
end;

function Ch64(X, Y, Z: UInt64): UInt64;
begin
  Result := (X and Y) xor ((not X) and Z);
end;

function Maj64(X, Y, Z: UInt64): UInt64;
begin
  Result := (X and Y) xor (X and Z) xor (Y and Z);
end;

function BigSigma064(X: UInt64): UInt64;
begin
  Result := RotR64(X, 28) xor RotR64(X, 34) xor RotR64(X, 39);
end;

function BigSigma164(X: UInt64): UInt64;
begin
  Result := RotR64(X, 14) xor RotR64(X, 18) xor RotR64(X, 41);
end;

function SmallSigma064(X: UInt64): UInt64;
begin
  Result := RotR64(X, 1) xor RotR64(X, 8) xor (X shr 7);
end;

function SmallSigma164(X: UInt64): UInt64;
begin
  Result := RotR64(X, 19) xor RotR64(X, 61) xor (X shr 6);
end;

function Sha256Digest(const Data: TBytes): TBytes;
var
  State: TSha256State;
  Block: TSha256Block;
  A, B, C, D, E, F, G, H: Cardinal;
  T1, T2: Cardinal;
  I, J: Integer;
  Padded: TBytes;
  BitLen: UInt64;
  Offset: Integer;
begin
  State := Sha256Init;
  BitLen := UInt64(Length(Data)) * 8;

  Padded := Copy(Data, 0, Length(Data));
  SetLength(Padded, Length(Padded) + 1);
  Padded[Length(Padded) - 1] := $80;
  while (Length(Padded) mod 64) <> 56 do
    SetLength(Padded, Length(Padded) + 1);
  SetLength(Padded, Length(Padded) + 8);
  for I := 0 to 7 do
    Padded[Length(Padded) - 8 + I] := Byte((BitLen shr (56 - I * 8)) and $FF);

  Offset := 0;
  while Offset < Length(Padded) do
  begin
    for I := 0 to 15 do
    begin
      J := Offset + I * 4;
      Block[I] := (Cardinal(Padded[J]) shl 24) or
                  (Cardinal(Padded[J + 1]) shl 16) or
                  (Cardinal(Padded[J + 2]) shl 8) or
                   Cardinal(Padded[J + 3]);
    end;
    for I := 16 to 63 do
      Block[I] := SmallSigma1(Block[I - 2]) + Block[I - 7] + SmallSigma0(Block[I - 15]) + Block[I - 16];

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
      T1 := H + BigSigma1(E) + Ch(E, F, G) + Sha256K[I] + Block[I];
      T2 := BigSigma0(A) + Maj(A, B, C);
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

    Inc(Offset, 64);
  end;

  SetLength(Result, 32);
  for I := 0 to 7 do
  begin
    Result[I * 4] := Byte((State[I] shr 24) and $FF);
    Result[I * 4 + 1] := Byte((State[I] shr 16) and $FF);
    Result[I * 4 + 2] := Byte((State[I] shr 8) and $FF);
    Result[I * 4 + 3] := Byte(State[I] and $FF);
  end;
end;

function Sha512Digest(const Data: TBytes): TBytes;
var
  State: TSha512State;
  Block: TSha512Block;
  A, B, C, D, E, F, G, H: UInt64;
  T1, T2: UInt64;
  I, J: Integer;
  Padded: TBytes;
  BitLen: UInt64;
  Offset: Integer;
begin
  State := Sha512Init;
  BitLen := UInt64(Length(Data)) * 8;

  Padded := Copy(Data, 0, Length(Data));
  SetLength(Padded, Length(Padded) + 1);
  Padded[Length(Padded) - 1] := $80;
  while (Length(Padded) mod 128) <> 112 do
    SetLength(Padded, Length(Padded) + 1);
  SetLength(Padded, Length(Padded) + 16);
  for I := 0 to 7 do
    Padded[Length(Padded) - 16 + I] := 0;
  for I := 0 to 7 do
    Padded[Length(Padded) - 8 + I] := Byte((BitLen shr (56 - I * 8)) and $FF);

  Offset := 0;
  while Offset < Length(Padded) do
  begin
    for I := 0 to 15 do
    begin
      J := Offset + I * 8;
      Block[I] := (UInt64(Padded[J]) shl 56) or
                  (UInt64(Padded[J + 1]) shl 48) or
                  (UInt64(Padded[J + 2]) shl 40) or
                  (UInt64(Padded[J + 3]) shl 32) or
                  (UInt64(Padded[J + 4]) shl 24) or
                  (UInt64(Padded[J + 5]) shl 16) or
                  (UInt64(Padded[J + 6]) shl 8) or
                   UInt64(Padded[J + 7]);
    end;
    for I := 16 to 79 do
      Block[I] := SmallSigma164(Block[I - 2]) + Block[I - 7] + SmallSigma064(Block[I - 15]) + Block[I - 16];

    A := State[0];
    B := State[1];
    C := State[2];
    D := State[3];
    E := State[4];
    F := State[5];
    G := State[6];
    H := State[7];

    for I := 0 to 79 do
    begin
      T1 := H + BigSigma164(E) + Ch64(E, F, G) + Sha512K[I] + Block[I];
      T2 := BigSigma064(A) + Maj64(A, B, C);
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

    Inc(Offset, 128);
  end;

  SetLength(Result, 64);
  for I := 0 to 7 do
  begin
    Result[I * 8] := Byte((State[I] shr 56) and $FF);
    Result[I * 8 + 1] := Byte((State[I] shr 48) and $FF);
    Result[I * 8 + 2] := Byte((State[I] shr 40) and $FF);
    Result[I * 8 + 3] := Byte((State[I] shr 32) and $FF);
    Result[I * 8 + 4] := Byte((State[I] shr 24) and $FF);
    Result[I * 8 + 5] := Byte((State[I] shr 16) and $FF);
    Result[I * 8 + 6] := Byte((State[I] shr 8) and $FF);
    Result[I * 8 + 7] := Byte(State[I] and $FF);
  end;
end;

{$ELSE}
uses
  System.NetEncoding, System.Hash;
{$ENDIF}

constructor TScramClient.Create(const UserName: string; const Algorithm: string = 'sha256');
var
  Nonce: TBytes;
  Guid: TGUID;
begin
  inherited Create;
  FUserName := UserName;
  FAlgorithm := LowerCase(Trim(Algorithm));
  if FAlgorithm = '' then
    FAlgorithm := 'sha256';
  if (FAlgorithm <> 'sha256') and (FAlgorithm <> 'sha512') then
    raise Exception.Create('unsupported SCRAM algorithm');
  SetLength(Nonce, 18);
  Randomize;
  if Length(Nonce) > 0 then
    FillChar(Nonce[0], Length(Nonce), 0);
  if CreateGUID(Guid) = 0 then
    Move(Guid, Nonce[0], Min(SizeOf(TGUID), Length(Nonce)));
  FClientNonce := Base64Encode(Nonce);
end;

function TScramClient.ClientFirstMessage: string;
begin
  FClientFirstBare := 'n=' + EscapeValue(FUserName) + ',r=' + FClientNonce;
  Result := 'n,,' + FClientFirstBare;
end;

function TScramClient.HandleServerFirst(const Password, ServerFirst: string): string;
var
  Attrs: TStringList;
  Nonce, SaltB64, IterStr: string;
  Iterations: Integer;
  Salt, Salted, ClientKey, StoredKey, ClientSignature, ClientProof, ServerKey: TBytes;
  ClientFinalNoProof, AuthMessage: string;
begin
  Attrs := ParseAttributes(ServerFirst);
  try
    Nonce := Attrs.Values['r'];
    SaltB64 := Attrs.Values['s'];
    IterStr := Attrs.Values['i'];
  finally
    Attrs.Free;
  end;
  if (Nonce = '') or (Pos(FClientNonce, Nonce) <> 1) then
    raise Exception.Create('SCRAM server nonce mismatch');
  if (SaltB64 = '') or (IterStr = '') then
    raise Exception.Create('SCRAM server-first missing fields');
  Iterations := StrToIntDef(IterStr, 0);
  if Iterations <= 0 then
    raise Exception.Create('Invalid SCRAM iteration count');
  Salt := Base64Decode(SaltB64);
  Salted := Pbkdf2Bytes(TEncoding.UTF8.GetBytes(Password), Salt, Iterations, DerivedKeyLength);
  ClientKey := HmacBytes(Salted, TEncoding.UTF8.GetBytes('Client Key'));
  StoredKey := HashBytes(ClientKey);
  ClientFinalNoProof := 'c=biws,r=' + Nonce;
  AuthMessage := FClientFirstBare + ',' + ServerFirst + ',' + ClientFinalNoProof;
  ClientSignature := HmacBytes(StoredKey, TEncoding.UTF8.GetBytes(AuthMessage));
  ClientProof := XorBytes(ClientKey, ClientSignature);
  ServerKey := HmacBytes(Salted, TEncoding.UTF8.GetBytes('Server Key'));
  FServerSignature := HmacBytes(ServerKey, TEncoding.UTF8.GetBytes(AuthMessage));
  Result := ClientFinalNoProof + ',p=' + Base64Encode(ClientProof);
end;

procedure TScramClient.VerifyServerFinal(const ServerFinal: string);
var
  Attrs: TStringList;
  Verifier: string;
begin
  Attrs := ParseAttributes(ServerFinal);
  try
    Verifier := Attrs.Values['v'];
  finally
    Attrs.Free;
  end;
  if (Verifier = '') or (Length(FServerSignature) = 0) then
    raise Exception.Create('SCRAM server-final missing verifier');
  if Verifier <> Base64Encode(FServerSignature) then
    raise Exception.Create('SCRAM server signature mismatch');
end;

function TScramClient.EscapeValue(const Value: string): string;
begin
  Result := StringReplace(StringReplace(Value, '=', '=3D', [rfReplaceAll]), ',', '=2C', [rfReplaceAll]);
end;

function TScramClient.ParseAttributes(const Message: string): TStringList;
var
  Parts: TStringList;
  Part: string;
  SepPos: Integer;
begin
  Result := TStringList.Create;
  Result.NameValueSeparator := '=';
  Parts := TStringList.Create;
  try
    ExtractStrings([','], [], PChar(Message), Parts);
    for Part in Parts do
    begin
      SepPos := Pos('=', Part);
      if SepPos > 0 then
        Result.Values[Copy(Part, 1, SepPos - 1)] := Copy(Part, SepPos + 1, MaxInt);
    end;
  finally
    Parts.Free;
  end;
end;

function TScramClient.XorBytes(const Left, Right: TBytes): TBytes;
var
  I: Integer;
begin
  SetLength(Result, Length(Left));
  for I := 0 to Length(Left) - 1 do
    Result[I] := Left[I] xor Right[I];
end;

function TScramClient.ConcatBytes(const Left, Right: TBytes): TBytes;
begin
  SetLength(Result, Length(Left) + Length(Right));
  if Length(Left) > 0 then
    Move(Left[0], Result[0], Length(Left));
  if Length(Right) > 0 then
    Move(Right[0], Result[Length(Left)], Length(Right));
end;

function TScramClient.BytesFromInt(I: Integer): TBytes;
begin
  SetLength(Result, 4);
  Result[0] := Byte((I shr 24) and $FF);
  Result[1] := Byte((I shr 16) and $FF);
  Result[2] := Byte((I shr 8) and $FF);
  Result[3] := Byte(I and $FF);
end;

function TScramClient.Base64Encode(const Data: TBytes): string;
{$IFDEF FPC}
const
  Alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
var
  I: Integer;
  B0, B1, B2: Byte;
  Pad: Integer;
  Chunk: Integer;
begin
  Result := '';
  I := 0;
  while I < Length(Data) do
  begin
    B0 := Data[I];
    if I + 1 < Length(Data) then
      B1 := Data[I + 1]
    else
      B1 := 0;
    if I + 2 < Length(Data) then
      B2 := Data[I + 2]
    else
      B2 := 0;
    Chunk := (B0 shl 16) or (B1 shl 8) or B2;
    Pad := 0;
    if I + 1 >= Length(Data) then
      Pad := 2
    else if I + 2 >= Length(Data) then
      Pad := 1;
    Result := Result + Alphabet[((Chunk shr 18) and $3F) + 1];
    Result := Result + Alphabet[((Chunk shr 12) and $3F) + 1];
    if Pad >= 2 then
      Result := Result + '='
    else
      Result := Result + Alphabet[((Chunk shr 6) and $3F) + 1];
    if Pad >= 1 then
      Result := Result + '='
    else
      Result := Result + Alphabet[(Chunk and $3F) + 1];
    Inc(I, 3);
  end;
end;
{$ELSE}
begin
  Result := TNetEncoding.Base64.EncodeBytesToString(Data);
end;
{$ENDIF}

function TScramClient.Base64Decode(const Value: string): TBytes;
{$IFDEF FPC}
const
  Alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
var
  Map: array[0..255] of ShortInt;
  I: Integer;
  C: Char;
  Buf: array[0..3] of Integer;
  BufCount: Integer;
  OutLen: Integer;
  Triplet: Integer;
begin
  for I := 0 to 255 do
    Map[I] := -1;
  for I := 1 to Length(Alphabet) do
    Map[Ord(Alphabet[I])] := I - 1;

  SetLength(Result, 0);
  BufCount := 0;
  for C in Value do
  begin
    if C = '=' then
      Buf[BufCount] := -2
    else
      Buf[BufCount] := Map[Ord(C)];
    if Buf[BufCount] = -1 then
      Continue;
    Inc(BufCount);
    if BufCount = 4 then
    begin
      Triplet := (Buf[0] shl 18) or (Buf[1] shl 12) or ((Buf[2] and $3F) shl 6) or (Buf[3] and $3F);
      OutLen := Length(Result);
      SetLength(Result, OutLen + 3);
      Result[OutLen] := Byte((Triplet shr 16) and $FF);
      if Buf[2] <> -2 then
        Result[OutLen + 1] := Byte((Triplet shr 8) and $FF)
      else
        SetLength(Result, OutLen + 1);
      if Buf[3] <> -2 then
        Result[OutLen + 2] := Byte(Triplet and $FF)
      else if Length(Result) > OutLen + 1 then
        SetLength(Result, OutLen + 2);
      BufCount := 0;
    end;
  end;
end;
{$ELSE}
begin
  Result := TNetEncoding.Base64.DecodeStringToBytes(Value);
end;
{$ENDIF}

function TScramClient.Sha256(const Data: TBytes): TBytes;
{$IFDEF FPC}
begin
  Result := Sha256Digest(Data);
end;
{$ELSE}
begin
  Result := THashSHA2.GetHashBytes(Data);
end;
{$ENDIF}

function TScramClient.Sha512(const Data: TBytes): TBytes;
{$IFDEF FPC}
begin
  Result := Sha512Digest(Data);
end;
{$ELSE}
begin
  Result := THashSHA2.GetHashBytes(Data, SHA512);
end;
{$ENDIF}

function TScramClient.HmacSha256(const Key, Data: TBytes): TBytes;
var
  BlockSize: Integer;
  KeyBlock, OKeyPad, IKeyPad, Inner: TBytes;
  I: Integer;
begin
  BlockSize := 64;
  KeyBlock := Key;
  if Length(KeyBlock) > BlockSize then
    KeyBlock := Sha256(KeyBlock);
  SetLength(KeyBlock, BlockSize);
  SetLength(OKeyPad, BlockSize);
  SetLength(IKeyPad, BlockSize);
  for I := 0 to BlockSize - 1 do
  begin
    OKeyPad[I] := KeyBlock[I] xor $5C;
    IKeyPad[I] := KeyBlock[I] xor $36;
  end;
  Inner := Sha256(ConcatBytes(IKeyPad, Data));
  Result := Sha256(ConcatBytes(OKeyPad, Inner));
end;

function TScramClient.HmacSha512(const Key, Data: TBytes): TBytes;
var
  BlockSize: Integer;
  KeyBlock, OKeyPad, IKeyPad, Inner: TBytes;
  I: Integer;
begin
  BlockSize := 128;
  KeyBlock := Key;
  if Length(KeyBlock) > BlockSize then
    KeyBlock := Sha512(KeyBlock);
  SetLength(KeyBlock, BlockSize);
  SetLength(OKeyPad, BlockSize);
  SetLength(IKeyPad, BlockSize);
  for I := 0 to BlockSize - 1 do
  begin
    OKeyPad[I] := KeyBlock[I] xor $5C;
    IKeyPad[I] := KeyBlock[I] xor $36;
  end;
  Inner := Sha512(ConcatBytes(IKeyPad, Data));
  Result := Sha512(ConcatBytes(OKeyPad, Inner));
end;

function TScramClient.Pbkdf2Sha256(const Password, Salt: TBytes; Iterations, KeyLen: Integer): TBytes;
var
  BlockCount, I, J: Integer;
  Block: TBytes;
  U, T: TBytes;
  Counter: TBytes;
begin
  BlockCount := (KeyLen + 31) div 32;
  SetLength(Result, 0);
  for I := 1 to BlockCount do
  begin
    Counter := BytesFromInt(I);
    Block := ConcatBytes(Salt, Counter);
    U := HmacSha256(Password, Block);
    T := Copy(U, 0, Length(U));
    for J := 2 to Iterations do
    begin
      U := HmacSha256(Password, U);
      T := XorBytes(T, U);
    end;
    Result := ConcatBytes(Result, T);
  end;
  SetLength(Result, KeyLen);
end;

function TScramClient.Pbkdf2Sha512(const Password, Salt: TBytes; Iterations, KeyLen: Integer): TBytes;
var
  BlockCount, I, J: Integer;
  Block: TBytes;
  U, T: TBytes;
  Counter: TBytes;
begin
  BlockCount := (KeyLen + 63) div 64;
  SetLength(Result, 0);
  for I := 1 to BlockCount do
  begin
    Counter := BytesFromInt(I);
    Block := ConcatBytes(Salt, Counter);
    U := HmacSha512(Password, Block);
    T := Copy(U, 0, Length(U));
    for J := 2 to Iterations do
    begin
      U := HmacSha512(Password, U);
      T := XorBytes(T, U);
    end;
    Result := ConcatBytes(Result, T);
  end;
  SetLength(Result, KeyLen);
end;

function TScramClient.HashBytes(const Data: TBytes): TBytes;
begin
  if FAlgorithm = 'sha512' then
    Result := Sha512(Data)
  else
    Result := Sha256(Data);
end;

function TScramClient.HmacBytes(const Key, Data: TBytes): TBytes;
begin
  if FAlgorithm = 'sha512' then
    Result := HmacSha512(Key, Data)
  else
    Result := HmacSha256(Key, Data);
end;

function TScramClient.Pbkdf2Bytes(const Password, Salt: TBytes; Iterations, KeyLen: Integer): TBytes;
begin
  if FAlgorithm = 'sha512' then
    Result := Pbkdf2Sha512(Password, Salt, Iterations, KeyLen)
  else
    Result := Pbkdf2Sha256(Password, Salt, Iterations, KeyLen);
end;

function TScramClient.DerivedKeyLength: Integer;
begin
  if FAlgorithm = 'sha512' then
    Result := 64
  else
    Result := 32;
end;

end.
