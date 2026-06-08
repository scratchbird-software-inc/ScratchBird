// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import 'dart:convert';
import 'dart:typed_data';

import 'config.dart';
import 'errors.dart';
import 'protocol.dart';

class ScratchBirdAuthMethodSurface {
  final String wireMethod;
  final String? pluginMethodId;
  final bool executableLocally;
  final bool brokerRequired;

  const ScratchBirdAuthMethodSurface({
    required this.wireMethod,
    required this.pluginMethodId,
    required this.executableLocally,
    required this.brokerRequired,
  });
}

class ScratchBirdAuthProbeResult {
  final bool reachable;
  final String ingressMode;
  final String resolvedHost;
  final int resolvedPort;
  final List<ScratchBirdAuthMethodSurface> admittedMethods;
  final String? requiredMethod;
  final String? requiredPluginMethodId;
  final int? allowedTransportMask;
  final bool additionalContinuationPossible;

  const ScratchBirdAuthProbeResult({
    required this.reachable,
    required this.ingressMode,
    required this.resolvedHost,
    required this.resolvedPort,
    required this.admittedMethods,
    required this.requiredMethod,
    required this.requiredPluginMethodId,
    required this.allowedTransportMask,
    required this.additionalContinuationPossible,
  });
}

class ScratchBirdResolvedAuthContext {
  final String ingressMode;
  final String? resolvedAuthMethod;
  final String? resolvedAuthPluginId;
  final bool managerAuthenticated;
  final bool attached;

  const ScratchBirdResolvedAuthContext({
    required this.ingressMode,
    required this.resolvedAuthMethod,
    required this.resolvedAuthPluginId,
    required this.managerAuthenticated,
    required this.attached,
  });

  ScratchBirdResolvedAuthContext copyWith({
    String? ingressMode,
    String? resolvedAuthMethod,
    String? resolvedAuthPluginId,
    bool? managerAuthenticated,
    bool? attached,
  }) {
    return ScratchBirdResolvedAuthContext(
      ingressMode: ingressMode ?? this.ingressMode,
      resolvedAuthMethod: resolvedAuthMethod ?? this.resolvedAuthMethod,
      resolvedAuthPluginId: resolvedAuthPluginId ?? this.resolvedAuthPluginId,
      managerAuthenticated: managerAuthenticated ?? this.managerAuthenticated,
      attached: attached ?? this.attached,
    );
  }
}

const String authParamMethodId = 'auth_method_id';
const String authParamMethodPayload = 'auth_method_payload';
const String authParamPayloadJson = 'auth_payload_json';
const String authParamPayloadB64 = 'auth_payload_b64';
const String authParamProviderProfile = 'auth_provider_profile';
const String authParamRequiredMethods = 'auth_required_methods';
const String authParamForbiddenMethods = 'auth_forbidden_methods';
const String authParamRequireChannelBinding = 'auth_require_channel_binding';
const String authParamWorkloadIdentityToken = 'workload_identity_token';
const String authParamProxyPrincipalAssertion = 'proxy_principal_assertion';

String authMethodName(int method) {
  switch (method) {
    case authOkMethod:
      return 'OK';
    case authPasswordMethod:
      return 'PASSWORD';
    case authMd5Method:
      return 'MD5';
    case authScramSha256Method:
      return 'SCRAM_SHA_256';
    case authScramSha512Method:
      return 'SCRAM_SHA_512';
    case authTokenMethod:
      return 'TOKEN';
    case authPeerMethod:
      return 'PEER';
    case authReattachMethod:
      return 'REATTACH';
    default:
      return '';
  }
}

String? authPluginIdForMethod(int method, [String? configuredMethodId]) {
  final configured = configuredMethodId?.trim() ?? '';
  if (configured.isNotEmpty) {
    return configured;
  }
  switch (method) {
    case authOkMethod:
      return 'scratchbird.auth.none';
    case authPasswordMethod:
      return 'scratchbird.auth.password_compat';
    case authMd5Method:
      return 'scratchbird.auth.md5_legacy';
    case authScramSha256Method:
      return 'scratchbird.auth.scram_sha_256';
    case authScramSha512Method:
      return 'scratchbird.auth.scram_sha_512';
    case authTokenMethod:
      return 'scratchbird.auth.authkey_token';
    case authPeerMethod:
      return 'scratchbird.auth.peer_uid';
    case authReattachMethod:
      return 'scratchbird.auth.reattach';
    default:
      return null;
  }
}

bool authMethodExecutableLocally(int method) {
  switch (method) {
    case authPasswordMethod:
    case authScramSha256Method:
    case authScramSha512Method:
    case authTokenMethod:
      return true;
    default:
      return false;
  }
}

bool authMethodBrokerRequired(int method) {
  return method == authPeerMethod;
}

bool additionalContinuationPossibleForAuthMethod(int method) {
  switch (method) {
    case authScramSha256Method:
    case authScramSha512Method:
    case authTokenMethod:
    case authPeerMethod:
      return true;
    default:
      return false;
  }
}

ScratchBirdAuthMethodSurface? describeAuthMethod(
  int method, {
  String? configuredMethodId,
}) {
  final methodName = authMethodName(method);
  if (methodName.isEmpty) {
    return null;
  }
  return ScratchBirdAuthMethodSurface(
    wireMethod: methodName,
    pluginMethodId: authPluginIdForMethod(method, configuredMethodId),
    executableLocally: authMethodExecutableLocally(method),
    brokerRequired: authMethodBrokerRequired(method),
  );
}

ScratchBirdResolvedAuthContext defaultResolvedAuthContext([
  String ingressMode = 'direct',
]) {
  return ScratchBirdResolvedAuthContext(
    ingressMode: ingressMode.isEmpty ? 'direct' : ingressMode,
    resolvedAuthMethod: null,
    resolvedAuthPluginId: null,
    managerAuthenticated: false,
    attached: false,
  );
}

void applyAuthPluginSelection(
  Map<String, String> params,
  ScratchBirdConfig config,
) {
  final methodId = config.authMethodId?.trim() ?? '';
  if (methodId.isNotEmpty) {
    if (!methodId.startsWith('scratchbird.auth.')) {
      throw const ScratchBirdAuthException(
        'invalid auth_method_id namespace',
        sqlState: '28000',
      );
    }
    params[authParamMethodId] = methodId;
  }
  if (config.authMethodPayload?.isNotEmpty == true) {
    params[authParamMethodPayload] = config.authMethodPayload!;
  }
  if (config.authPayloadJson?.isNotEmpty == true) {
    params[authParamPayloadJson] = config.authPayloadJson!;
  }
  if (config.authPayloadB64?.isNotEmpty == true) {
    params[authParamPayloadB64] = config.authPayloadB64!;
  }
  if (config.authProviderProfile?.isNotEmpty == true) {
    params[authParamProviderProfile] = config.authProviderProfile!;
  }
  if (config.authRequiredMethods?.isNotEmpty == true) {
    params[authParamRequiredMethods] = config.authRequiredMethods!;
  }
  if (config.authForbiddenMethods?.isNotEmpty == true) {
    params[authParamForbiddenMethods] = config.authForbiddenMethods!;
  }
  if (config.authRequireChannelBinding) {
    params[authParamRequireChannelBinding] = 'true';
  }
  if (config.workloadIdentityToken?.isNotEmpty == true) {
    params[authParamWorkloadIdentityToken] = config.workloadIdentityToken!;
  }
  if (config.proxyPrincipalAssertion?.isNotEmpty == true) {
    params[authParamProxyPrincipalAssertion] = config.proxyPrincipalAssertion!;
  }
}

Uint8List? resolveTokenAuthPayload(ScratchBirdConfig config) {
  if (config.authToken?.isNotEmpty == true) {
    return Uint8List.fromList(utf8.encode(config.authToken!));
  }
  if (config.authMethodPayload?.isNotEmpty == true) {
    return Uint8List.fromList(utf8.encode(config.authMethodPayload!));
  }
  if (config.authPayloadB64?.isNotEmpty == true) {
    return Uint8List.fromList(base64.decode(config.authPayloadB64!));
  }
  if (config.authPayloadJson?.isNotEmpty == true) {
    return Uint8List.fromList(utf8.encode(config.authPayloadJson!));
  }
  if (config.workloadIdentityToken?.isNotEmpty == true) {
    return Uint8List.fromList(utf8.encode(config.workloadIdentityToken!));
  }
  if (config.proxyPrincipalAssertion?.isNotEmpty == true) {
    return Uint8List.fromList(utf8.encode(config.proxyPrincipalAssertion!));
  }
  return null;
}
