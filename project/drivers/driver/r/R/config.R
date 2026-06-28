# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

sb_config <- function(dsn = "") {
  cfg <- list(
    host = "localhost",
    port = 3092L,
    transport = "inet",
    ipc_path = "",
    protocol = "native",
    front_door_mode = "direct",
    database = "",
    user = "",
    password = "",
    schema = "",
    role = "",
    sslmode = "require",
    sslrootcert = NULL,
    sslcert = NULL,
    sslkey = NULL,
    sslpassword = NULL,
    connect_timeout_ms = 30000L,
    socket_timeout_ms = 0L,
    application_name = "scratchbird_r",
    fetch_size = 0L,
    binary_transfer = TRUE,
    compression = "off",
    manager_auth_token = "",
    manager_username = "",
    manager_database = "",
    manager_connection_profile = "SBsql",
    manager_client_intent = "SBsql",
    manager_client_flags = 0L,
    manager_auth_fast_path = TRUE,
    connect_client_flags = 0L,
    auth_token = "",
    auth_method_id = "",
    auth_method_payload = "",
    auth_payload_json = "",
    auth_payload_b64 = "",
    auth_provider_profile = "",
    auth_required_methods = "",
    auth_forbidden_methods = "",
    auth_require_channel_binding = FALSE,
    workload_identity_token = "",
    proxy_principal_assertion = "",
    dormant_id = "",
    dormant_reattach_token = "",
    extra = list()
  )
  if (is.null(dsn) || trimws(dsn) == "") {
    return(cfg)
  }
  if (grepl("://", dsn)) {
    cfg <- parse_uri_dsn(cfg, dsn)
  } else {
    cfg <- parse_kv_dsn(cfg, dsn)
  }
  cfg
}

normalize_native_protocol <- function(value) {
  normalized <- tolower(trimws(as.character(value)))
  if (normalized %in% c("", "native", "scratchbird", "scratchbird-native", "scratchbird_native")) {
    return("native")
  }
  stop("Only protocol=native is supported; connect to the native parser listener/port.")
}

normalize_front_door_mode <- function(value) {
  normalized <- tolower(trimws(as.character(value)))
  if (normalized %in% c("", "direct")) {
    return("direct")
  }
  if (normalized %in% c("manager_proxy", "manager-proxy", "managed")) {
    return("manager_proxy")
  }
  stop("front_door_mode must be direct or manager_proxy.")
}

parse_uri_dsn <- function(cfg, dsn) {
  if (!startsWith(dsn, "scratchbird://")) stop("Unsupported DSN scheme")
  trimmed <- sub("^scratchbird://", "", dsn)
  pieces <- strsplit(trimmed, "\\?", fixed = FALSE)[[1]]
  authority_and_path <- pieces[1]
  query <- if (length(pieces) > 1) pieces[2] else ""
  path_split <- strsplit(authority_and_path, "/", fixed = TRUE)[[1]]
  authority <- path_split[1]
  if (length(path_split) > 1) {
    cfg$database <- utils::URLdecode(path_split[2])
  }
  user_host <- strsplit(authority, "@", fixed = TRUE)[[1]]
  host_part <- authority
  if (length(user_host) == 2) {
    userinfo <- user_host[1]
    host_part <- user_host[2]
    creds <- strsplit(userinfo, ":", fixed = TRUE)[[1]]
    cfg$user <- utils::URLdecode(creds[1])
    if (length(creds) > 1) cfg$password <- utils::URLdecode(creds[2])
  }
  host_port <- strsplit(host_part, ":", fixed = TRUE)[[1]]
  cfg$host <- utils::URLdecode(host_port[1])
  if (length(host_port) > 1) cfg$port <- as.integer(host_port[2])
  if (query != "") {
    params <- strsplit(query, "&", fixed = TRUE)[[1]]
    for (param in params) {
      if (param == "") next
      kv <- strsplit(param, "=", fixed = TRUE)[[1]]
      key <- utils::URLdecode(kv[1])
      value <- if (length(kv) > 1) utils::URLdecode(kv[2]) else ""
      cfg <- apply_param(cfg, key, value)
    }
  }
  cfg
}

parse_kv_dsn <- function(cfg, dsn) {
  sep <- if (grepl(";", dsn, fixed = TRUE)) ";" else " "
  tokens <- strsplit(dsn, sep, fixed = TRUE)[[1]]
  for (token in tokens) {
    token <- trimws(token)
    if (token == "") next
    kv <- strsplit(token, "=", fixed = TRUE)[[1]]
    if (length(kv) < 2) next
    key <- trimws(kv[1])
    value <- trimws(gsub("^\"|\"$", "", kv[2]))
    cfg <- apply_param(cfg, key, value)
  }
  cfg
}

apply_param <- function(cfg, key, value) {
  key <- tolower(key)
  if (key %in% c("host", "server", "data source", "datasource")) {
    cfg$host <- value
  } else if (key == "port") {
    cfg$port <- as.integer(value)
  } else if (key %in% c("transport", "transport_mode", "transportmode")) {
    cfg$transport <- normalize_transport(value)
  } else if (key == "route") {
    cfg$transport <- transport_for_route(value)
  } else if (key %in% c("ipc_path", "ipcpath", "ipc-path")) {
    cfg$ipc_path <- value
  } else if (key %in% c("database", "dbname", "initial catalog")) {
    cfg$database <- value
  } else if (key %in% c("protocol", "parser", "dialect")) {
    cfg$protocol <- normalize_native_protocol(value)
  } else if (key %in% c("front_door_mode", "frontdoormode", "connection_mode", "ingress_mode")) {
    cfg$front_door_mode <- normalize_front_door_mode(value)
  } else if (key %in% c("user", "username", "user id", "uid")) {
    cfg$user <- value
  } else if (key %in% c("password", "pwd")) {
    cfg$password <- value
  } else if (key %in% c("schema", "search_path", "searchpath", "currentschema")) {
    cfg$schema <- value
  } else if (key == "role") {
    cfg$role <- value
  } else if (key %in% c("sslmode", "ssl mode")) {
    cfg$sslmode <- value
  } else if (key == "sslrootcert") {
    cfg$sslrootcert <- value
  } else if (key == "sslcert") {
    cfg$sslcert <- value
  } else if (key == "sslkey") {
    cfg$sslkey <- value
  } else if (key == "sslpassword") {
    cfg$sslpassword <- value
  } else if (key %in% c("connect_timeout", "connecttimeout", "timeout")) {
    cfg$connect_timeout_ms <- as.integer(value) * 1000L
  } else if (key %in% c("socket_timeout", "sockettimeout")) {
    cfg$socket_timeout_ms <- as.integer(value) * 1000L
  } else if (key %in% c("application_name", "applicationname")) {
    cfg$application_name <- value
  } else if (key %in% c("fetch_size", "fetchsize", "defaultrowfetchsize")) {
    cfg$fetch_size <- as.integer(value)
  } else if (key %in% c("binary_transfer", "binarytransfer")) {
    cfg$binary_transfer <- value %in% c("1", "true", "TRUE")
  } else if (key == "compression") {
    cfg$compression <- if (tolower(value) == "zstd") "zstd" else "off"
  } else if (key %in% c("manager_auth_token", "mcp_auth_token")) {
    cfg$manager_auth_token <- value
  } else if (key %in% c("manager_username", "mcp_username")) {
    cfg$manager_username <- value
  } else if (key %in% c("manager_database", "mcp_database")) {
    cfg$manager_database <- value
  } else if (key %in% c("manager_connection_profile", "mcp_connection_profile")) {
    cfg$manager_connection_profile <- value
  } else if (key %in% c("manager_client_intent", "mcp_client_intent")) {
    cfg$manager_client_intent <- value
  } else if (key %in% c("manager_client_flags", "mcp_client_flags")) {
    parsed <- suppressWarnings(as.integer(value))
    if (!is.na(parsed)) cfg$manager_client_flags <- parsed
  } else if (key %in% c("manager_auth_fast_path", "mcp_auth_fast_path")) {
    cfg$manager_auth_fast_path <- tolower(value) %in% c("1", "true", "yes", "on")
  } else if (key %in% c("connect_client_flags", "connectclientflags")) {
    parsed <- suppressWarnings(as.integer(value))
    if (!is.na(parsed)) cfg$connect_client_flags <- parsed
  } else if (key %in% c("auth_token", "authtoken", "bearertoken", "token")) {
    cfg$auth_token <- value
  } else if (key %in% c("auth_method_id", "authmethodid")) {
    cfg$auth_method_id <- value
  } else if (key %in% c("auth_method_payload", "authmethodpayload")) {
    cfg$auth_method_payload <- value
  } else if (key %in% c("auth_payload_json", "authpayloadjson")) {
    cfg$auth_payload_json <- value
  } else if (key %in% c("auth_payload_b64", "authpayloadb64")) {
    cfg$auth_payload_b64 <- value
  } else if (key %in% c("auth_provider_profile", "authproviderprofile")) {
    cfg$auth_provider_profile <- value
  } else if (key %in% c("auth_required_methods", "authrequiredmethods")) {
    cfg$auth_required_methods <- value
  } else if (key %in% c("auth_forbidden_methods", "authforbiddenmethods")) {
    cfg$auth_forbidden_methods <- value
  } else if (key %in% c("auth_require_channel_binding", "authrequirechannelbinding")) {
    cfg$auth_require_channel_binding <- tolower(value) %in% c("1", "true", "yes", "on")
  } else if (key %in% c("workload_identity_token", "workloadidentitytoken")) {
    cfg$workload_identity_token <- value
  } else if (key %in% c("proxy_principal_assertion", "proxyprincipalassertion")) {
    cfg$proxy_principal_assertion <- value
  } else if (key %in% c("dormant_id", "dormantid")) {
    cfg$dormant_id <- value
  } else if (key %in% c("dormant_reattach_token", "dormantreattachtoken")) {
    cfg$dormant_reattach_token <- value
  } else {
    cfg$extra[[key]] <- value
  }
  cfg
}

normalize_transport <- function(value) {
  normalized <- tolower(trimws(as.character(value)))
  if (normalized %in% c("", "inet", "tcp", "network")) {
    return("inet")
  }
  if (normalized %in% c("ipc", "ipc_local", "local_ipc", "unix", "unix_socket", "uds")) {
    return("ipc")
  }
  if (identical(normalized, "embedded")) {
    return("embedded")
  }
  stop("transport must be inet, ipc, or embedded.")
}

transport_for_route <- function(value) {
  normalized <- tolower(trimws(as.character(value)))
  if (identical(normalized, "ipc_local")) return("ipc")
  if (identical(normalized, "embedded")) return("embedded")
  "inet"
}
