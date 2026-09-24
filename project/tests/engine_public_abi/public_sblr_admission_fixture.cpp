// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/engine.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// Only serialized bytes cross between this public-ABI test and its codec
// fixture producer. Never redeclare private C++ types: their layouts are not ABI.
namespace scratchbird::test {
std::string CanonicalOperationBytes();
bool ValidateCanonicalOperationBytes(std::string_view bytes);
}

namespace {

constexpr const char* kDatabasePath =
    "/tmp/sb_engine_public_sblr_admission.sbdb";

sb_engine_uuid_t TestUuid(unsigned char tail) {
  sb_engine_uuid_t uuid{};
  uuid.bytes[0] = 0x01;
  uuid.bytes[6] = 0x70;
  uuid.bytes[8] = 0x80;
  uuid.bytes[15] = tail;
  return uuid;
}

struct Harness {
  sb_engine_handle_t engine = nullptr;
  sb_engine_session_t session = nullptr;
  sb_engine_session_params_v1_t session_params{};

  bool Open() {
    sb_engine_open_params_v1_t open_params{};
    open_params.struct_size = sizeof(open_params);
    open_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    open_params.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
    open_params.database_path_utf8 = kDatabasePath;
    open_params.database_path_size = std::strlen(kDatabasePath);
    if (sb_engine_open(&open_params, &engine, nullptr) != SB_ENGINE_STATUS_OK ||
        engine == nullptr) {
      return false;
    }
    session_params.struct_size = sizeof(session_params);
    session_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    session_params.effective_user_uuid = TestUuid(1);
    session_params.session_uuid = TestUuid(2);
    session_params.trust_mode = SB_ENGINE_TRUST_EMBEDDED_TRUSTED;
    session_params.default_language_utf8 = "en";
    session_params.default_language_size = 2;
    return sb_engine_session_begin(engine, &session_params, &session, nullptr) ==
               SB_ENGINE_STATUS_OK &&
           session != nullptr;
  }

  ~Harness() {
    if (session != nullptr) {
      sb_engine_session_end_params_v1_t end_params{};
      end_params.struct_size = sizeof(end_params);
      end_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
      end_params.rollback_active_transactions = 1;
      end_params.cancel_open_results = 1;
      (void)sb_engine_session_end(session, &end_params, nullptr);
    }
    if (engine != nullptr) (void)sb_engine_close(engine, nullptr);
  }
};

bool HasDiagnostic(sb_engine_result_t result, std::string_view code) {
  sb_engine_diagnostic_set_view_t diagnostics{};
  if (result == nullptr ||
      sb_engine_result_diagnostics(result, &diagnostics) !=
          SB_ENGINE_STATUS_OK) {
    return false;
  }
  for (std::uint64_t i = 0; i < diagnostics.diagnostic_count; ++i) {
    const auto& diagnostic = diagnostics.diagnostics[i];
    if (diagnostic.symbolic_code.data != nullptr &&
        std::string_view(
            diagnostic.symbolic_code.data,
            static_cast<std::size_t>(diagnostic.symbolic_code.size_bytes)) ==
            code) {
      return diagnostic.reserved0 == 0 && diagnostic.reserved1 == 0;
    }
  }
  return false;
}

bool PayloadContains(sb_engine_result_t result, std::string_view expected) {
  sb_engine_string_view_t payload{};
  return result != nullptr &&
         sb_engine_result_payload(result, &payload) == SB_ENGINE_STATUS_OK &&
         payload.data != nullptr &&
         std::string_view(payload.data,
                          static_cast<std::size_t>(payload.size_bytes))
                 .find(expected) != std::string_view::npos;
}

sb_engine_status_t Dispatch(Harness& harness,
                            const std::vector<std::uint8_t>& envelope,
                            sb_engine_result_t* result,
                            std::uint64_t reserved0 = 0,
                            std::uint64_t reserved1 = 0) {
  sb_engine_request_context_v1_t context{};
  context.struct_size = sizeof(context);
  context.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  context.effective_user_uuid = harness.session_params.effective_user_uuid;
  context.session_uuid = harness.session_params.session_uuid;
  context.trust_mode = SB_ENGINE_TRUST_EMBEDDED_TRUSTED;
  context.rights_set_ref = 1;
  context.capability_set_ref = 1;

  sb_engine_sblr_dispatch_params_v1_t params{};
  params.struct_size = sizeof(params);
  params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  params.envelope_bytes = envelope.empty() ? nullptr : envelope.data();
  params.envelope_size_bytes = envelope.size();
  params.reserved0 = reserved0;
  params.reserved1 = reserved1;
  return sb_engine_dispatch_sblr(
      harness.session, nullptr, &context, &params, result);
}


}  // namespace

int main() {
  Harness harness;
  if (!harness.Open()) return 1;

  const std::string canonical = scratchbird::test::CanonicalOperationBytes();
  if (canonical.size() < 4 || canonical[0] != 'S' || canonical[1] != 'B' ||
      canonical[2] != 'O' || canonical[3] != 'P') {
    return 2;
  }
  if (!scratchbird::test::ValidateCanonicalOperationBytes(canonical)) return 3;

  const std::vector<std::uint8_t> nonempty(canonical.begin(), canonical.end());
  sb_engine_result_t result = nullptr;
  if (Dispatch(harness, nonempty, &result) != SB_ENGINE_STATUS_UNSUPPORTED ||
      !HasDiagnostic(result, "SBLR.ENVELOPE.FIELD_MISSING")) {
    if (result != nullptr) (void)sb_engine_result_release(result);
    return 5;
  }
  (void)sb_engine_result_release(result);

  result = nullptr;
  if (Dispatch(harness, nonempty, &result, 1, 0) !=
          SB_ENGINE_STATUS_INVALID_ARGUMENT ||
      !HasDiagnostic(result, "ENGINE.ABI.RESERVED_FIELD_INVALID")) {
    if (result != nullptr) (void)sb_engine_result_release(result);
    return 6;
  }
  (void)sb_engine_result_release(result);

  result = nullptr;
  if (Dispatch(harness, nonempty, &result, 0, 1) !=
          SB_ENGINE_STATUS_INVALID_ARGUMENT ||
      !HasDiagnostic(result, "ENGINE.ABI.RESERVED_FIELD_INVALID")) {
    if (result != nullptr) (void)sb_engine_result_release(result);
    return 7;
  }
  (void)sb_engine_result_release(result);

  result = nullptr;
  if (Dispatch(harness, {}, &result) != SB_ENGINE_STATUS_OK ||
      result == nullptr ||
      !PayloadContains(result, "empty envelope treated as capability probe")) {
    if (result != nullptr) (void)sb_engine_result_release(result);
    return 8;
  }
  sb_engine_result_class_t result_class = SB_ENGINE_RESULT_NONE;
  if (sb_engine_result_class(result, &result_class) != SB_ENGINE_STATUS_OK ||
      result_class != SB_ENGINE_RESULT_CAPABILITY_REPORT) {
    (void)sb_engine_result_release(result);
    return 9;
  }
  (void)sb_engine_result_release(result);
  return 0;
}
