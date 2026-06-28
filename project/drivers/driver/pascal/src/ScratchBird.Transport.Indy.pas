// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

unit ScratchBird.Transport.Indy;

{$mode delphi}
{$H+}

interface

uses
  SysUtils, Classes,
  ScratchBird.Config, ScratchBird.Errors, ScratchBird.Transport,
  IdTCPClient, IdSSL, IdSSLOpenSSL;

type
  TIndyScratchBirdTransport = class(TInterfacedObject, IScratchBirdTransport)
  private
    FConfig: TScratchBirdConfig;
    FTcp: TIdTCPClient;
    FSSL: TIdSSLIOHandlerSocketOpenSSL;
    FConnected: Boolean;
  public
    constructor Create;
    destructor Destroy; override;
    procedure Configure(const Config: TScratchBirdConfig);
    procedure Connect;
    procedure Disconnect;
    function ReadExact(Length: Integer): TBytes;
    procedure Write(const Data: TBytes);
    function IsConnected: Boolean;
  end;

implementation

constructor TIndyScratchBirdTransport.Create;
begin
  inherited Create;
  FTcp := TIdTCPClient.Create(nil);
  FSSL := TIdSSLIOHandlerSocketOpenSSL.Create(nil);
  FTcp.IOHandler := FSSL;
  FConnected := False;
end;

destructor TIndyScratchBirdTransport.Destroy;
begin
  Disconnect;
  FSSL.Free;
  FTcp.Free;
  inherited Destroy;
end;

procedure TIndyScratchBirdTransport.Configure(const Config: TScratchBirdConfig);
begin
  FConfig := Config;
end;

procedure TIndyScratchBirdTransport.Connect;
var
  Mode: string;
begin
  Mode := LowerCase(FConfig.SSLMode);
  if Trim(FConfig.Transport) = '' then
    FConfig.Transport := 'inet';
  if (not SameText(FConfig.Transport, 'inet')) and
     (not SameText(FConfig.Transport, 'ipc')) and
     (not SameText(FConfig.Transport, 'embedded')) then
    raise EScratchbirdNotSupported.CreateWithInfo(
      'transport must be inet, ipc, or embedded',
      '0A000', '', '');
  if SameText(FConfig.Transport, 'embedded') then
    raise EScratchbirdNotSupported.CreateWithInfo(
      'embedded transport is not supported by the Pascal driver; no ScratchBird C++ library boundary is exposed',
      '0A000', '', '');
  if SameText(FConfig.Transport, 'ipc') then
    raise EScratchbirdNotSupported.CreateWithInfo(
      'Unix-domain socket IPC transport is not supported by the Pascal Indy transport',
      '0A000', '', '');
  if Mode = 'disable' then
    raise EScratchbirdConnectionError.CreateWithInfo(
      'TLS is required for ScratchBird connections', '08001', '', '');

  FTcp.Host := FConfig.Host;
  FTcp.Port := FConfig.Port;
  FTcp.ConnectTimeout := FConfig.ConnectTimeoutMs;
  FTcp.ReadTimeout := FConfig.SocketTimeoutMs;

  {$IFDEF SCRATCHBIRD_TLS13}
  FSSL.SSLOptions.Method := sslvTLSv1_3;
  {$ELSE}
  raise EScratchbirdNotSupported.CreateWithInfo(
    'TLS 1.3 is required but Indy lacks TLS 1.3 support', '0A000', '', '');
  {$ENDIF}

  FSSL.SSLOptions.Mode := sslmClient;
  if FConfig.SSLCert <> '' then
    FSSL.SSLOptions.CertFile := FConfig.SSLCert;
  if FConfig.SSLKey <> '' then
    FSSL.SSLOptions.KeyFile := FConfig.SSLKey;
  if FConfig.SSLRootCert <> '' then
    FSSL.SSLOptions.RootCertFile := FConfig.SSLRootCert;
  if (Mode = 'verify-full') or (Mode = 'verify-ca') or (Mode = 'require') then
    FSSL.SSLOptions.VerifyMode := [sslvrfPeer]
  else
    FSSL.SSLOptions.VerifyMode := [];

  FTcp.IOHandler := FSSL;
  FTcp.Connect;
  FConnected := True;
end;

procedure TIndyScratchBirdTransport.Disconnect;
begin
  if FTcp.Connected then
    FTcp.Disconnect;
  FConnected := False;
end;

function TIndyScratchBirdTransport.ReadExact(Length: Integer): TBytes;
begin
  SetLength(Result, Length);
  FTcp.IOHandler.ReadBytes(Result, Length, False);
end;

procedure TIndyScratchBirdTransport.Write(const Data: TBytes);
begin
  if Length(Data) = 0 then
    Exit;
  FTcp.IOHandler.Write(Data);
end;

function TIndyScratchBirdTransport.IsConnected: Boolean;
begin
  Result := FConnected and FTcp.Connected;
end;

end.
