// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

"use strict";

const QUERY_PARAM_ALIASES = new Map([
  ["application_name", "application_name"],
  ["applicationname", "application_name"],
  ["currentschema", "search_path"],
  ["searchpath", "search_path"],
  ["search_path", "search_path"],
  ["ssl", "sslmode"],
  ["sslmode", "sslmode"],
  ["sslrootcert", "sslrootcert"],
  ["sslcert", "sslcert"],
  ["sslkey", "sslkey"],
  ["sslpassword", "sslpassword"],
  ["connecttimeout", "connect_timeout"],
  ["connect_timeout", "connect_timeout"],
  ["sockettimeout", "socket_timeout"],
  ["socket_timeout", "socket_timeout"],
  ["binarytransfer", "binary_transfer"],
  ["binary_transfer", "binary_transfer"],
  ["fetchsize", "fetch_size"],
  ["fetch_size", "fetch_size"],
  ["connectclientflags", "connect_client_flags"],
  ["connect_client_flags", "connect_client_flags"],
  ["frontdoormode", "front_door_mode"],
  ["front_door_mode", "front_door_mode"],
  ["managerauthtoken", "manager_auth_token"],
  ["manager_auth_token", "manager_auth_token"],
  ["managerusername", "manager_username"],
  ["manager_username", "manager_username"],
  ["managerdatabase", "manager_database"],
  ["manager_database", "manager_database"],
  ["managerconnectionprofile", "manager_connection_profile"],
  ["manager_connection_profile", "manager_connection_profile"],
  ["managerclientintent", "manager_client_intent"],
  ["manager_client_intent", "manager_client_intent"],
  ["managerclientflags", "manager_client_flags"],
  ["manager_client_flags", "manager_client_flags"],
  ["managerauthfastpath", "manager_auth_fast_path"],
  ["manager_auth_fast_path", "manager_auth_fast_path"],
  ["authtoken", "auth_token"],
  ["auth_token", "auth_token"],
  ["authmethodid", "auth_method_id"],
  ["auth_method_id", "auth_method_id"],
  ["authmethodpayload", "auth_method_payload"],
  ["auth_method_payload", "auth_method_payload"],
  ["authpayloadjson", "auth_payload_json"],
  ["auth_payload_json", "auth_payload_json"],
  ["authpayloadb64", "auth_payload_b64"],
  ["auth_payload_b64", "auth_payload_b64"],
  ["authproviderprofile", "auth_provider_profile"],
  ["auth_provider_profile", "auth_provider_profile"],
  ["authrequiredmethods", "auth_required_methods"],
  ["auth_required_methods", "auth_required_methods"],
  ["authforbiddenmethods", "auth_forbidden_methods"],
  ["auth_forbidden_methods", "auth_forbidden_methods"],
  ["authrequirechannelbinding", "auth_require_channel_binding"],
  ["auth_require_channel_binding", "auth_require_channel_binding"],
  ["workloadidentitytoken", "workload_identity_token"],
  ["workload_identity_token", "workload_identity_token"],
  ["proxyprincipalassertion", "proxy_principal_assertion"],
  ["proxy_principal_assertion", "proxy_principal_assertion"],
  ["dormantid", "dormant_id"],
  ["dormant_id", "dormant_id"],
  ["dormantreattachtoken", "dormant_reattach_token"],
  ["dormant_reattach_token", "dormant_reattach_token"],
]);

function _isFalsey(value) {
  return ["false", "0", "no", "off"].includes(String(value).trim().toLowerCase());
}

function _isTruthy(value) {
  return ["true", "1", "yes", "on"].includes(String(value).trim().toLowerCase());
}

function _canonicalParamKey(rawKey) {
  const key = String(rawKey).trim();
  return QUERY_PARAM_ALIASES.get(key.toLowerCase()) || key;
}

function _normalizeSslAlias(rawKey, value) {
  if (String(rawKey).trim().toLowerCase() !== "ssl") {
    return value;
  }
  if (_isTruthy(value)) {
    return "require";
  }
  if (_isFalsey(value)) {
    return "disable";
  }
  return value;
}

function normalizeScratchbirdQueryParams(rawParams) {
  const normalized = {};
  for (const [rawKey, rawValue] of Object.entries(rawParams || {})) {
    const rawKeyLower = String(rawKey).trim().toLowerCase();
    const key = _canonicalParamKey(rawKey);
    const value = _normalizeSslAlias(rawKey, rawValue);
    const normalizedValue = String(value).trim().toLowerCase();

    if (key === "sslmode" && normalizedValue === "disable") {
      throw new Error("sslmode=disable is not supported");
    }
    if (key === "binary_transfer" && _isFalsey(value)) {
      throw new Error("binary_transfer=false is not supported");
    }
    if (key === "compression" && normalizedValue === "zstd") {
      throw new Error("compression=zstd is not supported");
    }
    if (key === "front_door_mode" && !["direct", "manager_proxy"].includes(normalizedValue)) {
      throw new Error("front_door_mode must be direct or manager_proxy");
    }
    if (key === "auth_method_id") {
      const valueText = String(value).trim();
      if (valueText && !valueText.startsWith("scratchbird.auth.")) {
        throw new Error("auth_method_id must start with scratchbird.auth.");
      }
      normalized[key] = valueText;
      continue;
    }
    if (
      key === "search_path" &&
      normalized.search_path !== undefined &&
      !["currentschema", "current_schema"].includes(rawKeyLower)
    ) {
      continue;
    }
    normalized[key] = key === "front_door_mode" ? normalizedValue : value;
  }
  return normalized;
}

function parseScratchbirdConnectionUrl(urlString) {
  if (!urlString || typeof urlString !== "string") {
    throw new Error("connection URL is required");
  }

  let parsed;
  try {
    parsed = new URL(urlString);
  } catch (error) {
    throw new Error(`invalid connection URL: ${error.message}`);
  }

  if (parsed.protocol !== "scratchbird:") {
    throw new Error("Prisma adapter requires scratchbird:// URLs");
  }

  const params = normalizeScratchbirdQueryParams(Object.fromEntries(parsed.searchParams.entries()));

  return {
    host: parsed.hostname || "localhost",
    port: parsed.port ? Number(parsed.port) : 3092,
    database: (parsed.pathname || "").replace(/^\//, ""),
    username: decodeURIComponent(parsed.username || ""),
    password: decodeURIComponent(parsed.password || ""),
    params,
  };
}

function validatePrismaSchemaText(schemaText) {
  if (!schemaText || typeof schemaText !== "string") {
    throw new Error("schema.prisma text is required");
  }

  if (!/\bdatasource\s+\w+\s*\{/.test(schemaText)) {
    throw new Error("schema.prisma is missing datasource block");
  }
  if (!/\bgenerator\s+\w+\s*\{/.test(schemaText)) {
    throw new Error("schema.prisma is missing generator block");
  }
  if (!/url\s*=\s*env\(\s*"DATABASE_URL"\s*\)/.test(schemaText)) {
    throw new Error("schema.prisma datasource must use env(\"DATABASE_URL\")");
  }

  return true;
}

module.exports = {
  parseScratchbirdConnectionUrl,
  normalizeScratchbirdQueryParams,
  validatePrismaSchemaText,
};
