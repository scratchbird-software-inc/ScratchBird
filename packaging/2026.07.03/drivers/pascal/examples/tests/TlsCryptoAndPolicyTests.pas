// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

program TlsCryptoAndPolicyTests;

{$mode delphi}
{$APPTYPE CONSOLE}

uses
  SysUtils, DateUtils,
  ScratchBird.Tls.Crypto, ScratchBird.Tls.X509, ScratchBird.Tls.Types;

procedure Fail(const MessageText: string);
begin
  raise Exception.Create(MessageText);
end;

procedure AssertTrue(Value: Boolean; const MessageText: string);
begin
  if not Value then
    Fail(MessageText);
end;

procedure AssertEqualText(const Expected, Actual, MessageText: string);
begin
  if Expected <> Actual then
    Fail(MessageText + ': expected=' + Expected + ' actual=' + Actual);
end;

function HexNibble(Value: Char): Byte;
begin
  case Value of
    '0'..'9': Result := Byte(Ord(Value) - Ord('0'));
    'a'..'f': Result := Byte(Ord(Value) - Ord('a') + 10);
    'A'..'F': Result := Byte(Ord(Value) - Ord('A') + 10);
  else
    raise Exception.Create('invalid hex character: ' + Value);
  end;
end;

function HexToBytes(const Hex: string): TBytes;
var
  Clean: string;
  I: Integer;
begin
  Result := nil;
  Clean := StringReplace(Hex, ' ', '', [rfReplaceAll]);
  if (Length(Clean) mod 2) <> 0 then
    raise Exception.Create('hex string length must be even');
  SetLength(Result, Length(Clean) div 2);
  for I := 0 to Length(Result) - 1 do
    Result[I] := (HexNibble(Clean[(I * 2) + 1]) shl 4) or HexNibble(Clean[(I * 2) + 2]);
end;

function BytesToHex(const Data: TBytes): string;
const
  Digits: array[0..15] of Char = '0123456789abcdef';
var
  I: Integer;
begin
  SetLength(Result, Length(Data) * 2);
  for I := 0 to Length(Data) - 1 do
  begin
    Result[(I * 2) + 1] := Digits[(Data[I] shr 4) and $0F];
    Result[(I * 2) + 2] := Digits[Data[I] and $0F];
  end;
end;

function Utf8Bytes(const S: string): TBytes;
var
  U: UTF8String;
  I: Integer;
begin
  Result := nil;
  U := UTF8String(S);
  SetLength(Result, Length(U));
  for I := 1 to Length(U) do
    Result[I - 1] := Byte(U[I]);
end;

procedure TestSha256;
var
  Digest: TBytes;
begin
  Digest := Sha256(Utf8Bytes('abc'));
  AssertEqualText(
    'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad',
    BytesToHex(Digest),
    'SHA-256 test vector'
  );
end;

procedure TestHmacSha256;
var
  Digest: TBytes;
begin
  Digest := HmacSha256(
    Utf8Bytes('key'),
    Utf8Bytes('The quick brown fox jumps over the lazy dog')
  );
  AssertEqualText(
    'f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8',
    BytesToHex(Digest),
    'HMAC-SHA256 test vector'
  );
end;

procedure TestHkdfRfc5869Case1;
var
  IKM, Salt, Info, Okm: TBytes;
begin
  IKM := HexToBytes('0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b');
  Salt := HexToBytes('000102030405060708090a0b0c');
  Info := HexToBytes('f0f1f2f3f4f5f6f7f8f9');
  Okm := HkdfExpand(HkdfExtract(Salt, IKM), Info, 42);
  AssertEqualText(
    '3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865',
    BytesToHex(Okm),
    'HKDF RFC5869 case 1'
  );
end;

procedure TestHostnameRules;
begin
  AssertTrue(MatchHostname('db.internal.example.com', 'db.internal.example.com'), 'exact match');
  AssertTrue(MatchHostname('*.example.com', 'api.example.com'), 'single-label wildcard');
  AssertTrue(not MatchHostname('*.example.com', 'a.b.example.com'), 'wildcard must not span labels');
  AssertTrue(not MatchHostname('*.com', 'example.com'), 'reject top-level wildcard');
end;

procedure TestSanPrecedence;
var
  Cert: TTlsCertificateInfo;
  Err: string;
begin
  Cert.SubjectCommonName := 'db.example.com';
  SetLength(Cert.SubjectAltDnsNames, 1);
  Cert.SubjectAltDnsNames[0] := 'api.example.com';
  AssertTrue(
    not ValidateCertificateHostname(Cert, 'db.example.com', Err),
    'SAN entries must take precedence over CN'
  );
end;

procedure TestChainPolicyChecks;
var
  Chain: TTlsCertificateChain;
  Err: string;
  Cat: TTlsErrorCategory;
  NowUtc: TDateTime;
begin
  NowUtc := Now;
  Chain.TrustedByRootStore := True;
  Chain.RevocationChecked := True;
  Chain.RevocationGood := True;
  Chain.Leaf.SubjectCommonName := 'db.example.com';
  SetLength(Chain.Leaf.SubjectAltDnsNames, 1);
  Chain.Leaf.SubjectAltDnsNames[0] := 'db.example.com';
  Chain.Leaf.HasValidityWindow := True;
  Chain.Leaf.NotBeforeUtc := IncHour(NowUtc, -1);
  Chain.Leaf.NotAfterUtc := IncHour(NowUtc, 1);
  Chain.Leaf.ServerAuthEku := True;
  Chain.Leaf.ServerKeyUsageAllowed := True;

  AssertTrue(
    ValidateCertificateChainPolicy(Chain, 'db.example.com', trpSoftFail, NowUtc, Cat, Err),
    'valid chain policy should pass'
  );

  Chain.RevocationChecked := False;
  AssertTrue(
    not ValidateCertificateChainPolicy(Chain, 'db.example.com', trpHardFail, NowUtc, Cat, Err),
    'hard-fail revocation policy should fail when status unavailable'
  );
end;

begin
  try
    TestSha256;
    TestHmacSha256;
    TestHkdfRfc5869Case1;
    TestHostnameRules;
    TestSanPrecedence;
    TestChainPolicyChecks;
    Writeln('TlsCryptoAndPolicyTests: OK');
  except
    on E: Exception do
    begin
      Writeln('TlsCryptoAndPolicyTests: FAILED - ' + E.Message);
      Halt(1);
    end;
  end;
end.
