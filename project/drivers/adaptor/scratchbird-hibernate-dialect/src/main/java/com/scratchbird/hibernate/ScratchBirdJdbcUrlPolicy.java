// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.hibernate;

import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;

public final class ScratchBirdJdbcUrlPolicy {
  private static final Map<String, String> PARAM_ALIASES = Map.ofEntries(
      Map.entry("application_name", "application_name"),
      Map.entry("applicationname", "application_name"),
      Map.entry("currentschema", "search_path"),
      Map.entry("searchpath", "search_path"),
      Map.entry("search_path", "search_path"),
      Map.entry("ssl", "sslmode"),
      Map.entry("sslmode", "sslmode"),
      Map.entry("sslrootcert", "sslrootcert"),
      Map.entry("sslcert", "sslcert"),
      Map.entry("sslkey", "sslkey"),
      Map.entry("sslpassword", "sslpassword"),
      Map.entry("connecttimeout", "connectTimeout"),
      Map.entry("connect_timeout", "connectTimeout"),
      Map.entry("sockettimeout", "socketTimeout"),
      Map.entry("socket_timeout", "socketTimeout"),
      Map.entry("binarytransfer", "binaryTransfer"),
      Map.entry("binary_transfer", "binaryTransfer"),
      Map.entry("fetchsize", "fetch_size"),
      Map.entry("fetch_size", "fetch_size"),
      Map.entry("connectclientflags", "connect_client_flags"),
      Map.entry("connect_client_flags", "connect_client_flags"),
      Map.entry("frontdoormode", "front_door_mode"),
      Map.entry("front_door_mode", "front_door_mode"),
      Map.entry("managerauthtoken", "manager_auth_token"),
      Map.entry("manager_auth_token", "manager_auth_token"),
      Map.entry("managerusername", "manager_username"),
      Map.entry("manager_username", "manager_username"),
      Map.entry("managerdatabase", "manager_database"),
      Map.entry("manager_database", "manager_database"),
      Map.entry("managerconnectionprofile", "manager_connection_profile"),
      Map.entry("manager_connection_profile", "manager_connection_profile"),
      Map.entry("managerclientintent", "manager_client_intent"),
      Map.entry("manager_client_intent", "manager_client_intent"),
      Map.entry("managerclientflags", "manager_client_flags"),
      Map.entry("manager_client_flags", "manager_client_flags"),
      Map.entry("managerauthfastpath", "manager_auth_fast_path"),
      Map.entry("manager_auth_fast_path", "manager_auth_fast_path"),
      Map.entry("authtoken", "auth_token"),
      Map.entry("auth_token", "auth_token"),
      Map.entry("authmethodid", "auth_method_id"),
      Map.entry("auth_method_id", "auth_method_id"),
      Map.entry("authmethodpayload", "auth_method_payload"),
      Map.entry("auth_method_payload", "auth_method_payload"),
      Map.entry("authpayloadjson", "auth_payload_json"),
      Map.entry("auth_payload_json", "auth_payload_json"),
      Map.entry("authpayloadb64", "auth_payload_b64"),
      Map.entry("auth_payload_b64", "auth_payload_b64"),
      Map.entry("authproviderprofile", "auth_provider_profile"),
      Map.entry("auth_provider_profile", "auth_provider_profile"),
      Map.entry("authrequiredmethods", "auth_required_methods"),
      Map.entry("auth_required_methods", "auth_required_methods"),
      Map.entry("authforbiddenmethods", "auth_forbidden_methods"),
      Map.entry("auth_forbidden_methods", "auth_forbidden_methods"),
      Map.entry("authrequirechannelbinding", "auth_require_channel_binding"),
      Map.entry("auth_require_channel_binding", "auth_require_channel_binding"),
      Map.entry("workloadidentitytoken", "workload_identity_token"),
      Map.entry("workload_identity_token", "workload_identity_token"),
      Map.entry("proxyprincipalassertion", "proxy_principal_assertion"),
      Map.entry("proxy_principal_assertion", "proxy_principal_assertion"),
      Map.entry("dormantid", "dormant_id"),
      Map.entry("dormant_id", "dormant_id"),
      Map.entry("dormantreattachtoken", "dormant_reattach_token"),
      Map.entry("dormant_reattach_token", "dormant_reattach_token"));

  private ScratchBirdJdbcUrlPolicy() {}

  public static Map<String, String> validateAndParse(String jdbcUrl) {
    if (jdbcUrl == null || jdbcUrl.isBlank()) {
      throw new IllegalArgumentException("JDBC URL is required");
    }
    if (!jdbcUrl.startsWith("jdbc:scratchbird:")) {
      throw new IllegalArgumentException("JDBC URL must start with jdbc:scratchbird:");
    }

    int queryIndex = jdbcUrl.indexOf('?');
    Map<String, String> params = new LinkedHashMap<>();
    if (queryIndex < 0 || queryIndex == jdbcUrl.length() - 1) {
      return params;
    }

    String query = jdbcUrl.substring(queryIndex + 1);
    for (String pair : query.split("&")) {
      if (pair.isBlank()) {
        continue;
      }
      String[] parts = pair.split("=", 2);
      String key = decode(parts[0]);
      String value = parts.length > 1 ? decode(parts[1]) : "";
      String normalizedKey = key.toLowerCase(Locale.ROOT);
      String canonicalKey = PARAM_ALIASES.getOrDefault(normalizedKey, key);
      String normalizedValue = normalizeSslAlias(normalizedKey, value).trim().toLowerCase(Locale.ROOT);
      String normalizedTextValue = normalizeSslAlias(normalizedKey, value);

      if (canonicalKey.equals("sslmode") && normalizedValue.equals("disable")) {
        throw new IllegalArgumentException("sslmode=disable is not supported");
      }
      if (canonicalKey.equals("binaryTransfer")
          && isFalse(normalizedValue)) {
        throw new IllegalArgumentException("binaryTransfer=false is not supported");
      }
      if (canonicalKey.equals("compression") && normalizedValue.equals("zstd")) {
        throw new IllegalArgumentException("compression=zstd is not supported");
      }
      if (canonicalKey.equals("front_door_mode")
          && !(normalizedValue.equals("direct") || normalizedValue.equals("manager_proxy"))) {
        throw new IllegalArgumentException("front_door_mode must be direct or manager_proxy");
      }
      if (canonicalKey.equals("auth_method_id")
          && !normalizedTextValue.isBlank()
          && !normalizedTextValue.startsWith("scratchbird.auth.")) {
        throw new IllegalArgumentException("auth_method_id must start with scratchbird.auth.");
      }

      if (canonicalKey.equals("search_path")
          && params.containsKey("search_path")
          && !(normalizedKey.equals("currentschema") || normalizedKey.equals("current_schema"))) {
        continue;
      }
      params.put(canonicalKey, canonicalKey.equals("front_door_mode") ? normalizedValue : normalizedTextValue);
    }
    return params;
  }

  private static String decode(String value) {
    return URLDecoder.decode(value, StandardCharsets.UTF_8);
  }

  private static boolean isFalse(String value) {
    return value.equals("false") || value.equals("0") || value.equals("off") || value.equals("no");
  }

  private static String normalizeSslAlias(String normalizedKey, String value) {
    if (!normalizedKey.equals("ssl")) {
      return value;
    }
    String normalized = value.trim().toLowerCase(Locale.ROOT);
    if (normalized.equals("true") || normalized.equals("1") || normalized.equals("yes") || normalized.equals("on")) {
      return "require";
    }
    if (isFalse(normalized)) {
      return "disable";
    }
    return value;
  }
}
