// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

unit ScratchBird.Tls.RecordLayer;

{$mode delphi}
{$H+}

interface

uses
  SysUtils;

const
  TLS_RECORD_HEADER_SIZE = 5;
  TLS_LEGACY_VERSION_1_2 = $0303;
  TLS_MAX_RECORD_PLAINTEXT = 16384;
  TLS_MAX_RECORD_CIPHERTEXT = 16640;

  TLS_CONTENT_CHANGE_CIPHER_SPEC = 20;
  TLS_CONTENT_ALERT = 21;
  TLS_CONTENT_HANDSHAKE = 22;
  TLS_CONTENT_APPLICATION_DATA = 23;
  TLS_CONTENT_HEARTBEAT = 24;

type
  TTlsRecordHeader = record
    ContentType: Byte;
    LegacyVersion: Word;
    Length: Word;
  end;

function BuildRecord(ContentType: Byte; const Payload: TBytes;
  LegacyVersion: Word = TLS_LEGACY_VERSION_1_2): TBytes;
function ParseRecordHeader(const Frame: TBytes; out Header: TTlsRecordHeader): Boolean;
function ParseRecord(const Frame: TBytes; out Header: TTlsRecordHeader;
  out Payload: TBytes): Boolean;

implementation

function BuildRecord(ContentType: Byte; const Payload: TBytes;
  LegacyVersion: Word): TBytes;
var
  PayloadLength: Integer;
begin
  Result := nil;
  PayloadLength := Length(Payload);
  if PayloadLength > TLS_MAX_RECORD_CIPHERTEXT then
    raise Exception.Create('TLS record payload exceeds maximum allowed size.');

  SetLength(Result, TLS_RECORD_HEADER_SIZE + PayloadLength);
  Result[0] := ContentType;
  Result[1] := Byte((LegacyVersion shr 8) and $FF);
  Result[2] := Byte(LegacyVersion and $FF);
  Result[3] := Byte((PayloadLength shr 8) and $FF);
  Result[4] := Byte(PayloadLength and $FF);
  if PayloadLength > 0 then
    Move(Payload[0], Result[TLS_RECORD_HEADER_SIZE], PayloadLength);
end;

function ParseRecordHeader(const Frame: TBytes; out Header: TTlsRecordHeader): Boolean;
begin
  FillChar(Header, SizeOf(Header), 0);
  if Length(Frame) < TLS_RECORD_HEADER_SIZE then
    Exit(False);

  Header.ContentType := Frame[0];
  Header.LegacyVersion := (Word(Frame[1]) shl 8) or Word(Frame[2]);
  Header.Length := (Word(Frame[3]) shl 8) or Word(Frame[4]);

  Result := Header.Length <= TLS_MAX_RECORD_CIPHERTEXT;
end;

function ParseRecord(const Frame: TBytes; out Header: TTlsRecordHeader;
  out Payload: TBytes): Boolean;
var
  PayloadLength: Integer;
begin
  SetLength(Payload, 0);
  if not ParseRecordHeader(Frame, Header) then
    Exit(False);

  PayloadLength := Header.Length;
  if Length(Frame) <> TLS_RECORD_HEADER_SIZE + PayloadLength then
    Exit(False);

  SetLength(Payload, PayloadLength);
  if PayloadLength > 0 then
    Move(Frame[TLS_RECORD_HEADER_SIZE], Payload[0], PayloadLength);
  Result := True;
end;

end.
