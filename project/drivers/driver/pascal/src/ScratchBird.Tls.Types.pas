// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

unit ScratchBird.Tls.Types;

{$mode delphi}
{$H+}

interface

uses
  SysUtils;

type
  TSocketHandle = PtrInt;

const
  INVALID_TLS_SOCKET_HANDLE = TSocketHandle(-1);

type
  TTlsMode = (tmDisable, tmAllow, tmPrefer, tmRequire, tmVerifyCA, tmVerifyFull);
  TTlsVersion = (tvUnknown, tvTLS12, tvTLS13);
  TTlsRevocationPolicy = (trpDisabled, trpSoftFail, trpHardFail);

  TTlsErrorCategory = (
    teNone,
    teHandshakeFailed,
    teCertificateInvalid,
    teHostnameMismatch,
    teIoError,
    teProtocolError,
    teConfigError,
    teNotImplemented
  );

  TTlsConfig = record
    Mode: TTlsMode;
    ServerName: string;
    Port: Word;
    RootCAPath: string;
    ClientCertPath: string;
    ClientKeyPath: string;
    ClientKeyPassword: string;
    MinVersion: TTlsVersion;
    MaxVersion: TTlsVersion;
    RevocationPolicy: TTlsRevocationPolicy;
    ConnectTimeoutMs: Integer;
    SocketTimeoutMs: Integer;
  end;

  TTlsPeerInfo = record
    Subject: string;
    Issuer: string;
    AlpnProtocol: string;
    Version: TTlsVersion;
    CipherSuite: Cardinal;
  end;

  TTlsError = record
    Category: TTlsErrorCategory;
    MessageText: string;
    AlertCode: Integer;
    SystemCode: Integer;
  end;

  TTlsStatus = record
    Success: Boolean;
    LastError: TTlsError;
  end;

function DefaultTlsConfig: TTlsConfig;

implementation

function DefaultTlsConfig: TTlsConfig;
begin
  Result.Mode := tmRequire;
  Result.ServerName := '';
  Result.Port := 3092;
  Result.RootCAPath := '';
  Result.ClientCertPath := '';
  Result.ClientKeyPath := '';
  Result.ClientKeyPassword := '';
  Result.MinVersion := tvTLS13;
  Result.MaxVersion := tvTLS13;
  Result.RevocationPolicy := trpSoftFail;
  Result.ConnectTimeoutMs := 30000;
  Result.SocketTimeoutMs := 0;
end;

end.
