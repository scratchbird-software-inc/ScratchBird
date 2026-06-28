// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

unit ScratchBird.Transport.Native;

{$mode delphi}
{$H+}

interface

uses
  SysUtils,
  sockets,
  {$IFDEF UNIX}
  netdb,
  {$ENDIF}
  ScratchBird.Config, ScratchBird.Errors, ScratchBird.Transport,
  ScratchBird.Tls.Types, ScratchBird.Tls.Context;

type
  TNativeScratchBirdTransport = class(TInterfacedObject, IScratchBirdTransport)
  private
    FConfig: TScratchBirdConfig;
    FTlsConfig: TTlsConfig;
    FTlsContext: TTlsContext;
    FConnected: Boolean;
    FPlainSocket: TSocketHandle;
    function ParseTlsMode(const SSLMode: string): TTlsMode;
    procedure RaiseTlsFailure(const Stage: string; const Status: TTlsStatus);
    function UseIpcSocket: Boolean;
    function UsePlainSocket: Boolean;
    procedure ConnectPlain;
    procedure ConnectIpc;
    procedure DisconnectPlain;
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

constructor TNativeScratchBirdTransport.Create;
begin
  inherited Create;
  FTlsContext := TTlsContext.Create;
  FTlsConfig := DefaultTlsConfig;
  FConnected := False;
  FPlainSocket := INVALID_TLS_SOCKET_HANDLE;
end;

function TNativeScratchBirdTransport.ParseTlsMode(const SSLMode: string): TTlsMode;
var
  Mode: string;
begin
  Mode := LowerCase(Trim(SSLMode));
  if (Mode = '') or (Mode = 'require') then
    Exit(tmRequire);
  if Mode = 'disable' then
    Exit(tmDisable);
  if Mode = 'allow' then
    Exit(tmAllow);
  if Mode = 'prefer' then
    Exit(tmPrefer);
  if (Mode = 'verify-ca') or (Mode = 'verify_ca') then
    Exit(tmVerifyCA);
  if (Mode = 'verify-full') or (Mode = 'verify_full') then
    Exit(tmVerifyFull);
  raise EScratchbirdConnectionError.CreateWithInfo(
    'Unsupported sslmode value: ' + SSLMode,
    '08001', '', '');
end;

destructor TNativeScratchBirdTransport.Destroy;
begin
  Disconnect;
  FTlsContext.Free;
  inherited Destroy;
end;

procedure TNativeScratchBirdTransport.Configure(const Config: TScratchBirdConfig);
begin
  FConfig := Config;
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
  FTlsConfig.Mode := ParseTlsMode(Config.SSLMode);
  if UseIpcSocket then
    FTlsConfig.Mode := tmDisable;
  if FTlsConfig.Mode in [tmVerifyCA, tmVerifyFull] then
    FTlsConfig.RevocationPolicy := trpHardFail
  else
    FTlsConfig.RevocationPolicy := trpSoftFail;
  FTlsConfig.ServerName := Config.Host;
  FTlsConfig.Port := Config.Port;
  FTlsConfig.RootCAPath := Config.SSLRootCert;
  FTlsConfig.ClientCertPath := Config.SSLCert;
  FTlsConfig.ClientKeyPath := Config.SSLKey;
  FTlsConfig.ClientKeyPassword := Config.SSLPassword;
  FTlsConfig.ConnectTimeoutMs := Config.ConnectTimeoutMs;
  FTlsConfig.SocketTimeoutMs := Config.SocketTimeoutMs;
end;

procedure TNativeScratchBirdTransport.RaiseTlsFailure(const Stage: string; const Status: TTlsStatus);
var
  MessageText: string;
begin
  MessageText := Stage;
  if Status.LastError.MessageText <> '' then
    MessageText := MessageText + ': ' + Status.LastError.MessageText;
  if Status.LastError.Category = teNotImplemented then
    raise EScratchbirdNotSupported.CreateWithInfo(MessageText, '0A000', '', '');
  raise EScratchbirdConnectionError.CreateWithInfo(MessageText, '08001', '', '');
end;

function TNativeScratchBirdTransport.UseIpcSocket: Boolean;
begin
  Result := SameText(FConfig.Transport, 'ipc');
end;

function TNativeScratchBirdTransport.UsePlainSocket: Boolean;
begin
  Result := UseIpcSocket or (FTlsConfig.Mode = tmDisable);
end;

procedure TNativeScratchBirdTransport.ConnectPlain;
{$IFDEF MSWINDOWS}
begin
  if UseIpcSocket then
    raise EScratchbirdNotSupported.CreateWithInfo(
      'Unix-domain socket IPC transport is not supported by this Pascal runtime',
      '0A000', '', '');
  raise EScratchbirdNotSupported.CreateWithInfo(
    'native plain socket transport is not implemented for Windows Pascal builds',
    '0A000', '', '');
end;
{$ELSE}
var
  HostAddr: THostAddr;
  HostEntry: THostEntry;
  Address: TInetSockAddr;
begin
  if UseIpcSocket then
  begin
    ConnectIpc;
    Exit;
  end;

  if FPlainSocket <> INVALID_TLS_SOCKET_HANDLE then
  begin
    CloseSocket(FPlainSocket);
    FPlainSocket := INVALID_TLS_SOCKET_HANDLE;
  end;

  FPlainSocket := fpSocket(AF_INET, SOCK_STREAM, 0);
  if FPlainSocket < 0 then
    raise EScratchbirdConnectionError.CreateWithInfo(
      'native socket create failed: ' + IntToStr(SocketError),
      '08001', '', '');

  HostAddr := StrToHostAddr(FConfig.Host);
  if HostAddr.s_bytes[1] = 0 then
  begin
    if not ResolveHostByName(FConfig.Host, HostEntry) then
    begin
      CloseSocket(FPlainSocket);
      FPlainSocket := INVALID_TLS_SOCKET_HANDLE;
      raise EScratchbirdConnectionError.CreateWithInfo(
        'native socket resolve failed: ' + FConfig.Host,
        '08001', '', '');
    end;
    HostAddr := HostEntry.Addr;
  end;

  FillChar(Address, SizeOf(Address), 0);
  Address.sin_family := AF_INET;
  Address.sin_port := ShortHostToNet(FTlsConfig.Port);
  Address.sin_addr.s_addr := HostToNet(HostAddr.s_addr);

  if fpConnect(FPlainSocket, @Address, SizeOf(Address)) <> 0 then
  begin
    CloseSocket(FPlainSocket);
    FPlainSocket := INVALID_TLS_SOCKET_HANDLE;
    raise EScratchbirdConnectionError.CreateWithInfo(
      'native socket connect failed: ' + IntToStr(SocketError),
      '08001', '', '');
  end;
end;
{$ENDIF}

procedure TNativeScratchBirdTransport.ConnectIpc;
{$IFDEF MSWINDOWS}
begin
  raise EScratchbirdNotSupported.CreateWithInfo(
    'Unix-domain socket IPC transport is not supported by this Pascal runtime',
    '0A000', '', '');
end;
{$ELSE}
var
  Address: TUnixSockAddr;
  PathBytes: RawByteString;
  I: Integer;
begin
  if Trim(FConfig.IpcPath) = '' then
    raise EScratchbirdConnectionError.CreateWithInfo(
      'ipc_path is required for local IPC transport',
      '08001', '', '');

  if FPlainSocket <> INVALID_TLS_SOCKET_HANDLE then
  begin
    CloseSocket(FPlainSocket);
    FPlainSocket := INVALID_TLS_SOCKET_HANDLE;
  end;

  PathBytes := RawByteString(FConfig.IpcPath);
  if Length(PathBytes) >= SizeOf(Address.path) then
    raise EScratchbirdConnectionError.CreateWithInfo(
      'ipc_path is too long for a Unix-domain socket',
      '08001', '', '');

  FPlainSocket := fpSocket(AF_UNIX, SOCK_STREAM, 0);
  if FPlainSocket < 0 then
    raise EScratchbirdConnectionError.CreateWithInfo(
      'native IPC socket create failed: ' + IntToStr(SocketError),
      '08001', '', '');

  FillChar(Address, SizeOf(Address), 0);
  Address.family := AF_UNIX;
  for I := 1 to Length(PathBytes) do
    Address.path[I - 1] := PathBytes[I];

  if fpConnect(FPlainSocket, @Address, SizeOf(Address)) <> 0 then
  begin
    CloseSocket(FPlainSocket);
    FPlainSocket := INVALID_TLS_SOCKET_HANDLE;
    raise EScratchbirdConnectionError.CreateWithInfo(
      'native IPC socket connect failed: ' + IntToStr(SocketError),
      '08001', '', '');
  end;
end;
{$ENDIF}

procedure TNativeScratchBirdTransport.DisconnectPlain;
begin
  if FPlainSocket <> INVALID_TLS_SOCKET_HANDLE then
  begin
    CloseSocket(FPlainSocket);
    FPlainSocket := INVALID_TLS_SOCKET_HANDLE;
  end;
end;

procedure TNativeScratchBirdTransport.Connect;
var
  Status: TTlsStatus;
  SocketHandle: TSocketHandle;
begin
  if UsePlainSocket then
  begin
    ConnectPlain;
    FConnected := True;
    Exit;
  end;

  Status := FTlsContext.Initialize(FTlsConfig);
  if not Status.Success then
    RaiseTlsFailure('native TLS initialize failed', Status);
  SocketHandle := INVALID_TLS_SOCKET_HANDLE;
  Status := FTlsContext.Handshake(SocketHandle);
  if not Status.Success then
    RaiseTlsFailure('native TLS handshake failed', Status);
  FConnected := True;
end;

procedure TNativeScratchBirdTransport.Disconnect;
begin
  DisconnectPlain;
  FTlsContext.Shutdown;
  FConnected := False;
end;

function TNativeScratchBirdTransport.ReadExact(Length: Integer): TBytes;
var
  Status: TTlsStatus;
  Offset: Integer;
  BytesRead: Integer;
begin
  Result := nil;
  if UsePlainSocket then
  begin
    if (FPlainSocket = INVALID_TLS_SOCKET_HANDLE) or (not FConnected) then
      raise EScratchbirdConnectionError.CreateWithInfo('native socket is not connected', '08003', '', '');
    if Length <= 0 then
    begin
      SetLength(Result, 0);
      Exit;
    end;
    SetLength(Result, Length);
    Offset := 0;
    while Offset < Length do
    begin
      BytesRead := fpRecv(FPlainSocket, @Result[Offset], Length - Offset, 0);
      if BytesRead <= 0 then
      begin
        DisconnectPlain;
        FConnected := False;
        raise EScratchbirdConnectionError.CreateWithInfo(
          'native socket read failed: ' + IntToStr(SocketError),
          '08006', '', '');
      end;
      Inc(Offset, BytesRead);
    end;
    Exit;
  end;

  if Length <= 0 then
  begin
    SetLength(Result, 0);
    Exit;
  end;
  SetLength(Result, Length);
  Offset := 0;
  while Offset < Length do
  begin
    BytesRead := FTlsContext.Read(Result[Offset], Length - Offset);
    if BytesRead <= 0 then
    begin
      SetLength(Result, 0);
      Status.Success := False;
      Status.LastError := FTlsContext.LastError;
      RaiseTlsFailure('native TLS read failed', Status);
    end;
    Inc(Offset, BytesRead);
  end;
end;

procedure TNativeScratchBirdTransport.Write(const Data: TBytes);
var
  Status: TTlsStatus;
  Offset: Integer;
  BytesWritten: Integer;
begin
  if UsePlainSocket then
  begin
    if (FPlainSocket = INVALID_TLS_SOCKET_HANDLE) or (not FConnected) then
      raise EScratchbirdConnectionError.CreateWithInfo('native socket is not connected', '08003', '', '');
    if Length(Data) = 0 then
      Exit;
    Offset := 0;
    while Offset < Length(Data) do
    begin
      BytesWritten := fpSend(FPlainSocket, @Data[Offset], Length(Data) - Offset, 0);
      if BytesWritten <= 0 then
      begin
        DisconnectPlain;
        FConnected := False;
        raise EScratchbirdConnectionError.CreateWithInfo(
          'native socket write failed: ' + IntToStr(SocketError),
          '08006', '', '');
      end;
      Inc(Offset, BytesWritten);
    end;
    Exit;
  end;

  if Length(Data) = 0 then
    Exit;
  Offset := 0;
  while Offset < Length(Data) do
  begin
    BytesWritten := FTlsContext.Write(Data[Offset], Length(Data) - Offset);
    if BytesWritten <= 0 then
    begin
      Status.Success := False;
      Status.LastError := FTlsContext.LastError;
      RaiseTlsFailure('native TLS write failed', Status);
    end;
    Inc(Offset, BytesWritten);
  end;
end;

function TNativeScratchBirdTransport.IsConnected: Boolean;
begin
  Result := FConnected;
end;

end.
