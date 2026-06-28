// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import { ClientConfig } from "./types";

function normalizeNativeProtocol(value?: string): string {
  const normalized = (value ?? "").trim().toLowerCase();
  if (
    normalized === "" ||
    normalized === "native" ||
    normalized === "scratchbird" ||
    normalized === "scratchbird-native" ||
    normalized === "scratchbird_native"
  ) {
    return "native";
  }
  throw new Error("Only protocol=native is supported; connect to the native parser listener/port.");
}

function normalizeFrontDoorMode(value?: string): "direct" | "manager_proxy" {
  const normalized = (value ?? "").trim().toLowerCase();
  if (normalized === "" || normalized === "direct") {
    return "direct";
  }
  if (normalized === "manager_proxy" || normalized === "manager-proxy" || normalized === "managed") {
    return "manager_proxy";
  }
  throw new Error("front_door_mode must be direct or manager_proxy.");
}

export { normalizeNativeProtocol, normalizeFrontDoorMode };

export function parseDsn(dsn?: string): Partial<ClientConfig> {
  if (!dsn) {
    return {};
  }
  if (dsn.includes("://")) {
    return parseUri(dsn);
  }
  return parseKv(dsn);
}

function parseUri(dsn: string): Partial<ClientConfig> {
  const url = new URL(dsn);
  if (url.protocol !== "scratchbird:") {
    throw new Error(`Unsupported DSN scheme: ${url.protocol}`);
  }
  const params: Partial<ClientConfig> = {};
  if (url.hostname) params.host = url.hostname;
  if (url.port) params.port = Number(url.port);
  if (url.username) params.user = decodeURIComponent(url.username);
  if (url.password) params.password = decodeURIComponent(url.password);
  if (url.pathname && url.pathname !== "/") {
    params.database = url.pathname.replace(/^\//, "");
  }
  url.searchParams.forEach((value, key) => {
    setConfigParam(params, key, value);
  });
  return params;
}

function parseKv(dsn: string): Partial<ClientConfig> {
  const params: Partial<ClientConfig> = {};
  const tokens = dsn.split(/\s+/);
  for (const token of tokens) {
    const idx = token.indexOf("=");
    if (idx <= 0) continue;
    const key = token.slice(0, idx).trim();
    const value = token.slice(idx + 1).trim();
    setConfigParam(params, key, value);
  }
  return params;
}

function setConfigParam(config: Partial<ClientConfig>, key: string, value: string) {
  switch (key.toLowerCase()) {
    case "host":
      config.host = value;
      break;
    case "port":
      config.port = Number(value);
      break;
    case "front_door_mode":
    case "frontdoormode":
    case "connection_mode":
    case "ingress_mode":
      config.frontDoorMode = normalizeFrontDoorMode(value);
      break;
    case "transport_mode":
    case "transportmode":
    case "transport":
      config.transportMode = value;
      break;
    case "ipc_method":
    case "ipcmethod":
      config.ipcMethod = value;
      break;
    case "ipc_path":
    case "ipcpath":
    case "socket_path":
    case "pipe_name":
      config.ipcPath = value;
      break;
    case "database":
    case "dbname":
      config.database = value;
      break;
    case "user":
      config.user = value;
      break;
    case "password":
      config.password = value;
      break;
    case "schema":
    case "search_path":
    case "searchpath":
    case "currentschema":
      config.schema = value;
      break;
    case "metadataexpandschemaparents":
    case "metadata_expand_schema_parents":
    case "expandschemaparents":
    case "expand_schema_parents":
    case "dbeaverexpandschemaparents":
    case "dbeaver_expand_schema_parents":
      config.metadataExpandSchemaParents =
        value.toLowerCase() === "true" ||
        value === "1" ||
        value.toLowerCase() === "yes" ||
        value.toLowerCase() === "on";
      break;
    case "protocol":
    case "parser":
    case "dialect":
      config.protocol = normalizeNativeProtocol(value);
      break;
    case "sslmode":
      config.sslmode = value;
      break;
    case "sslrootcert":
      config.sslrootcert = value;
      break;
    case "sslcert":
      config.sslcert = value;
      break;
    case "sslkey":
      config.sslkey = value;
      break;
    case "sslpassword":
      config.sslpassword = value;
      break;
    case "connect_timeout":
    case "connecttimeout":
      config.connectTimeoutMs = Number(value) * 1000;
      break;
    case "socket_timeout":
    case "sockettimeout":
      config.socketTimeoutMs = Number(value) * 1000;
      break;
    case "application_name":
    case "applicationname":
      config.applicationName = value;
      break;
    case "role":
      config.role = value;
      break;
    case "binary_transfer":
    case "binarytransfer":
      config.binaryTransfer = value === "true" || value === "1";
      break;
    case "compression":
      config.compression = value === "zstd" ? "zstd" : "off";
      break;
    case "manager_auth_token":
    case "mcp_auth_token":
      config.managerAuthToken = value;
      break;
    case "manager_username":
    case "mcp_username":
      config.managerUsername = value;
      break;
    case "manager_database":
    case "mcp_database":
      config.managerDatabase = value;
      break;
    case "manager_connection_profile":
    case "mcp_connection_profile":
      config.managerConnectionProfile = value;
      break;
    case "manager_client_intent":
    case "mcp_client_intent":
      config.managerClientIntent = value;
      break;
    case "manager_client_flags":
    case "mcp_client_flags":
      config.managerClientFlags = Number(value) || 0;
      break;
    case "manager_auth_fast_path":
    case "mcp_auth_fast_path":
      config.managerAuthFastPath =
        value.toLowerCase() === "true" ||
        value === "1" ||
        value.toLowerCase() === "yes" ||
        value.toLowerCase() === "on";
      break;
    case "client_flags":
    case "connect_client_flags":
      config.connectClientFlags = Number(value) || 0;
      break;
    case "auth_token":
    case "authtoken":
    case "bearer_token":
    case "bearertoken":
    case "token":
      config.authToken = value;
      break;
    case "auth_method_id":
    case "authmethodid":
      config.authMethodId = value.trim();
      break;
    case "auth_method_payload":
    case "authmethodpayload":
      config.authMethodPayload = value;
      break;
    case "auth_payload_json":
    case "authpayloadjson":
      config.authPayloadJson = value;
      break;
    case "auth_payload_b64":
    case "authpayloadb64":
      config.authPayloadB64 = value;
      break;
    case "auth_provider_profile":
    case "authproviderprofile":
      config.authProviderProfile = value.trim();
      break;
    case "auth_required_methods":
    case "authrequiredmethods":
      config.authRequiredMethods = value.trim();
      break;
    case "auth_forbidden_methods":
    case "authforbiddenmethods":
      config.authForbiddenMethods = value.trim();
      break;
    case "auth_require_channel_binding":
    case "authrequirechannelbinding":
      config.authRequireChannelBinding =
        value.toLowerCase() === "true" ||
        value === "1" ||
        value.toLowerCase() === "yes" ||
        value.toLowerCase() === "on";
      break;
    case "workload_identity_token":
    case "workloadidentitytoken":
      config.workloadIdentityToken = value;
      break;
    case "proxy_principal_assertion":
    case "proxyprincipalassertion":
    case "proxy_assertion":
      config.proxyPrincipalAssertion = value;
      break;
    case "dormant_id":
    case "dormantid":
      config.dormantId = value;
      break;
    case "dormant_reattach_token":
    case "dormantreattachtoken":
      config.dormantReattachToken = value;
      break;
    default:
      break;
  }
}
