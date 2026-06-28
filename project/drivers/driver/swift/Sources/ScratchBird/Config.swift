// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import Foundation

func normalizeNativeProtocol(_ value: String?) throws -> String {
    let normalized = (value ?? "").trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    switch normalized {
    case "", "native", "scratchbird", "scratchbird-native", "scratchbird_native":
        return "native"
    default:
        throw NSError(
            domain: "ScratchBird",
            code: -1,
            userInfo: [NSLocalizedDescriptionKey: "Only protocol=native is supported; connect to the native parser listener/port."]
        )
    }
}

func normalizeFrontDoorMode(_ value: String?) throws -> String {
    let normalized = (value ?? "").trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    switch normalized {
    case "", "direct":
        return "direct"
    case "manager_proxy", "manager-proxy", "managed":
        return "manager_proxy"
    default:
        throw NSError(
            domain: "ScratchBird",
            code: -1,
            userInfo: [NSLocalizedDescriptionKey: "front_door_mode must be direct or manager_proxy."]
        )
    }
}

func normalizeSslMode(_ value: String?) throws -> String {
    let normalized = (value ?? "require").trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    switch normalized {
    case "verify_ca":
        return "verify-ca"
    case "verify_full":
        return "verify-full"
    case "disable", "allow", "prefer", "require", "verify-ca", "verify-full":
        return normalized
    default:
        throw NSError(
            domain: "ScratchBird",
            code: -1,
            userInfo: [NSLocalizedDescriptionKey: "Unsupported sslmode value: \(value ?? "")"]
        )
    }
}

private func parseBool(_ value: String?, default defaultValue: Bool) -> Bool {
    guard let value else { return defaultValue }
    switch value.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() {
    case "true", "1", "yes", "on":
        return true
    case "false", "0", "no", "off":
        return false
    default:
        return defaultValue
    }
}

private func parseInt(_ value: String?, default defaultValue: Int) -> Int {
    guard let value, let parsed = Int(value) else { return defaultValue }
    return parsed
}

private func normalizeConnectionParams(_ raw: [String: String]) -> [String: String] {
    var out: [String: String] = [:]
    for (key, value) in raw {
        let lower = key.lowercased()
        switch lower {
        case "dbname":
            out["database"] = value
        case "username":
            out["user"] = value
        case "applicationname":
            out["application_name"] = value
        case "searchpath":
            out["search_path"] = value
        case "binarytransfer":
            out["binary_transfer"] = value
        case "ipcpath", "unixsocket", "socketpath":
            out["ipc_path"] = value
        case "frontdoormode", "connection_mode", "ingress_mode":
            out["front_door_mode"] = value
        case "mcp_auth_token":
            out["manager_auth_token"] = value
        case "auth_token", "authtoken", "bearer_token", "bearertoken", "token":
            out["auth_token"] = value
        case "mcp_username":
            out["manager_username"] = value
        case "mcp_database":
            out["manager_database"] = value
        case "mcp_connection_profile":
            out["manager_connection_profile"] = value
        case "mcp_client_intent":
            out["manager_client_intent"] = value
        case "mcp_client_flags":
            out["manager_client_flags"] = value
        case "mcp_auth_fast_path":
            out["manager_auth_fast_path"] = value
        case "connectclientflags":
            out["connect_client_flags"] = value
        case "authmethodid":
            out["auth_method_id"] = value
        case "authmethodpayload":
            out["auth_method_payload"] = value
        case "authpayloadjson":
            out["auth_payload_json"] = value
        case "authpayloadb64":
            out["auth_payload_b64"] = value
        case "authproviderprofile":
            out["auth_provider_profile"] = value
        case "authrequiredmethods":
            out["auth_required_methods"] = value
        case "authforbiddenmethods":
            out["auth_forbidden_methods"] = value
        case "authrequirechannelbinding":
            out["auth_require_channel_binding"] = value
        case "workloadidentitytoken":
            out["workload_identity_token"] = value
        case "proxyprincipalassertion":
            out["proxy_principal_assertion"] = value
        case "dormantid":
            out["dormant_id"] = value
        case "dormantreattachtoken":
            out["dormant_reattach_token"] = value
        default:
            out[lower] = value
        }
    }
    return out
}

public struct ScratchBirdConfig {
    public var host: String
    public var port: Int
    public var protocolName: String
    public var frontDoorMode: String
    public var database: String
    public var user: String
    public var password: String?
    public var sslmode: String
    public var sslrootcert: String?
    public var sslcert: String?
    public var sslkey: String?
    public var sslpassword: String?
    public var ipcPath: String?
    public var applicationName: String?
    public var searchPath: String?
    public var role: String?
    public var binaryTransfer: Bool
    public var compression: String
    public var fetchSize: Int
    public var connectClientFlags: Int
    public var managerAuthToken: String?
    public var managerUsername: String?
    public var managerDatabase: String?
    public var managerConnectionProfile: String
    public var managerClientIntent: String
    public var managerClientFlags: Int
    public var managerAuthFastPath: Bool
    public var authToken: String?
    public var authMethodId: String?
    public var authMethodPayload: String?
    public var authPayloadJson: String?
    public var authPayloadB64: String?
    public var authProviderProfile: String?
    public var authRequiredMethods: String?
    public var authForbiddenMethods: String?
    public var authRequireChannelBinding: Bool
    public var workloadIdentityToken: String?
    public var proxyPrincipalAssertion: String?
    public var dormantId: String?
    public var dormantReattachToken: String?
    public var keepaliveIntervalMs: Int
    public var keepaliveMaxIdleBeforeCheckMs: Int
    public var keepaliveValidationTimeoutMs: Int
    public var leakDetectionThresholdMs: Int
    public var leakDetectionCheckIntervalMs: Int
    public var leakDetectionCaptureStackTrace: Bool

    public init(
        host: String = "localhost",
        port: Int = 3092,
        protocolName: String = "native",
        frontDoorMode: String = "direct",
        database: String,
        user: String,
        password: String? = nil,
        sslmode: String = "require",
        sslrootcert: String? = nil,
        sslcert: String? = nil,
        sslkey: String? = nil,
        sslpassword: String? = nil,
        ipcPath: String? = nil,
        applicationName: String? = nil,
        searchPath: String? = nil,
        role: String? = nil,
        binaryTransfer: Bool = true,
        compression: String = "off",
        fetchSize: Int = 0,
        connectClientFlags: Int = 0,
        managerAuthToken: String? = nil,
        managerUsername: String? = nil,
        managerDatabase: String? = nil,
        managerConnectionProfile: String = "SBsql",
        managerClientIntent: String = "SBsql",
        managerClientFlags: Int = 0,
        managerAuthFastPath: Bool = true,
        authToken: String? = nil,
        authMethodId: String? = nil,
        authMethodPayload: String? = nil,
        authPayloadJson: String? = nil,
        authPayloadB64: String? = nil,
        authProviderProfile: String? = nil,
        authRequiredMethods: String? = nil,
        authForbiddenMethods: String? = nil,
        authRequireChannelBinding: Bool = false,
        workloadIdentityToken: String? = nil,
        proxyPrincipalAssertion: String? = nil,
        dormantId: String? = nil,
        dormantReattachToken: String? = nil,
        keepaliveIntervalMs: Int = 120_000,
        keepaliveMaxIdleBeforeCheckMs: Int = 600_000,
        keepaliveValidationTimeoutMs: Int = 5_000,
        leakDetectionThresholdMs: Int = 30_000,
        leakDetectionCheckIntervalMs: Int = 10_000,
        leakDetectionCaptureStackTrace: Bool = false
    ) {
        self.host = host
        self.port = port
        self.protocolName = protocolName
        self.frontDoorMode = frontDoorMode
        self.database = database
        self.user = user
        self.password = password
        self.sslmode = sslmode
        self.sslrootcert = sslrootcert
        self.sslcert = sslcert
        self.sslkey = sslkey
        self.sslpassword = sslpassword
        self.ipcPath = ipcPath
        self.applicationName = applicationName
        self.searchPath = searchPath
        self.role = role
        self.binaryTransfer = binaryTransfer
        self.compression = compression
        self.fetchSize = fetchSize
        self.connectClientFlags = connectClientFlags
        self.managerAuthToken = managerAuthToken
        self.managerUsername = managerUsername
        self.managerDatabase = managerDatabase
        self.managerConnectionProfile = managerConnectionProfile
        self.managerClientIntent = managerClientIntent
        self.managerClientFlags = managerClientFlags
        self.managerAuthFastPath = managerAuthFastPath
        self.authToken = authToken
        self.authMethodId = authMethodId
        self.authMethodPayload = authMethodPayload
        self.authPayloadJson = authPayloadJson
        self.authPayloadB64 = authPayloadB64
        self.authProviderProfile = authProviderProfile
        self.authRequiredMethods = authRequiredMethods
        self.authForbiddenMethods = authForbiddenMethods
        self.authRequireChannelBinding = authRequireChannelBinding
        self.workloadIdentityToken = workloadIdentityToken
        self.proxyPrincipalAssertion = proxyPrincipalAssertion
        self.dormantId = dormantId
        self.dormantReattachToken = dormantReattachToken
        self.keepaliveIntervalMs = keepaliveIntervalMs
        self.keepaliveMaxIdleBeforeCheckMs = keepaliveMaxIdleBeforeCheckMs
        self.keepaliveValidationTimeoutMs = keepaliveValidationTimeoutMs
        self.leakDetectionThresholdMs = leakDetectionThresholdMs
        self.leakDetectionCheckIntervalMs = leakDetectionCheckIntervalMs
        self.leakDetectionCaptureStackTrace = leakDetectionCaptureStackTrace
    }

    public init(dsn: String) {
        self.init(database: "", user: "")
        if dsn.contains("://"), let url = URLComponents(string: dsn) {
            let userInfo = url.user
            let passInfo = url.password
            let query = url.queryItems ?? []
            let rawParams = Dictionary(uniqueKeysWithValues: query.map { ($0.name, $0.value ?? "") })
            let params = normalizeConnectionParams(rawParams)
            self.host = url.host ?? "localhost"
            self.port = url.port ?? 3092
            self.protocolName = params["protocol"] ?? params["parser"] ?? params["dialect"] ?? "native"
            self.frontDoorMode = params["front_door_mode"] ?? "direct"
            self.database = url.path.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
            self.user = params["user"] ?? userInfo ?? ""
            self.password = params["password"] ?? passInfo
            self.sslmode = params["sslmode"] ?? "require"
            self.sslrootcert = params["sslrootcert"]
            self.sslcert = params["sslcert"]
            self.sslkey = params["sslkey"]
            self.sslpassword = params["sslpassword"]
            self.ipcPath = params["ipc_path"]
            self.applicationName = params["application_name"]
            self.searchPath = params["search_path"]
            self.role = params["role"]
            self.binaryTransfer = parseBool(params["binary_transfer"], default: true)
            self.compression = params["compression"] ?? "off"
            self.fetchSize = parseInt(params["fetch_size"], default: 0)
            self.connectClientFlags = parseInt(params["connect_client_flags"], default: 0)
            self.managerAuthToken = params["manager_auth_token"]
            self.managerUsername = params["manager_username"]
            self.managerDatabase = params["manager_database"]
            self.managerConnectionProfile = params["manager_connection_profile"] ?? "SBsql"
            self.managerClientIntent = params["manager_client_intent"] ?? "SBsql"
            self.managerClientFlags = parseInt(params["manager_client_flags"], default: 0)
            self.managerAuthFastPath = parseBool(params["manager_auth_fast_path"], default: true)
            self.authToken = params["auth_token"]
            self.authMethodId = params["auth_method_id"]
            self.authMethodPayload = params["auth_method_payload"]
            self.authPayloadJson = params["auth_payload_json"]
            self.authPayloadB64 = params["auth_payload_b64"]
            self.authProviderProfile = params["auth_provider_profile"]
            self.authRequiredMethods = params["auth_required_methods"]
            self.authForbiddenMethods = params["auth_forbidden_methods"]
            self.authRequireChannelBinding = parseBool(params["auth_require_channel_binding"], default: false)
            self.workloadIdentityToken = params["workload_identity_token"]
            self.proxyPrincipalAssertion = params["proxy_principal_assertion"]
            self.dormantId = params["dormant_id"]
            self.dormantReattachToken = params["dormant_reattach_token"]
            self.keepaliveIntervalMs = parseInt(params["keepalive_interval_ms"], default: 120_000)
            self.keepaliveMaxIdleBeforeCheckMs = parseInt(params["keepalive_max_idle_before_check_ms"], default: 600_000)
            self.keepaliveValidationTimeoutMs = parseInt(params["keepalive_validation_timeout_ms"], default: 5_000)
            self.leakDetectionThresholdMs = parseInt(params["leak_detection_threshold_ms"], default: 30_000)
            self.leakDetectionCheckIntervalMs = parseInt(params["leak_detection_check_interval_ms"], default: 10_000)
            self.leakDetectionCaptureStackTrace = parseBool(params["leak_detection_capture_stack_trace"], default: false)
        } else {
            var rawParams: [String: String] = [:]
            for part in dsn.split(separator: " ") {
                let pieces = part.split(separator: "=", maxSplits: 1)
                if pieces.count == 2 {
                    rawParams[String(pieces[0])] = String(pieces[1])
                }
            }
            let params = normalizeConnectionParams(rawParams)
            self.host = params["host"] ?? "localhost"
            self.port = Int(params["port"] ?? "3092") ?? 3092
            self.protocolName = params["protocol"] ?? params["parser"] ?? params["dialect"] ?? "native"
            self.frontDoorMode = params["front_door_mode"] ?? "direct"
            self.database = params["database"] ?? params["dbname"] ?? ""
            self.user = params["user"] ?? params["username"] ?? ""
            self.password = params["password"]
            self.sslmode = params["sslmode"] ?? "require"
            self.sslrootcert = params["sslrootcert"]
            self.sslcert = params["sslcert"]
            self.sslkey = params["sslkey"]
            self.sslpassword = params["sslpassword"]
            self.ipcPath = params["ipc_path"]
            self.applicationName = params["application_name"]
            self.searchPath = params["search_path"]
            self.role = params["role"]
            self.binaryTransfer = parseBool(params["binary_transfer"], default: true)
            self.compression = params["compression"] ?? "off"
            self.fetchSize = parseInt(params["fetch_size"], default: 0)
            self.connectClientFlags = parseInt(params["connect_client_flags"], default: 0)
            self.managerAuthToken = params["manager_auth_token"]
            self.managerUsername = params["manager_username"]
            self.managerDatabase = params["manager_database"]
            self.managerConnectionProfile = params["manager_connection_profile"] ?? "SBsql"
            self.managerClientIntent = params["manager_client_intent"] ?? "SBsql"
            self.managerClientFlags = parseInt(params["manager_client_flags"], default: 0)
            self.managerAuthFastPath = parseBool(params["manager_auth_fast_path"], default: true)
            self.authToken = params["auth_token"]
            self.authMethodId = params["auth_method_id"]
            self.authMethodPayload = params["auth_method_payload"]
            self.authPayloadJson = params["auth_payload_json"]
            self.authPayloadB64 = params["auth_payload_b64"]
            self.authProviderProfile = params["auth_provider_profile"]
            self.authRequiredMethods = params["auth_required_methods"]
            self.authForbiddenMethods = params["auth_forbidden_methods"]
            self.authRequireChannelBinding = parseBool(params["auth_require_channel_binding"], default: false)
            self.workloadIdentityToken = params["workload_identity_token"]
            self.proxyPrincipalAssertion = params["proxy_principal_assertion"]
            self.dormantId = params["dormant_id"]
            self.dormantReattachToken = params["dormant_reattach_token"]
            self.keepaliveIntervalMs = parseInt(params["keepalive_interval_ms"], default: 120_000)
            self.keepaliveMaxIdleBeforeCheckMs = parseInt(params["keepalive_max_idle_before_check_ms"], default: 600_000)
            self.keepaliveValidationTimeoutMs = parseInt(params["keepalive_validation_timeout_ms"], default: 5_000)
            self.leakDetectionThresholdMs = parseInt(params["leak_detection_threshold_ms"], default: 30_000)
            self.leakDetectionCheckIntervalMs = parseInt(params["leak_detection_check_interval_ms"], default: 10_000)
            self.leakDetectionCaptureStackTrace = parseBool(params["leak_detection_capture_stack_trace"], default: false)
        }
    }
}
