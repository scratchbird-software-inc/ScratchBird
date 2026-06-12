// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/engine.h"

#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <string_view>

namespace {

sb_engine_uuid_t test_uuid(unsigned char tail) {
  sb_engine_uuid_t uuid{};
  uuid.bytes[0] = 0x01;
  uuid.bytes[6] = 0x70;
  uuid.bytes[15] = tail;
  return uuid;
}

bool registered_code(std::string_view code) {
  static const std::set<std::string> registered = {
      "ENGINE.ABI.STRUCT_SIZE_INVALID",
      "ENGINE.ABI.VERSION_UNSUPPORTED",
      "ENGINE.ABI.OUTPUT_POINTER_INVALID",
      "ENGINE.ABI.PARAMETER_NULL",
      "ENGINE.OPEN.PATH_INVALID",
      "ENGINE.OPEN.MODE_INVALID",
      "ENGINE.ABI.INVALID_HANDLE",
      "ENGINE.SESSION.LANGUAGE_INVALID",
      "ENGINE.SESSION.TRANSACTION_ACTIVE",
      "ENGINE.RESULT.STREAM_ACTIVE",
      "SECURITY.IDENTITY.MISSING",
      "SBLR.ENVELOPE.INVALID",
      "SBLR.ENVELOPE.CHECKSUM_INVALID",
      "SBLR.VERSION.UNSUPPORTED",
      "SBLR.OPCODE.UNKNOWN",
      "SBLR.OPCODE.REFERENCE_META_FORBIDDEN",
      "SBLR.DESCRIPTOR.INVALID",
      "SBLR.EXECUTION.ADMISSION_ONLY",
      "SBLR.CAPABILITY.FORBIDDEN",
      "ENGINE.METRIC.ROOT_INVALID",
  };
  return registered.find(std::string(code)) != registered.end();
}

bool check_diagnostics(sb_engine_result_t result) {
  sb_engine_diagnostic_set_view_t diagnostics{};
  if (sb_engine_result_diagnostics(result, &diagnostics) != SB_ENGINE_STATUS_OK) {
    return false;
  }
  if (diagnostics.struct_size != sizeof(diagnostics) || diagnostics.abi_version != SB_ENGINE_ABI_VERSION_PACKED ||
      diagnostics.diagnostic_count == 0 || diagnostics.diagnostics == nullptr) {
    return false;
  }
  for (std::uint64_t i = 0; i < diagnostics.diagnostic_count; ++i) {
    const auto& diagnostic = diagnostics.diagnostics[i];
    if (diagnostic.struct_size != sizeof(diagnostic) || diagnostic.abi_version != SB_ENGINE_ABI_VERSION_PACKED ||
        diagnostic.symbolic_code.data == nullptr || diagnostic.symbolic_code.size_bytes == 0 ||
        diagnostic.message_key.data == nullptr || diagnostic.message_key.size_bytes == 0 ||
        diagnostic.severity != SB_ENGINE_DIAGNOSTIC_ERROR) {
      return false;
    }
    if (!registered_code(std::string_view(diagnostic.symbolic_code.data,
                                          static_cast<std::size_t>(diagnostic.symbolic_code.size_bytes)))) {
      return false;
    }
  }
  return true;
}

bool has_diagnostic(sb_engine_result_t result, std::string_view code) {
  sb_engine_diagnostic_set_view_t diagnostics{};
  if (sb_engine_result_diagnostics(result, &diagnostics) != SB_ENGINE_STATUS_OK) {
    return false;
  }
  for (std::uint64_t i = 0; i < diagnostics.diagnostic_count; ++i) {
    const auto& diagnostic = diagnostics.diagnostics[i];
    if (diagnostic.symbolic_code.data != nullptr &&
        std::string_view(diagnostic.symbolic_code.data,
                         static_cast<std::size_t>(diagnostic.symbolic_code.size_bytes)) == code) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  sb_engine_open_params_v1_t bad_open{};
  bad_open.struct_size = sizeof(bad_open) - 1;
  bad_open.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  sb_engine_result_t result = nullptr;
  if (sb_engine_open(&bad_open, nullptr, &result) != SB_ENGINE_STATUS_INVALID_ARGUMENT || result == nullptr ||
      !check_diagnostics(result)) {
    return 1;
  }
  (void)sb_engine_result_release(result);

  sb_engine_open_params_v1_t bad_abi{};
  bad_abi.struct_size = sizeof(bad_abi);
  bad_abi.abi_version = SB_ENGINE_ABI_VERSION_PACKED + 1;
  sb_engine_handle_t rejected_engine = nullptr;
  result = nullptr;
  if (sb_engine_open(&bad_abi, &rejected_engine, &result) != SB_ENGINE_STATUS_INVALID_ARGUMENT ||
      rejected_engine != nullptr || result == nullptr || !check_diagnostics(result) ||
      !has_diagnostic(result, "ENGINE.ABI.VERSION_UNSUPPORTED")) {
    return 11;
  }
  (void)sb_engine_result_release(result);

  sb_engine_open_params_v1_t open_params{};
  open_params.struct_size = sizeof(open_params);
  open_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  open_params.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
  sb_engine_handle_t engine = nullptr;
  if (sb_engine_open(&open_params, &engine, nullptr) != SB_ENGINE_STATUS_OK || engine == nullptr) {
    return 2;
  }

  sb_engine_session_params_v1_t session_params{};
  session_params.struct_size = sizeof(session_params);
  session_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  session_params.effective_user_uuid = test_uuid(1);
  session_params.session_uuid = test_uuid(2);
  session_params.trust_mode = SB_ENGINE_TRUST_EMBEDDED_TRUSTED;
  session_params.default_language_utf8 = "en";
  session_params.default_language_size = 2;
  sb_engine_session_t session = nullptr;
  if (sb_engine_session_begin(engine, &session_params, &session, nullptr) != SB_ENGINE_STATUS_OK || session == nullptr) {
    return 3;
  }

  sb_engine_request_context_v1_t context{};
  context.struct_size = sizeof(context);
  context.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  context.trust_mode = SB_ENGINE_TRUST_EMBEDDED_TRUSTED;
  context.rights_set_ref = 1;
  context.capability_set_ref = 1;
  sb_engine_sblr_dispatch_params_v1_t dispatch{};
  dispatch.struct_size = sizeof(dispatch);
  dispatch.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  result = nullptr;
  if (sb_engine_dispatch_sblr(session, nullptr, &context, &dispatch, &result) != SB_ENGINE_STATUS_SECURITY_DENIED ||
      result == nullptr || !check_diagnostics(result)) {
    return 4;
  }
  (void)sb_engine_result_release(result);

  sb_engine_session_end_params_v1_t end_params{};
  end_params.struct_size = sizeof(end_params);
  end_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  end_params.rollback_active_transactions = 1;
  end_params.cancel_open_results = 1;
  if (sb_engine_session_end(session, &end_params, nullptr) != SB_ENGINE_STATUS_OK ||
      sb_engine_close(engine, nullptr) != SB_ENGINE_STATUS_OK) {
    return 5;
  }
  return 0;
}
