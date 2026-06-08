// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

unit ScratchBird.Tls.Context;

{$mode delphi}
{$H+}

interface

uses
  SysUtils,
  ScratchBird.Tls.Types,
  ScratchBird.Tls.Handshake,
  ScratchBird.Tls.X509;

type
  TTlsContext = class
  private
    FConfig: TTlsConfig;
    FPeerInfo: TTlsPeerInfo;
    FLastError: TTlsError;
    FInitialized: Boolean;
    FHandshakeState: TTlsHandshakeState;
    FSocketHandle: TSocketHandle;
    FHasPeerChain: Boolean;
    FPeerChain: TTlsCertificateChain;
    FVerifyHookError: string;
    FSocket: TObject;
    FHandler: TObject;
    procedure ClearPeerInfo;
    procedure ClearLastError;
    procedure SetError(Category: TTlsErrorCategory; const MessageText: string;
      AlertCode: Integer = 0; SystemCode: Integer = 0);
    function BuildStatus(Success: Boolean): TTlsStatus;
    function ValidateConfig(out ErrorText: string): Boolean;
    function StrictVerifyMode: Boolean;
    function VerifyFullMode: Boolean;
    function IsConnected: Boolean;
    function MapTlsVersion(const VersionText: string): TTlsVersion;
    function ValidatePeerPolicy(out Category: TTlsErrorCategory; out ErrorText: string): Boolean;
    procedure CloseSocket;
    procedure HandleVerifyCertificate(Sender: TObject; var Allow: Boolean);
  public
    constructor Create;
    destructor Destroy; override;
    function Initialize(const Config: TTlsConfig): TTlsStatus;
    function Handshake: TTlsStatus; overload;
    function Handshake(var SocketHandle: TSocketHandle): TTlsStatus; overload;
    function Read(var Buffer; Count: Integer): Integer;
    function Write(const Buffer; Count: Integer): Integer;
    function Shutdown: TTlsStatus;
    function PeerInfo: TTlsPeerInfo;
    function LastError: TTlsError;
    function HandshakeState: TTlsHandshakeState;
    procedure SetPeerCertificateChain(const Chain: TTlsCertificateChain);
  end;

implementation

uses
  openssl,
  sslbase,
  sockets,
  ssockets,
  opensslsockets,
  fpopenssl;

constructor TTlsContext.Create;
begin
  inherited Create;
  FInitialized := False;
  FHandshakeState := hsIdle;
  FSocketHandle := INVALID_TLS_SOCKET_HANDLE;
  FHasPeerChain := False;
  FVerifyHookError := '';
  FSocket := nil;
  FHandler := nil;
  ClearPeerInfo;
  ClearLastError;
end;

destructor TTlsContext.Destroy;
begin
  CloseSocket;
  inherited Destroy;
end;

procedure TTlsContext.ClearPeerInfo;
begin
  FPeerInfo.Subject := '';
  FPeerInfo.Issuer := '';
  FPeerInfo.AlpnProtocol := '';
  FPeerInfo.Version := tvUnknown;
  FPeerInfo.CipherSuite := 0;
end;

procedure TTlsContext.ClearLastError;
begin
  FLastError.Category := teNone;
  FLastError.MessageText := '';
  FLastError.AlertCode := 0;
  FLastError.SystemCode := 0;
end;

procedure TTlsContext.SetError(Category: TTlsErrorCategory; const MessageText: string;
  AlertCode: Integer; SystemCode: Integer);
begin
  FLastError.Category := Category;
  FLastError.MessageText := MessageText;
  FLastError.AlertCode := AlertCode;
  FLastError.SystemCode := SystemCode;
end;

function TTlsContext.BuildStatus(Success: Boolean): TTlsStatus;
begin
  Result.Success := Success;
  Result.LastError := FLastError;
end;

function TTlsContext.StrictVerifyMode: Boolean;
begin
  Result := FConfig.Mode in [tmRequire, tmVerifyCA, tmVerifyFull];
end;

function TTlsContext.VerifyFullMode: Boolean;
begin
  Result := FConfig.Mode = tmVerifyFull;
end;

function TTlsContext.ValidateConfig(out ErrorText: string): Boolean;
begin
  ErrorText := '';
  if Trim(FConfig.ServerName) = '' then
  begin
    ErrorText := 'TLS server name/host is required.';
    Exit(False);
  end;
  if FConfig.Port = 0 then
  begin
    ErrorText := 'TLS TCP port must be in range 1..65535.';
    Exit(False);
  end;
  if FConfig.MinVersion = tvUnknown then
  begin
    ErrorText := 'TLS minimum version must be explicitly configured.';
    Exit(False);
  end;
  if FConfig.MaxVersion = tvUnknown then
  begin
    ErrorText := 'TLS maximum version must be explicitly configured.';
    Exit(False);
  end;
  if Ord(FConfig.MinVersion) > Ord(FConfig.MaxVersion) then
  begin
    ErrorText := 'TLS minimum version cannot be greater than maximum version.';
    Exit(False);
  end;
  if VerifyFullMode and (Trim(FConfig.ServerName) = '') then
  begin
    ErrorText := 'TLS verify-full mode requires a server name for hostname checks.';
    Exit(False);
  end;
  if (Trim(FConfig.ClientCertPath) = '') xor (Trim(FConfig.ClientKeyPath) = '') then
  begin
    ErrorText := 'Both client certificate and client key paths must be provided together.';
    Exit(False);
  end;
  if (FConfig.RevocationPolicy = trpHardFail) and (Trim(FConfig.RootCAPath) = '') then
  begin
    ErrorText := 'hard-fail revocation policy requires an explicit root CA path.';
    Exit(False);
  end;
  Result := True;
end;

function TTlsContext.MapTlsVersion(const VersionText: string): TTlsVersion;
var
  S: string;
begin
  S := LowerCase(Trim(VersionText));
  if Pos('1.3', S) > 0 then
    Exit(tvTLS13);
  if Pos('1.2', S) > 0 then
    Exit(tvTLS12);
  Result := tvUnknown;
end;

procedure TTlsContext.CloseSocket;
begin
  FHandler := nil;
  if Assigned(FSocket) then
    FreeAndNil(FSocket);
  FSocketHandle := INVALID_TLS_SOCKET_HANDLE;
end;

function TTlsContext.IsConnected: Boolean;
begin
  Result := Assigned(FSocket);
end;

procedure TTlsContext.HandleVerifyCertificate(Sender: TObject; var Allow: Boolean);
var
  PeerName: string;
begin
  Allow := True;
  FVerifyHookError := '';
  if not VerifyFullMode then
    Exit;

  if not (Sender is TOpenSSLSocketHandler) then
  begin
    FVerifyHookError := 'TLS verify callback sender type is invalid.';
    Allow := False;
    Exit;
  end;

  PeerName := Trim(TOpenSSLSocketHandler(Sender).SSL.PeerName);
  if PeerName = '' then
  begin
    FVerifyHookError := 'peer certificate common name is empty.';
    Allow := False;
    Exit;
  end;

  if not MatchHostname(PeerName, FConfig.ServerName) then
  begin
    FVerifyHookError := 'hostname does not match peer certificate common name.';
    Allow := False;
    Exit;
  end;
end;

function TTlsContext.ValidatePeerPolicy(out Category: TTlsErrorCategory; out ErrorText: string): Boolean;
var
  VerifyResult: Integer;
  PeerName: string;
  Handler: TOpenSSLSocketHandler;
begin
  Category := teNone;
  ErrorText := '';

  if not Assigned(FHandler) then
  begin
    Category := teIoError;
    ErrorText := 'TLS handler is not initialized.';
    Exit(False);
  end;

  Handler := TOpenSSLSocketHandler(FHandler);

  if FVerifyHookError <> '' then
  begin
    if VerifyFullMode then
      Category := teHostnameMismatch
    else
      Category := teCertificateInvalid;
    ErrorText := FVerifyHookError;
    Exit(False);
  end;

  if StrictVerifyMode then
  begin
    VerifyResult := Handler.SSL.VerifyResult;
    if VerifyResult <> X509_V_OK then
    begin
      Category := teCertificateInvalid;
      ErrorText := 'TLS certificate verification failed with code ' + IntToStr(VerifyResult) + '.';
      Exit(False);
    end;
  end;

  if VerifyFullMode then
  begin
    PeerName := Trim(Handler.SSL.PeerName);
    if PeerName = '' then
    begin
      Category := teHostnameMismatch;
      ErrorText := 'peer certificate common name is missing.';
      Exit(False);
    end;
    if not MatchHostname(PeerName, FConfig.ServerName) then
    begin
      Category := teHostnameMismatch;
      ErrorText := 'hostname does not match peer certificate common name.';
      Exit(False);
    end;
  end;

  Result := True;
end;

function TTlsContext.Initialize(const Config: TTlsConfig): TTlsStatus;
var
  ErrorText: string;
begin
  FConfig := Config;
  CloseSocket;
  ClearPeerInfo;
  ClearLastError;
  FHasPeerChain := False;
  FVerifyHookError := '';
  FHandshakeState := hsIdle;

  if not ValidateConfig(ErrorText) then
  begin
    SetError(teConfigError, ErrorText);
    FInitialized := False;
    Exit(BuildStatus(False));
  end;

  FInitialized := True;
  Result := BuildStatus(True);
end;

function TTlsContext.Handshake: TTlsStatus;
var
  SocketHandle: TSocketHandle;
begin
  SocketHandle := INVALID_TLS_SOCKET_HANDLE;
  Result := Handshake(SocketHandle);
end;

function TTlsContext.Handshake(var SocketHandle: TSocketHandle): TTlsStatus;
var
  Handler: TOpenSSLSocketHandler;
  Sock: TInetSocket;
  CipherList: string;
  PolicyCategory: TTlsErrorCategory;
  PolicyError: string;
  Detail: string;
begin
  if not FInitialized then
  begin
    SetError(teConfigError, 'Native TLS handshake requested before Initialize.');
    Exit(BuildStatus(False));
  end;

  if IsConnected then
  begin
    SocketHandle := FSocketHandle;
    ClearLastError;
    FHandshakeState := hsHandshakeComplete;
    Exit(BuildStatus(True));
  end;

  CloseSocket;
  FVerifyHookError := '';
  FHandshakeState := hsClientHelloSent;

  Handler := nil;
  Sock := nil;
  try
    if FConfig.Mode = tmDisable then
    begin
      Sock := TInetSocket.Create(FConfig.ServerName, FConfig.Port, nil);
      if FConfig.ConnectTimeoutMs > 0 then
        Sock.ConnectTimeout := FConfig.ConnectTimeoutMs;
      if FConfig.SocketTimeoutMs > 0 then
        Sock.IOTimeout := FConfig.SocketTimeoutMs;
      try
        Sock.Connect;
      except
        on E: Exception do
        begin
          SetError(teIoError, 'Socket connect failed: ' + E.Message);
          FHandshakeState := hsError;
          FreeAndNil(Sock);
          Exit(BuildStatus(False));
        end;
      end;

      FSocket := Sock;
      Sock := nil;
      FHandler := nil;
      FSocketHandle := TInetSocket(FSocket).Handle;
      SocketHandle := FSocketHandle;

      ClearPeerInfo;
      ClearLastError;
      FHandshakeState := hsHandshakeComplete;
      Exit(BuildStatus(True));
    end;

    Handler := TOpenSSLSocketHandler.Create;
    Handler.SSLType := stAny;
    Handler.VerifyPeerCert := StrictVerifyMode;
    Handler.SendHostAsSNI := True;
    Handler.OnVerifyCertificate := HandleVerifyCertificate;

    CipherList := 'HIGH:!aNULL:!eNULL:!MD5:!RC4';
    Handler.CertificateData.CipherList := CipherList;

    if Trim(FConfig.RootCAPath) <> '' then
      Handler.CertificateData.CertCA.FileName := FConfig.RootCAPath;

    if Trim(FConfig.ClientCertPath) <> '' then
      Handler.CertificateData.Certificate.FileName := FConfig.ClientCertPath;

    if Trim(FConfig.ClientKeyPath) <> '' then
      Handler.CertificateData.PrivateKey.FileName := FConfig.ClientKeyPath;

    if Trim(FConfig.ClientKeyPassword) <> '' then
      Handler.CertificateData.KeyPassword := FConfig.ClientKeyPassword;

    Sock := TInetSocket.Create(FConfig.ServerName, FConfig.Port, Handler);

    if FConfig.ConnectTimeoutMs > 0 then
      Sock.ConnectTimeout := FConfig.ConnectTimeoutMs;
    if FConfig.SocketTimeoutMs > 0 then
      Sock.IOTimeout := FConfig.SocketTimeoutMs;

    try
      Sock.Connect;
    except
      on E: Exception do
      begin
        Detail := E.Message;
        if Assigned(Handler) and (Trim(Handler.SSLLastErrorString) <> '') then
          Detail := Detail + ': ' + Trim(Handler.SSLLastErrorString);
        SetError(teHandshakeFailed, 'TLS connect failed: ' + Detail);
        FHandshakeState := hsError;
        Handler := nil; // Socket owns handler once TInetSocket is constructed.
        FreeAndNil(Sock);
        Exit(BuildStatus(False));
      end;
    end;

    FSocket := Sock;
    Sock := nil;
    FHandler := Handler;
    Handler := nil;
    FSocketHandle := TInetSocket(FSocket).Handle;
    SocketHandle := FSocketHandle;

    FPeerInfo.Subject := TOpenSSLSocketHandler(FHandler).SSL.PeerSubject;
    FPeerInfo.Issuer := TOpenSSLSocketHandler(FHandler).SSL.PeerIssuer;
    FPeerInfo.Version := MapTlsVersion(TOpenSSLSocketHandler(FHandler).SSL.Version);
    FPeerInfo.AlpnProtocol := '';
    FPeerInfo.CipherSuite := 0;

    if FPeerInfo.Version = tvUnknown then
    begin
      SetError(teHandshakeFailed, 'TLS handshake completed but protocol version is unknown.');
      FHandshakeState := hsError;
      CloseSocket;
      Exit(BuildStatus(False));
    end;

    if (FConfig.MinVersion = tvTLS13) and (FPeerInfo.Version <> tvTLS13) then
    begin
      SetError(teHandshakeFailed, 'TLS 1.3 is required for ScratchBird connections.');
      FHandshakeState := hsError;
      CloseSocket;
      Exit(BuildStatus(False));
    end;

    if not ValidatePeerPolicy(PolicyCategory, PolicyError) then
    begin
      SetError(PolicyCategory, PolicyError);
      FHandshakeState := hsError;
      CloseSocket;
      Exit(BuildStatus(False));
    end;

    ClearLastError;
    FHandshakeState := hsHandshakeComplete;
    Result := BuildStatus(True);
  finally
    if Assigned(Sock) then
      FreeAndNil(Sock);
    if Assigned(Handler) then
      FreeAndNil(Handler);
  end;
end;

function TTlsContext.Read(var Buffer; Count: Integer): Integer;
var
  ReadResult: Integer;
begin
  if Count < 0 then
  begin
    SetError(teConfigError, 'TLS read count cannot be negative.');
    Exit(-1);
  end;

  if not IsConnected then
  begin
    SetError(teIoError, 'TLS read requested while socket is not connected.');
    Exit(-1);
  end;

  ReadResult := TInetSocket(FSocket).Read(Buffer, Count);
  if ReadResult < 0 then
  begin
    SetError(teIoError, 'TLS read failed with socket error ' + IntToStr(TInetSocket(FSocket).LastError) + '.');
    Exit(-1);
  end;

  if (ReadResult = 0) and (Count > 0) then
  begin
    SetError(teIoError, 'TLS socket closed by peer.');
    Exit(-1);
  end;

  ClearLastError;
  Result := ReadResult;
end;

function TTlsContext.Write(const Buffer; Count: Integer): Integer;
var
  WriteResult: Integer;
begin
  if Count < 0 then
  begin
    SetError(teConfigError, 'TLS write count cannot be negative.');
    Exit(-1);
  end;

  if Count = 0 then
  begin
    Result := 0;
    Exit;
  end;

  if not IsConnected then
  begin
    SetError(teIoError, 'TLS write requested while socket is not connected.');
    Exit(-1);
  end;

  WriteResult := TInetSocket(FSocket).Write(Buffer, Count);
  if WriteResult < 0 then
  begin
    SetError(teIoError, 'TLS write failed with socket error ' + IntToStr(TInetSocket(FSocket).LastError) + '.');
    Exit(-1);
  end;

  ClearLastError;
  Result := WriteResult;
end;

function TTlsContext.Shutdown: TTlsStatus;
begin
  CloseSocket;
  FInitialized := False;
  FHandshakeState := hsClosed;
  ClearLastError;
  Result := BuildStatus(True);
end;

function TTlsContext.PeerInfo: TTlsPeerInfo;
begin
  Result := FPeerInfo;
end;

function TTlsContext.LastError: TTlsError;
begin
  Result := FLastError;
end;

function TTlsContext.HandshakeState: TTlsHandshakeState;
begin
  Result := FHandshakeState;
end;

procedure TTlsContext.SetPeerCertificateChain(const Chain: TTlsCertificateChain);
begin
  FPeerChain := Chain;
  FHasPeerChain := True;
end;

end.
