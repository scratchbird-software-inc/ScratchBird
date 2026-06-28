// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

class ScratchBirdConfig {
  final String host;
  final int port;
  final String protocol;
  final String frontDoorMode;
  final String database;
  final String user;
  final String? password;
  final String sslmode;
  final String? sslrootcert;
  final String? sslcert;
  final String? sslkey;
  final String? sslpassword;
  final String? ipcPath;
  final int connectTimeoutMs;
  final int socketTimeoutMs;
  final String? applicationName;
  final String? searchPath;
  final String? role;
  final bool binaryTransfer;
  final String compression;
  final int fetchSize;
  final bool metadataExpandSchemaParents;
  final int connectClientFlags;
  final String? authToken;
  final String? authMethodId;
  final String? authMethodPayload;
  final String? authPayloadJson;
  final String? authPayloadB64;
  final String? authProviderProfile;
  final String? authRequiredMethods;
  final String? authForbiddenMethods;
  final bool authRequireChannelBinding;
  final String? workloadIdentityToken;
  final String? proxyPrincipalAssertion;
  final String? dormantId;
  final String? dormantReattachToken;
  final String? managerAuthToken;
  final String? managerUsername;
  final String? managerDatabase;
  final String? managerConnectionProfile;
  final String? managerClientIntent;
  final int managerClientFlags;
  final bool managerAuthFastPath;

  const ScratchBirdConfig({
    required this.host,
    required this.port,
    this.protocol = 'native',
    this.frontDoorMode = 'direct',
    required this.database,
    required this.user,
    this.password,
    this.sslmode = 'require',
    this.sslrootcert,
    this.sslcert,
    this.sslkey,
    this.sslpassword,
    this.ipcPath,
    this.connectTimeoutMs = 5000,
    this.socketTimeoutMs = 0,
    this.applicationName,
    this.searchPath,
    this.role,
    this.binaryTransfer = true,
    this.compression = 'off',
    this.fetchSize = 0,
    this.metadataExpandSchemaParents = false,
    this.connectClientFlags = 0,
    this.authToken,
    this.authMethodId,
    this.authMethodPayload,
    this.authPayloadJson,
    this.authPayloadB64,
    this.authProviderProfile,
    this.authRequiredMethods,
    this.authForbiddenMethods,
    this.authRequireChannelBinding = false,
    this.workloadIdentityToken,
    this.proxyPrincipalAssertion,
    this.dormantId,
    this.dormantReattachToken,
    this.managerAuthToken,
    this.managerUsername,
    this.managerDatabase,
    this.managerConnectionProfile = 'SBsql',
    this.managerClientIntent = 'SBsql',
    this.managerClientFlags = 0,
    this.managerAuthFastPath = true,
  });

  factory ScratchBirdConfig.fromDsn(String dsn) {
    if (dsn.contains('://')) {
      return _fromUri(Uri.parse(dsn));
    }
    return _fromKv(dsn);
  }

  ScratchBirdConfig copyWith({
    String? host,
    int? port,
    String? protocol,
    String? frontDoorMode,
    String? database,
    String? user,
    String? password,
    String? sslmode,
    String? ipcPath,
    String? applicationName,
    String? role,
    bool? binaryTransfer,
    String? compression,
    int? fetchSize,
    bool? metadataExpandSchemaParents,
    int? connectClientFlags,
    String? authToken,
    String? authMethodId,
    String? authMethodPayload,
    String? authPayloadJson,
    String? authPayloadB64,
    String? authProviderProfile,
    String? authRequiredMethods,
    String? authForbiddenMethods,
    bool? authRequireChannelBinding,
    String? workloadIdentityToken,
    String? proxyPrincipalAssertion,
    String? dormantId,
    String? dormantReattachToken,
    String? managerAuthToken,
    String? managerUsername,
    String? managerDatabase,
    String? managerConnectionProfile,
    String? managerClientIntent,
    int? managerClientFlags,
    bool? managerAuthFastPath,
  }) {
    return ScratchBirdConfig(
      host: host ?? this.host,
      port: port ?? this.port,
      protocol: protocol ?? this.protocol,
      frontDoorMode: frontDoorMode ?? this.frontDoorMode,
      database: database ?? this.database,
      user: user ?? this.user,
      password: password ?? this.password,
      sslmode: sslmode ?? this.sslmode,
      sslrootcert: sslrootcert,
      sslcert: sslcert,
      sslkey: sslkey,
      sslpassword: sslpassword,
      ipcPath: ipcPath ?? this.ipcPath,
      connectTimeoutMs: connectTimeoutMs,
      socketTimeoutMs: socketTimeoutMs,
      applicationName: applicationName ?? this.applicationName,
      searchPath: searchPath,
      role: role ?? this.role,
      binaryTransfer: binaryTransfer ?? this.binaryTransfer,
      compression: compression ?? this.compression,
      fetchSize: fetchSize ?? this.fetchSize,
      metadataExpandSchemaParents:
          metadataExpandSchemaParents ?? this.metadataExpandSchemaParents,
      connectClientFlags: connectClientFlags ?? this.connectClientFlags,
      authToken: authToken ?? this.authToken,
      authMethodId: authMethodId ?? this.authMethodId,
      authMethodPayload: authMethodPayload ?? this.authMethodPayload,
      authPayloadJson: authPayloadJson ?? this.authPayloadJson,
      authPayloadB64: authPayloadB64 ?? this.authPayloadB64,
      authProviderProfile: authProviderProfile ?? this.authProviderProfile,
      authRequiredMethods: authRequiredMethods ?? this.authRequiredMethods,
      authForbiddenMethods: authForbiddenMethods ?? this.authForbiddenMethods,
      authRequireChannelBinding:
          authRequireChannelBinding ?? this.authRequireChannelBinding,
      workloadIdentityToken:
          workloadIdentityToken ?? this.workloadIdentityToken,
      proxyPrincipalAssertion:
          proxyPrincipalAssertion ?? this.proxyPrincipalAssertion,
      dormantId: dormantId ?? this.dormantId,
      dormantReattachToken: dormantReattachToken ?? this.dormantReattachToken,
      managerAuthToken: managerAuthToken ?? this.managerAuthToken,
      managerUsername: managerUsername ?? this.managerUsername,
      managerDatabase: managerDatabase ?? this.managerDatabase,
      managerConnectionProfile:
          managerConnectionProfile ?? this.managerConnectionProfile,
      managerClientIntent: managerClientIntent ?? this.managerClientIntent,
      managerClientFlags: managerClientFlags ?? this.managerClientFlags,
      managerAuthFastPath: managerAuthFastPath ?? this.managerAuthFastPath,
    );
  }
}

ScratchBirdConfig _fromUri(Uri uri) {
  final userInfo = uri.userInfo.split(':');
  final user = userInfo.isNotEmpty ? Uri.decodeComponent(userInfo[0]) : '';
  final password =
      userInfo.length > 1 ? Uri.decodeComponent(userInfo[1]) : null;
  final params = _normalizeParams(uri.queryParameters);
  return ScratchBirdConfig(
    host: uri.host,
    port: uri.port == 0 ? 3092 : uri.port,
    protocol: normalizeNativeProtocol(
        params['protocol'] ?? params['parser'] ?? params['dialect']),
    frontDoorMode: normalizeFrontDoorMode(params['front_door_mode']),
    database: uri.path.replaceFirst('/', ''),
    user: params['user'] ?? user,
    password: params['password'] ?? password,
    sslmode: params['sslmode'] ?? 'require',
    ipcPath: params['ipc_path'],
    applicationName: params['application_name'],
    role: params['role'],
    binaryTransfer: _parseBool(params['binary_transfer'], true),
    compression: params['compression'] ?? 'off',
    fetchSize: _parseInt(params['fetch_size'], 0),
    metadataExpandSchemaParents:
        _parseBool(params['metadata_expand_schema_parents'], false),
    connectClientFlags: _parseInt(params['connect_client_flags'], 0),
    authToken: params['auth_token'],
    authMethodId: params['auth_method_id'],
    authMethodPayload: params['auth_method_payload'],
    authPayloadJson: params['auth_payload_json'],
    authPayloadB64: params['auth_payload_b64'],
    authProviderProfile: params['auth_provider_profile'],
    authRequiredMethods: params['auth_required_methods'],
    authForbiddenMethods: params['auth_forbidden_methods'],
    authRequireChannelBinding:
        _parseBool(params['auth_require_channel_binding'], false),
    workloadIdentityToken: params['workload_identity_token'],
    proxyPrincipalAssertion: params['proxy_principal_assertion'],
    dormantId: params['dormant_id'],
    dormantReattachToken: params['dormant_reattach_token'],
    managerAuthToken: params['manager_auth_token'],
    managerUsername: params['manager_username'],
    managerDatabase: params['manager_database'],
    managerConnectionProfile: params['manager_connection_profile'] ?? 'SBsql',
    managerClientIntent: params['manager_client_intent'] ?? 'SBsql',
    managerClientFlags: _parseInt(params['manager_client_flags'], 0),
    managerAuthFastPath: _parseBool(params['manager_auth_fast_path'], true),
  );
}

ScratchBirdConfig _fromKv(String dsn) {
  final parts = dsn.split(RegExp(r'\s+'));
  final params = <String, String>{};
  for (final part in parts) {
    final idx = part.indexOf('=');
    if (idx <= 0) continue;
    params[part.substring(0, idx)] = part.substring(idx + 1);
  }
  final normalized = _normalizeParams(params);
  return ScratchBirdConfig(
    host: normalized['host'] ?? 'localhost',
    port: int.tryParse(normalized['port'] ?? '3092') ?? 3092,
    protocol: normalizeNativeProtocol(normalized['protocol'] ??
        normalized['parser'] ??
        normalized['dialect']),
    frontDoorMode: normalizeFrontDoorMode(normalized['front_door_mode']),
    database: normalized['database'] ?? normalized['dbname'] ?? '',
    user: normalized['user'] ?? '',
    password: normalized['password'],
    sslmode: normalized['sslmode'] ?? 'require',
    ipcPath: normalized['ipc_path'],
    applicationName: normalized['application_name'],
    role: normalized['role'],
    binaryTransfer: _parseBool(normalized['binary_transfer'], true),
    compression: normalized['compression'] ?? 'off',
    fetchSize: _parseInt(normalized['fetch_size'], 0),
    metadataExpandSchemaParents:
        _parseBool(normalized['metadata_expand_schema_parents'], false),
    connectClientFlags: _parseInt(normalized['connect_client_flags'], 0),
    authToken: normalized['auth_token'],
    authMethodId: normalized['auth_method_id'],
    authMethodPayload: normalized['auth_method_payload'],
    authPayloadJson: normalized['auth_payload_json'],
    authPayloadB64: normalized['auth_payload_b64'],
    authProviderProfile: normalized['auth_provider_profile'],
    authRequiredMethods: normalized['auth_required_methods'],
    authForbiddenMethods: normalized['auth_forbidden_methods'],
    authRequireChannelBinding:
        _parseBool(normalized['auth_require_channel_binding'], false),
    workloadIdentityToken: normalized['workload_identity_token'],
    proxyPrincipalAssertion: normalized['proxy_principal_assertion'],
    dormantId: normalized['dormant_id'],
    dormantReattachToken: normalized['dormant_reattach_token'],
    managerAuthToken: normalized['manager_auth_token'],
    managerUsername: normalized['manager_username'],
    managerDatabase: normalized['manager_database'],
    managerConnectionProfile:
        normalized['manager_connection_profile'] ?? 'SBsql',
    managerClientIntent: normalized['manager_client_intent'] ?? 'SBsql',
    managerClientFlags: _parseInt(normalized['manager_client_flags'], 0),
    managerAuthFastPath: _parseBool(normalized['manager_auth_fast_path'], true),
  );
}

String normalizeNativeProtocol(String? value) {
  final normalized = (value ?? '').trim().toLowerCase();
  switch (normalized) {
    case '':
    case 'native':
    case 'scratchbird':
    case 'scratchbird-native':
    case 'scratchbird_native':
      return 'native';
    default:
      throw ArgumentError(
          'Only protocol=native is supported; connect to the native parser listener/port.');
  }
}

String normalizeFrontDoorMode(String? value) {
  final normalized = (value ?? '').trim().toLowerCase();
  switch (normalized) {
    case '':
    case 'direct':
      return 'direct';
    case 'manager_proxy':
    case 'manager-proxy':
    case 'managed':
      return 'manager_proxy';
    default:
      throw ArgumentError('front_door_mode must be direct or manager_proxy.');
  }
}

bool _parseBool(String? value, bool defaultValue) {
  if (value == null || value.isEmpty) {
    return defaultValue;
  }
  final normalized = value.toLowerCase();
  return normalized == 'true' ||
      normalized == '1' ||
      normalized == 'yes' ||
      normalized == 'on';
}

int _parseInt(String? value, int defaultValue) {
  if (value == null || value.isEmpty) {
    return defaultValue;
  }
  return int.tryParse(value) ?? defaultValue;
}

Map<String, String> _normalizeParams(Map<String, String> params) {
  final out = <String, String>{};
  params.forEach((key, value) {
    final lower = key.toLowerCase();
    switch (lower) {
      case 'dbname':
        out['database'] = value;
        break;
      case 'username':
        out['user'] = value;
        break;
      case 'applicationname':
        out['application_name'] = value;
        break;
      case 'searchpath':
        out['search_path'] = value;
        break;
      case 'binarytransfer':
        out['binary_transfer'] = value;
        break;
      case 'ipcpath':
      case 'unixsocket':
      case 'socketpath':
        out['ipc_path'] = value;
        break;
      case 'connectclientflags':
        out['connect_client_flags'] = value;
        break;
      case 'authtoken':
      case 'bearertoken':
      case 'token':
        out['auth_token'] = value;
        break;
      case 'authmethodid':
        out['auth_method_id'] = value;
        break;
      case 'authmethodpayload':
        out['auth_method_payload'] = value;
        break;
      case 'authpayloadjson':
        out['auth_payload_json'] = value;
        break;
      case 'authpayloadb64':
        out['auth_payload_b64'] = value;
        break;
      case 'authproviderprofile':
        out['auth_provider_profile'] = value;
        break;
      case 'authrequiredmethods':
        out['auth_required_methods'] = value;
        break;
      case 'authforbiddenmethods':
        out['auth_forbidden_methods'] = value;
        break;
      case 'authrequirechannelbinding':
        out['auth_require_channel_binding'] = value;
        break;
      case 'workloadidentitytoken':
        out['workload_identity_token'] = value;
        break;
      case 'proxyprincipalassertion':
        out['proxy_principal_assertion'] = value;
        break;
      case 'dormantid':
        out['dormant_id'] = value;
        break;
      case 'dormantreattachtoken':
        out['dormant_reattach_token'] = value;
        break;
      case 'frontdoormode':
      case 'connection_mode':
      case 'ingress_mode':
        out['front_door_mode'] = value;
        break;
      case 'metadataexpandschemaparents':
      case 'metadata_expand_schema_parents':
      case 'expandschemaparents':
      case 'expand_schema_parents':
      case 'dbeaverexpandschemaparents':
      case 'dbeaver_expand_schema_parents':
        out['metadata_expand_schema_parents'] = value;
        break;
      case 'mcp_auth_token':
      case 'managerauthtoken':
        out['manager_auth_token'] = value;
        break;
      case 'mcp_username':
      case 'managerusername':
        out['manager_username'] = value;
        break;
      case 'mcp_database':
      case 'managerdatabase':
        out['manager_database'] = value;
        break;
      case 'mcp_connection_profile':
      case 'managerconnectionprofile':
        out['manager_connection_profile'] = value;
        break;
      case 'mcp_client_intent':
      case 'managerclientintent':
        out['manager_client_intent'] = value;
        break;
      case 'mcp_client_flags':
      case 'managerclientflags':
        out['manager_client_flags'] = value;
        break;
      case 'mcp_auth_fast_path':
      case 'managerauthfastpath':
        out['manager_auth_fast_path'] = value;
        break;
      default:
        out[lower] = value;
    }
  });
  return out;
}
