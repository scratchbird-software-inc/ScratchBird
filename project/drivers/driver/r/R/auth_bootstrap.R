# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

sb_auth_method_name <- function(method) {
  switch(
    as.character(as.integer(method)),
    "0" = "OK",
    "1" = "PASSWORD",
    "2" = "MD5",
    "3" = "SCRAM_SHA_256",
    "4" = "SCRAM_SHA_512",
    "5" = "TOKEN",
    "6" = "PEER",
    "7" = "REATTACH",
    NULL
  )
}

sb_auth_plugin_id_for_method <- function(method, configured_method_id = "") {
  configured <- trimws(as.character(if (is.null(configured_method_id)) "" else configured_method_id))
  if (nzchar(configured)) {
    return(configured)
  }

  switch(
    as.character(as.integer(method)),
    "0" = "scratchbird.auth.none",
    "1" = "scratchbird.auth.password_compat",
    "2" = "scratchbird.auth.md5_legacy",
    "3" = "scratchbird.auth.scram_sha_256",
    "4" = "scratchbird.auth.scram_sha_512",
    "5" = "scratchbird.auth.authkey_token",
    "6" = "scratchbird.auth.peer_uid",
    "7" = "scratchbird.auth.reattach",
    NULL
  )
}

sb_auth_method_executable_locally <- function(method) {
  as.integer(method) %in% c(SB_AUTH_PASSWORD, SB_AUTH_SCRAM_SHA256, SB_AUTH_SCRAM_SHA512, SB_AUTH_TOKEN)
}

sb_auth_method_broker_required <- function(method) {
  as.integer(method) == SB_AUTH_PEER
}

sb_additional_continuation_possible <- function(method) {
  as.integer(method) %in% c(SB_AUTH_SCRAM_SHA256, SB_AUTH_SCRAM_SHA512, SB_AUTH_TOKEN, SB_AUTH_PEER)
}

sb_describe_auth_method <- function(method, configured_method_id = "") {
  wire_method <- sb_auth_method_name(method)
  if (is.null(wire_method)) {
    return(NULL)
  }

  list(
    wire_method = wire_method,
    plugin_method_id = sb_auth_plugin_id_for_method(method, configured_method_id),
    executable_locally = sb_auth_method_executable_locally(method),
    broker_required = sb_auth_method_broker_required(method)
  )
}

sb_default_resolved_auth_context <- function(ingress_mode = "direct") {
  normalized <- trimws(as.character(if (is.null(ingress_mode)) "direct" else ingress_mode))
  if (!nzchar(normalized)) normalized <- "direct"
  list(
    ingress_mode = normalized,
    resolved_auth_method = NULL,
    resolved_auth_plugin_id = NULL,
    manager_authenticated = FALSE,
    attached = FALSE
  )
}

sb_resolve_token_auth_payload <- function(cfg) {
  candidate <- first_non_blank(
    cfg$auth_token,
    cfg$auth_method_payload,
    decode_auth_payload_b64(cfg$auth_payload_b64),
    cfg$auth_payload_json,
    cfg$workload_identity_token,
    cfg$proxy_principal_assertion
  )

  if (is.null(candidate)) {
    return(raw())
  }

  if (is.raw(candidate)) {
    return(candidate)
  }

  charToRaw(enc2utf8(as.character(candidate)))
}

sb_apply_auth_plugin_selection <- function(params, cfg) {
  append_non_blank <- function(target, name, value) {
    if (is.null(value)) {
      return(target)
    }
    text <- trimws(as.character(value))
    if (!nzchar(text)) {
      return(target)
    }
    target[[name]] <- text
    target
  }

  params <- append_non_blank(params, "auth_method_id", cfg$auth_method_id)
  params <- append_non_blank(params, "auth_method_payload", cfg$auth_method_payload)
  params <- append_non_blank(params, "auth_payload_json", cfg$auth_payload_json)
  params <- append_non_blank(params, "auth_payload_b64", cfg$auth_payload_b64)
  params <- append_non_blank(params, "auth_provider_profile", cfg$auth_provider_profile)
  params <- append_non_blank(params, "auth_required_methods", cfg$auth_required_methods)
  params <- append_non_blank(params, "auth_forbidden_methods", cfg$auth_forbidden_methods)
  if (isTRUE(cfg$auth_require_channel_binding)) {
    params[["auth_require_channel_binding"]] <- "true"
  }
  params <- append_non_blank(params, "workload_identity_token", cfg$workload_identity_token)
  params <- append_non_blank(params, "proxy_principal_assertion", cfg$proxy_principal_assertion)
  params
}

first_non_blank <- function(...) {
  values <- list(...)
  for (value in values) {
    if (is.null(value)) next
    if (is.raw(value) && length(value) > 0) return(value)
    text <- trimws(as.character(value))
    if (nzchar(text)) return(value)
  }
  NULL
}

decode_auth_payload_b64 <- function(value) {
  text <- trimws(as.character(if (is.null(value)) "" else value))
  if (!nzchar(text)) {
    return(NULL)
  }
  openssl::base64_decode(text)
}
