// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import Foundation

public struct ScratchBirdAuthMethodSurface: Equatable {
    public let wireMethod: String
    public let pluginMethodId: String?
    public let executableLocally: Bool
    public let brokerRequired: Bool

    public init(
        wireMethod: String,
        pluginMethodId: String?,
        executableLocally: Bool,
        brokerRequired: Bool
    ) {
        self.wireMethod = wireMethod
        self.pluginMethodId = pluginMethodId
        self.executableLocally = executableLocally
        self.brokerRequired = brokerRequired
    }
}

public struct ScratchBirdAuthProbeResult: Equatable {
    public let reachable: Bool
    public let ingressMode: String
    public let resolvedHost: String
    public let resolvedPort: Int
    public let admittedMethods: [ScratchBirdAuthMethodSurface]
    public let requiredMethod: String?
    public let requiredPluginMethodId: String?
    public let allowedTransportMask: UInt64?
    public let additionalContinuationPossible: Bool

    public init(
        reachable: Bool,
        ingressMode: String,
        resolvedHost: String,
        resolvedPort: Int,
        admittedMethods: [ScratchBirdAuthMethodSurface],
        requiredMethod: String?,
        requiredPluginMethodId: String?,
        allowedTransportMask: UInt64?,
        additionalContinuationPossible: Bool
    ) {
        self.reachable = reachable
        self.ingressMode = ingressMode
        self.resolvedHost = resolvedHost
        self.resolvedPort = resolvedPort
        self.admittedMethods = admittedMethods
        self.requiredMethod = requiredMethod
        self.requiredPluginMethodId = requiredPluginMethodId
        self.allowedTransportMask = allowedTransportMask
        self.additionalContinuationPossible = additionalContinuationPossible
    }
}

public struct ScratchBirdResolvedAuthContext: Equatable {
    public let ingressMode: String
    public let resolvedAuthMethod: String?
    public let resolvedAuthPluginId: String?
    public let managerAuthenticated: Bool
    public let attached: Bool

    public init(
        ingressMode: String,
        resolvedAuthMethod: String?,
        resolvedAuthPluginId: String?,
        managerAuthenticated: Bool,
        attached: Bool
    ) {
        self.ingressMode = ingressMode
        self.resolvedAuthMethod = resolvedAuthMethod
        self.resolvedAuthPluginId = resolvedAuthPluginId
        self.managerAuthenticated = managerAuthenticated
        self.attached = attached
    }
}

let authParamMethodId = "auth_method_id"
let authParamMethodPayload = "auth_method_payload"
let authParamPayloadJson = "auth_payload_json"
let authParamPayloadB64 = "auth_payload_b64"
let authParamProviderProfile = "auth_provider_profile"
let authParamRequiredMethods = "auth_required_methods"
let authParamForbiddenMethods = "auth_forbidden_methods"
let authParamRequireChannelBinding = "auth_require_channel_binding"
let authParamWorkloadIdentityToken = "workload_identity_token"
let authParamProxyPrincipalAssertion = "proxy_principal_assertion"

func authMethodName(_ method: UInt8) -> String {
    switch method {
    case authOkMethod:
        return "OK"
    case authPasswordMethod:
        return "PASSWORD"
    case authMd5Method:
        return "MD5"
    case authScramSha256Method:
        return "SCRAM_SHA_256"
    case authScramSha512Method:
        return "SCRAM_SHA_512"
    case authTokenMethod:
        return "TOKEN"
    case authPeerMethod:
        return "PEER"
    case authReattachMethod:
        return "REATTACH"
    default:
        return ""
    }
}

func authPluginIdForMethod(_ method: UInt8, configuredMethodId: String? = nil) -> String {
    let configured = (configuredMethodId ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
    if !configured.isEmpty {
        return configured
    }
    switch method {
    case authOkMethod:
        return "scratchbird.auth.none"
    case authPasswordMethod:
        return "scratchbird.auth.password_compat"
    case authMd5Method:
        return "scratchbird.auth.md5_legacy"
    case authScramSha256Method:
        return "scratchbird.auth.scram_sha_256"
    case authScramSha512Method:
        return "scratchbird.auth.scram_sha_512"
    case authTokenMethod:
        return "scratchbird.auth.authkey_token"
    case authPeerMethod:
        return "scratchbird.auth.peer_uid"
    case authReattachMethod:
        return "scratchbird.auth.reattach"
    default:
        return ""
    }
}

func authMethodExecutableLocally(_ method: UInt8) -> Bool {
    switch method {
    case authPasswordMethod, authScramSha256Method, authScramSha512Method, authTokenMethod:
        return true
    default:
        return false
    }
}

func authMethodBrokerRequired(_ method: UInt8) -> Bool {
    method == authPeerMethod
}

func additionalContinuationPossible(_ method: UInt8) -> Bool {
    switch method {
    case authScramSha256Method, authScramSha512Method, authTokenMethod, authPeerMethod:
        return true
    default:
        return false
    }
}

func describeAuthMethod(_ method: UInt8, configuredMethodId: String? = nil) -> ScratchBirdAuthMethodSurface? {
    let methodName = authMethodName(method)
    if methodName.isEmpty {
        return nil
    }
    return ScratchBirdAuthMethodSurface(
        wireMethod: methodName,
        pluginMethodId: authPluginIdForMethod(method, configuredMethodId: configuredMethodId),
        executableLocally: authMethodExecutableLocally(method),
        brokerRequired: authMethodBrokerRequired(method)
    )
}

func defaultResolvedAuthContext(_ ingressMode: String) -> ScratchBirdResolvedAuthContext {
    ScratchBirdResolvedAuthContext(
        ingressMode: ingressMode.isEmpty ? "direct" : ingressMode,
        resolvedAuthMethod: nil,
        resolvedAuthPluginId: nil,
        managerAuthenticated: false,
        attached: false
    )
}

func applyAuthPluginSelection(
    params: inout [String: String],
    config: ScratchBirdConfig
) throws {
    let methodId = config.authMethodId?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
    if !methodId.isEmpty {
        if !methodId.hasPrefix("scratchbird.auth.") {
            throw ScratchBirdAuthorizationException(
                message: "invalid auth_method_id namespace",
                sqlState: "28000"
            )
        }
        params[authParamMethodId] = methodId
    }
    if let value = config.authMethodPayload, !value.isEmpty {
        params[authParamMethodPayload] = value
    }
    if let value = config.authPayloadJson, !value.isEmpty {
        params[authParamPayloadJson] = value
    }
    if let value = config.authPayloadB64, !value.isEmpty {
        params[authParamPayloadB64] = value
    }
    if let value = config.authProviderProfile, !value.isEmpty {
        params[authParamProviderProfile] = value
    }
    if let value = config.authRequiredMethods, !value.isEmpty {
        params[authParamRequiredMethods] = value
    }
    if let value = config.authForbiddenMethods, !value.isEmpty {
        params[authParamForbiddenMethods] = value
    }
    if config.authRequireChannelBinding {
        params[authParamRequireChannelBinding] = "1"
    }
    if let value = config.workloadIdentityToken, !value.isEmpty {
        params[authParamWorkloadIdentityToken] = value
    }
    if let value = config.proxyPrincipalAssertion, !value.isEmpty {
        params[authParamProxyPrincipalAssertion] = value
    }
}
