// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

"use strict";

const EXTRA_OPTION_ALIASES = new Map([
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

function parseBoolean(value) {
  if (value === undefined || value === null) {
    return undefined;
  }
  const normalized = String(value).trim().toLowerCase();
  if (["1", "true", "yes", "on"].includes(normalized)) {
    return true;
  }
  if (["0", "false", "no", "off"].includes(normalized)) {
    return false;
  }
  return undefined;
}

function canonicalExtraKey(rawKey) {
  const key = String(rawKey).trim();
  return EXTRA_OPTION_ALIASES.get(key.toLowerCase()) || key;
}

function normalizeSslAlias(rawKey, value) {
  if (String(rawKey).trim().toLowerCase() !== "ssl") {
    return value;
  }
  const boolValue = parseBoolean(value);
  if (boolValue === true) {
    return "require";
  }
  if (boolValue === false) {
    return "disable";
  }
  return value;
}

function parseUrl(urlString) {
  if (!urlString) {
    return {};
  }
  let parsed;
  try {
    parsed = new URL(urlString);
  } catch (err) {
    throw new Error(`Invalid ScratchBird URL: ${err.message}`);
  }

  const query = {};
  parsed.searchParams.forEach((value, key) => {
    query[key] = value;
  });

  return {
    protocol: parsed.protocol,
    host: parsed.hostname || undefined,
    port: parsed.port ? Number(parsed.port) : undefined,
    database: parsed.pathname ? parsed.pathname.replace(/^\//, "") : undefined,
    username: parsed.username || undefined,
    password: parsed.password || undefined,
    query,
  };
}

function enforceGuardrails(params) {
  const normalized = Object.create(null);
  for (const [rawKey, rawValue] of Object.entries(params || {})) {
    const rawKeyLower = String(rawKey).trim().toLowerCase();
    const key = canonicalExtraKey(rawKey);
    const value = normalizeSslAlias(rawKey, rawValue);
    const normalizedValue = String(value).trim().toLowerCase();

    if (key === "sslmode" && normalizedValue === "disable") {
      throw new Error("sslmode=disable is not supported");
    }
    if (key === "binary_transfer") {
      const boolValue = parseBoolean(normalizedValue);
      if (boolValue === false) {
        throw new Error("binaryTransfer=false is not supported");
      }
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

function normalizeTypeOrmOptions(options) {
  const input = options || {};
  const fromUrl = parseUrl(input.url);

  if (fromUrl.protocol && fromUrl.protocol !== "scratchbird:") {
    throw new Error("TypeORM URL protocol must be scratchbird://");
  }

  const mergedExtra = {
    ...(fromUrl.query || {}),
    ...(input.extra || {}),
  };

  const normalizedExtra = enforceGuardrails(mergedExtra);

  return {
    type: input.type || "scratchbird",
    host: input.host || fromUrl.host || "localhost",
    port: Number(input.port || fromUrl.port || 3092),
    database: input.database || fromUrl.database || "main",
    username: input.username || fromUrl.username,
    password: input.password || fromUrl.password,
    extra: normalizedExtra,
  };
}

module.exports = {
  normalizeTypeOrmOptions,
  enforceGuardrails,
  parseBoolean,
};
