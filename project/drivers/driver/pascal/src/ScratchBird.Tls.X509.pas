// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

unit ScratchBird.Tls.X509;

{$mode delphi}
{$H+}

interface

uses
  SysUtils, ScratchBird.Tls.Types;

type
  TCertDnsNames = array of string;

  TTlsCertificateInfo = record
    SubjectCommonName: string;
    SubjectAltDnsNames: TCertDnsNames;
    NotBeforeUtc: TDateTime;
    NotAfterUtc: TDateTime;
    HasValidityWindow: Boolean;
    ServerAuthEku: Boolean;
    ServerKeyUsageAllowed: Boolean;
  end;

  TTlsCertificateChain = record
    Leaf: TTlsCertificateInfo;
    TrustedByRootStore: Boolean;
    RevocationChecked: Boolean;
    RevocationGood: Boolean;
  end;

function MatchHostname(const Pattern, Hostname: string): Boolean;
function ValidateCertificateTimes(const Cert: TTlsCertificateInfo; const MomentUtc: TDateTime;
  out ErrorText: string): Boolean;
function ValidateCertificatePurpose(const Cert: TTlsCertificateInfo; out ErrorText: string): Boolean;
function ValidateCertificateHostname(const Cert: TTlsCertificateInfo; const Hostname: string;
  out ErrorText: string): Boolean;
function ValidateCertificateChainPolicy(const Chain: TTlsCertificateChain; const Hostname: string;
  RevocationPolicy: TTlsRevocationPolicy; const MomentUtc: TDateTime;
  out ErrorCategory: TTlsErrorCategory; out ErrorText: string): Boolean;

implementation

function NormalizeName(const Value: string): string;
begin
  Result := LowerCase(Trim(Value));
  if (Result <> '') and (Result[Length(Result)] = '.') then
    Delete(Result, Length(Result), 1);
end;

function HasInvalidWildcardUsage(const Pattern: string): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to Length(Pattern) do
  begin
    if Pattern[I] <> '*' then
      Continue;
    if (I <> 1) or (Length(Pattern) < 3) or (Pattern[2] <> '.') then
      Exit(True);
  end;
end;

function MatchHostname(const Pattern, Hostname: string): Boolean;
var
  P: string;
  H: string;
  Suffix: string;
  Prefix: string;
  DotPos: Integer;
begin
  P := NormalizeName(Pattern);
  H := NormalizeName(Hostname);
  if (P = '') or (H = '') then
    Exit(False);

  if Pos('*', P) = 0 then
    Exit(P = H);

  if HasInvalidWildcardUsage(P) then
    Exit(False);

  if (Length(P) < 3) or (Copy(P, 1, 2) <> '*.') then
    Exit(False);

  Suffix := Copy(P, 2, MaxInt);  // ".example.com"
  if Pos('.', Copy(Suffix, 2, MaxInt)) = 0 then
    Exit(False); // reject patterns like "*.com"

  if (Length(H) <= Length(Suffix)) or
     (Copy(H, Length(H) - Length(Suffix) + 1, Length(Suffix)) <> Suffix) then
    Exit(False);

  Prefix := Copy(H, 1, Length(H) - Length(Suffix));
  DotPos := Pos('.', Prefix);
  Result := (Prefix <> '') and (DotPos = 0);
end;

function ValidateCertificateTimes(const Cert: TTlsCertificateInfo; const MomentUtc: TDateTime;
  out ErrorText: string): Boolean;
begin
  ErrorText := '';
  if not Cert.HasValidityWindow then
    Exit(True);
  if MomentUtc < Cert.NotBeforeUtc then
  begin
    ErrorText := 'certificate is not valid yet';
    Exit(False);
  end;
  if MomentUtc > Cert.NotAfterUtc then
  begin
    ErrorText := 'certificate has expired';
    Exit(False);
  end;
  Result := True;
end;

function ValidateCertificatePurpose(const Cert: TTlsCertificateInfo; out ErrorText: string): Boolean;
begin
  ErrorText := '';
  if not Cert.ServerAuthEku then
  begin
    ErrorText := 'certificate does not permit serverAuth extended usage';
    Exit(False);
  end;
  if not Cert.ServerKeyUsageAllowed then
  begin
    ErrorText := 'certificate key usage does not allow TLS server authentication';
    Exit(False);
  end;
  Result := True;
end;

function ValidateCertificateHostname(const Cert: TTlsCertificateInfo; const Hostname: string;
  out ErrorText: string): Boolean;
var
  I: Integer;
  Candidate: string;
begin
  ErrorText := '';
  if Length(Cert.SubjectAltDnsNames) > 0 then
  begin
    for I := 0 to High(Cert.SubjectAltDnsNames) do
    begin
      Candidate := Cert.SubjectAltDnsNames[I];
      if MatchHostname(Candidate, Hostname) then
        Exit(True);
    end;
    ErrorText := 'hostname does not match any certificate SAN DNS entry';
    Exit(False);
  end;

  if Cert.SubjectCommonName <> '' then
  begin
    if MatchHostname(Cert.SubjectCommonName, Hostname) then
      Exit(True);
    ErrorText := 'hostname does not match certificate subject common name';
    Exit(False);
  end;

  ErrorText := 'certificate contains no SAN DNS entries and no subject common name';
  Result := False;
end;

function ValidateCertificateChainPolicy(const Chain: TTlsCertificateChain; const Hostname: string;
  RevocationPolicy: TTlsRevocationPolicy; const MomentUtc: TDateTime;
  out ErrorCategory: TTlsErrorCategory; out ErrorText: string): Boolean;
begin
  ErrorCategory := teNone;
  ErrorText := '';

  if not Chain.TrustedByRootStore then
  begin
    ErrorCategory := teCertificateInvalid;
    ErrorText := 'certificate chain is not trusted by configured root CA set';
    Exit(False);
  end;

  if not ValidateCertificateTimes(Chain.Leaf, MomentUtc, ErrorText) then
  begin
    ErrorCategory := teCertificateInvalid;
    Exit(False);
  end;

  if not ValidateCertificatePurpose(Chain.Leaf, ErrorText) then
  begin
    ErrorCategory := teCertificateInvalid;
    Exit(False);
  end;

  if not ValidateCertificateHostname(Chain.Leaf, Hostname, ErrorText) then
  begin
    ErrorCategory := teHostnameMismatch;
    Exit(False);
  end;

  if RevocationPolicy <> trpDisabled then
  begin
    if not Chain.RevocationChecked then
    begin
      if RevocationPolicy = trpHardFail then
      begin
        ErrorCategory := teCertificateInvalid;
        ErrorText := 'revocation policy requires OCSP/CRL check but status is unavailable';
        Exit(False);
      end;
    end
    else if not Chain.RevocationGood then
    begin
      ErrorCategory := teCertificateInvalid;
      ErrorText := 'certificate was revoked';
      Exit(False);
    end;
  end;

  Result := True;
end;

end.
