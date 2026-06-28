<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

namespace ScratchBird\PDO;

final class Config
{
    public string $host = 'localhost';
    public int $port = 3092;
    public string $transport = 'inet';
    public string $ipcPath = '';
    public string $frontDoorMode = 'direct';
    public string $protocol = 'native';
    public string $database = '';
    public string $user = '';
    public string $password = '';
    public string $schema = '';
    public bool $metadataExpandSchemaParents = false;
    public string $role = '';
    public string $sslMode = 'require';
    public ?string $sslRootCert = null;
    public ?string $sslCert = null;
    public ?string $sslKey = null;
    public ?string $sslPassword = null;
    public int $connectTimeoutMs = 30000;
    public int $socketTimeoutMs = 0;
    public string $applicationName = 'scratchbird_php';
    public bool $binaryTransfer = true;
    public string $compression = 'off';
    public int $connectClientFlags = 0x0100;
    public string $authMethodId = '';
    public string $authMethodPayload = '';
    public string $authToken = '';
    public string $authPayloadJson = '';
    public string $authPayloadB64 = '';
    public string $authProviderProfile = '';
    public string $authRequiredMethods = '';
    public string $authForbiddenMethods = '';
    public bool $authRequireChannelBinding = false;
    public string $workloadIdentityToken = '';
    public string $proxyPrincipalAssertion = '';
    public int $fetchSize = 0;
    public string $managerAuthToken = '';
    public string $managerUsername = '';
    public string $managerDatabase = '';
    public string $managerConnectionProfile = 'SBsql';
    public string $managerClientIntent = 'SBsql';
    public int $managerClientFlags = 0;
    public bool $managerAuthFastPath = true;

    public static function fromDsn(string $dsn): self
    {
        $dsn = trim($dsn);
        if ($dsn === '') {
            return new self();
        }
        if (str_contains($dsn, '://')) {
            return self::parseUri($dsn);
        }
        return self::parseKeyValue($dsn);
    }

    private static function parseUri(string $dsn): self
    {
        $parts = parse_url($dsn);
        if ($parts === false || ($parts['scheme'] ?? '') !== 'scratchbird') {
            throw new \InvalidArgumentException('Unsupported DSN scheme');
        }
        $cfg = new self();
        if (!empty($parts['host'])) {
            $cfg->host = $parts['host'];
        }
        if (!empty($parts['port'])) {
            $cfg->port = (int)$parts['port'];
        }
        if (!empty($parts['user'])) {
            $cfg->user = urldecode($parts['user']);
        }
        if (!empty($parts['pass'])) {
            $cfg->password = urldecode($parts['pass']);
        }
        if (!empty($parts['path']) && $parts['path'] !== '/') {
            $cfg->database = ltrim($parts['path'], '/');
        }
        if (!empty($parts['query'])) {
            parse_str($parts['query'], $query);
            foreach ($query as $key => $value) {
                self::applyParam($cfg, (string)$key, (string)$value);
            }
        }
        return $cfg;
    }

    private static function parseKeyValue(string $dsn): self
    {
        $cfg = new self();
        $separator = str_contains($dsn, ';') ? ';' : ' ';
        $pairs = array_filter(array_map('trim', explode($separator, $dsn)));
        foreach ($pairs as $pair) {
            if (!str_contains($pair, '=')) {
                continue;
            }
            [$key, $value] = array_map('trim', explode('=', $pair, 2));
            $value = trim($value, '"');
            self::applyParam($cfg, $key, $value);
        }
        return $cfg;
    }

    private static function applyParam(self $cfg, string $key, string $value): void
    {
        switch (strtolower($key)) {
            case 'host':
            case 'server':
            case 'data source':
            case 'datasource':
                $cfg->host = $value;
                break;
            case 'port':
                $cfg->port = (int)$value;
                break;
            case 'transport':
            case 'transport_mode':
            case 'transportmode':
                $cfg->transport = self::normalizeTransport($value);
                break;
            case 'route':
                $cfg->transport = self::transportForRoute($value);
                break;
            case 'ipc_path':
            case 'ipcpath':
            case 'ipc-path':
                $cfg->ipcPath = $value;
                break;
            case 'front_door_mode':
            case 'frontdoormode':
            case 'connection_mode':
            case 'ingress_mode':
                $cfg->frontDoorMode = self::normalizeFrontDoorMode($value);
                break;
            case 'database':
            case 'dbname':
            case 'initial catalog':
                $cfg->database = $value;
                break;
            case 'protocol':
            case 'parser':
            case 'dialect':
                $cfg->protocol = self::normalizeNativeProtocol($value);
                break;
            case 'user':
            case 'username':
            case 'user id':
            case 'uid':
                $cfg->user = $value;
                break;
            case 'password':
            case 'pwd':
                $cfg->password = $value;
                break;
            case 'schema':
            case 'search_path':
            case 'searchpath':
            case 'currentschema':
                $cfg->schema = $value;
                break;
            case 'metadataexpandschemaparents':
            case 'metadata_expand_schema_parents':
            case 'expandschemaparents':
            case 'expand_schema_parents':
            case 'dbeaverexpandschemaparents':
            case 'dbeaver_expand_schema_parents':
                $cfg->metadataExpandSchemaParents = self::parseBool($value, $cfg->metadataExpandSchemaParents);
                break;
            case 'role':
                $cfg->role = $value;
                break;
            case 'sslmode':
            case 'ssl mode':
                $cfg->sslMode = $value;
                break;
            case 'sslrootcert':
                $cfg->sslRootCert = $value;
                break;
            case 'sslcert':
                $cfg->sslCert = $value;
                break;
            case 'sslkey':
                $cfg->sslKey = $value;
                break;
            case 'sslpassword':
                $cfg->sslPassword = $value;
                break;
            case 'connect_timeout':
            case 'connecttimeout':
            case 'timeout':
                $cfg->connectTimeoutMs = (int)$value * 1000;
                break;
            case 'socket_timeout':
            case 'sockettimeout':
                $cfg->socketTimeoutMs = (int)$value * 1000;
                break;
            case 'application_name':
            case 'applicationname':
                $cfg->applicationName = $value;
                break;
            case 'binary_transfer':
            case 'binarytransfer':
                $cfg->binaryTransfer = self::parseBool($value, $cfg->binaryTransfer);
                break;
            case 'compression':
                $cfg->compression = self::normalizeCompression($value);
                break;
            case 'client_flags':
            case 'connect_client_flags':
                $cfg->connectClientFlags = (int)$value;
                break;
            case 'auth_method_id':
            case 'authmethodid':
                $cfg->authMethodId = trim($value);
                break;
            case 'auth_method_payload':
            case 'authmethodpayload':
                $cfg->authMethodPayload = $value;
                break;
            case 'auth_token':
            case 'authtoken':
            case 'bearer_token':
            case 'bearertoken':
            case 'token':
                $cfg->authToken = $value;
                break;
            case 'auth_payload_json':
            case 'authpayloadjson':
                $cfg->authPayloadJson = $value;
                break;
            case 'auth_payload_b64':
            case 'authpayloadb64':
                $cfg->authPayloadB64 = $value;
                break;
            case 'auth_provider_profile':
            case 'authproviderprofile':
                $cfg->authProviderProfile = trim($value);
                break;
            case 'auth_required_methods':
            case 'authrequiredmethods':
                $cfg->authRequiredMethods = trim($value);
                break;
            case 'auth_forbidden_methods':
            case 'authforbiddenmethods':
                $cfg->authForbiddenMethods = trim($value);
                break;
            case 'auth_require_channel_binding':
            case 'authrequirechannelbinding':
                $cfg->authRequireChannelBinding = self::parseBool($value, $cfg->authRequireChannelBinding);
                break;
            case 'workload_identity_token':
            case 'workloadidentitytoken':
                $cfg->workloadIdentityToken = $value;
                break;
            case 'proxy_principal_assertion':
            case 'proxyprincipalassertion':
            case 'proxy_assertion':
                $cfg->proxyPrincipalAssertion = $value;
                break;
            case 'fetch_size':
            case 'fetchsize':
            case 'default_fetch_size':
                $cfg->fetchSize = max(0, (int)$value);
                break;
            case 'manager_auth_token':
            case 'mcp_auth_token':
                $cfg->managerAuthToken = $value;
                break;
            case 'manager_username':
            case 'mcp_username':
                $cfg->managerUsername = $value;
                break;
            case 'manager_database':
            case 'mcp_database':
                $cfg->managerDatabase = $value;
                break;
            case 'manager_connection_profile':
            case 'mcp_connection_profile':
                $cfg->managerConnectionProfile = $value;
                break;
            case 'manager_client_intent':
            case 'mcp_client_intent':
                $cfg->managerClientIntent = $value;
                break;
            case 'manager_client_flags':
            case 'mcp_client_flags':
                $cfg->managerClientFlags = (int)$value;
                break;
            case 'manager_auth_fast_path':
            case 'mcp_auth_fast_path':
                $cfg->managerAuthFastPath = self::parseBool($value, $cfg->managerAuthFastPath);
                break;
        }
    }

    private static function parseBool(string $value, bool $default): bool
    {
        $normalized = strtolower(trim($value));
        if (in_array($normalized, ['1', 'true', 'yes', 'on'], true)) {
            return true;
        }
        if (in_array($normalized, ['0', 'false', 'no', 'off'], true)) {
            return false;
        }
        return $default;
    }

    private static function normalizeNativeProtocol(string $value): string
    {
        $normalized = strtolower(trim($value));
        if ($normalized === '' ||
            $normalized === 'native' ||
            $normalized === 'scratchbird' ||
            $normalized === 'scratchbird-native' ||
            $normalized === 'scratchbird_native') {
            return 'native';
        }
        throw new \InvalidArgumentException('Only protocol=native is supported; connect to the native parser listener/port.');
    }

    private static function normalizeFrontDoorMode(string $value): string
    {
        $normalized = strtolower(trim($value));
        if ($normalized === '' || $normalized === 'direct') {
            return 'direct';
        }
        if ($normalized === 'manager_proxy' || $normalized === 'manager-proxy' || $normalized === 'managed') {
            return 'manager_proxy';
        }
        throw new \InvalidArgumentException('front_door_mode must be direct or manager_proxy.');
    }

    private static function normalizeTransport(string $value): string
    {
        $normalized = strtolower(trim($value));
        if (in_array($normalized, ['', 'inet', 'tcp', 'network'], true)) {
            return 'inet';
        }
        if (in_array($normalized, ['ipc', 'ipc_local', 'local_ipc', 'unix', 'unix_socket', 'uds'], true)) {
            return 'ipc';
        }
        if ($normalized === 'embedded') {
            return 'embedded';
        }
        throw new \InvalidArgumentException('transport must be inet, ipc, or embedded.');
    }

    private static function transportForRoute(string $value): string
    {
        $normalized = strtolower(trim($value));
        if ($normalized === 'ipc_local') {
            return 'ipc';
        }
        if ($normalized === 'embedded') {
            return 'embedded';
        }
        return 'inet';
    }

    private static function normalizeCompression(string $value): string
    {
        $normalized = strtolower(trim($value));
        if ($normalized === '' || $normalized === 'off' || $normalized === 'none') {
            return 'off';
        }
        if ($normalized === 'zstd') {
            return 'zstd';
        }
        throw new \InvalidArgumentException('compression must be off or zstd.');
    }
}
