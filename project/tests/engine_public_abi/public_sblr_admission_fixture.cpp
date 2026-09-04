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

// This public-ABI fixture deliberately does not gain a private include path.
// Declare the frozen SBOP v1 codec value contract locally and link the same
// production encoder/decoder already carried by sb_engine.
namespace scratchbird::engine::sblr {

inline constexpr std::uint32_t kEngineSblrEnvelopeMajor = 1;
inline constexpr std::uint32_t kEngineSblrEnvelopeMinor = 0;

enum class SblrValueKind : std::uint16_t {
  uuid_ref = 1,
  descriptor_ref = 2,
  policy_ref = 3,
  principal_ref = 4,
  literal_typed = 5,
  parameter_slot = 6,
  result_target = 7,
  proof_token = 8,
  epoch_token = 9,
  profile_ref = 10,
  artifact_ref = 11,
  udr_ref = 12,
  list = 13,
  map = 14,
  null_value = 15,
  transaction_begin_options = 22,
};

using SblrTxnUuidV1 = std::array<std::uint8_t, 16>;
using SblrTxnShaV1 = std::array<std::uint8_t, 32>;

struct SblrTransactionBeginOptionsV1 {
  SblrTxnUuidV1 isolation_profile_uuid{};
  SblrTxnUuidV1 transaction_policy_snapshot_uuid{};
  std::uint64_t isolation_profile_generation = 0;
  std::uint64_t transaction_policy_generation = 0;
  std::uint64_t deadline_monotonic_ns = 0;
  std::uint8_t read_mode = 0;
  std::uint8_t authority_scope = 0;
  std::uint8_t wait_policy = 0;
  SblrTxnShaV1 options_sha256{};
};

std::vector<std::uint8_t> EncodeSblrTransactionBeginOptionsV1(
    SblrTransactionBeginOptionsV1* options);

struct SblrOperand {
  std::string type;
  std::string name;
  std::string value;
  std::uint32_t ordinal = 0;
  SblrValueKind value_kind = SblrValueKind::null_value;
  std::uint16_t value_flags = 0;
  std::vector<std::uint8_t> value_body;
};

struct SblrSourceSymbolArtifact {
  std::string symbol_kind;
  std::string stable_key;
  std::string resolved_uuid;
  std::string render_hint;
  std::string scope;
  std::string source_hash;
  bool authoritative = false;
  bool contains_sql_text = false;
};

struct SblrOperationRenderHint {
  std::string hint_kind;
  std::string stable_key;
  std::string value;
  bool authoritative = false;
  bool contains_sql_text = false;
};

struct SblrSourceArtifactMap {
  std::string policy_status = "absent";
  std::string source_identity;
  std::string source_hash;
  std::string artifact_format = "sblr.source_artifact_map.v1";
  bool render_metadata_only = true;
  bool contains_sql_text = false;
  bool raw_sql_text_authoritative = false;
  std::vector<SblrSourceSymbolArtifact> symbols;
  std::vector<SblrOperationRenderHint> operation_render_hints;
};

struct SblrOperationEnvelope {
  std::uint32_t envelope_major = kEngineSblrEnvelopeMajor;
  std::uint32_t envelope_minor = kEngineSblrEnvelopeMinor;
  std::uint16_t opcode_code = 0;
  std::uint16_t operation_version_major = 1;
  std::uint16_t operation_version_minor = 0;
  std::string operation_id;
  std::string opcode;
  std::string result_shape;
  std::string diagnostic_shape;
  std::string parser_package_uuid;
  std::uint32_t parser_package_version_major = 1;
  std::uint32_t parser_package_version_minor = 0;
  std::uint32_t parser_package_version_patch = 0;
  std::string registry_snapshot_uuid;
  std::string trace_key;
  std::vector<SblrOperand> operands;
  SblrSourceArtifactMap source_artifact_map;
  bool contains_sql_text = false;
  bool parser_resolved_names_to_uuids = false;
  bool requires_security_context = true;
  bool requires_transaction_context = false;
  bool requires_cluster_authority = false;
};

struct SblrEnvelopeDiagnostic {
  std::string code;
  std::string message;
  bool error = true;
};

struct SblrDecodeResult {
  bool ok = false;
  SblrOperationEnvelope envelope;
  std::vector<std::uint8_t> canonical_bytes;
  std::vector<SblrEnvelopeDiagnostic> diagnostics;
};

SblrOperationEnvelope MakeSblrEnvelope(std::string operation_id,
                                       std::string opcode,
                                       std::string trace_key = {});
SblrDecodeResult DecodeSblrEnvelope(std::string_view encoded);
std::string EncodeSblrEnvelope(const SblrOperationEnvelope& envelope);

}  // namespace scratchbird::engine::sblr

namespace {

constexpr const char* kDatabasePath =
    "/tmp/sb_engine_public_sblr_admission.sbdb";

sb_engine_uuid_t TestUuid(unsigned char tail) {
  sb_engine_uuid_t uuid{};
  uuid.bytes[0] = 0x01;
  uuid.bytes[6] = 0x70;
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

std::string CanonicalOperationBytes() {
  namespace sblr = scratchbird::engine::sblr;
  auto envelope = sblr::MakeSblrEnvelope(
      "engine.op.txn_begin", "SBLR_TXN_BEGIN",
      "public-abi-canonical-codec-structure");
  envelope.opcode_code = 0x0100u;
  envelope.result_shape = "transaction_handle";
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.parser_package_uuid =
      "019e05b1-f009-7000-8000-000000000020";
  envelope.registry_snapshot_uuid =
      "019e05b1-f009-7000-8000-000000000021";

  sblr::SblrTransactionBeginOptionsV1 options;
  options.isolation_profile_uuid[0] = 1;
  options.isolation_profile_generation = 1;
  options.transaction_policy_snapshot_uuid[0] = 2;
  options.transaction_policy_generation = 1;
  options.read_mode = 1;
  options.authority_scope = 1;
  options.wait_policy = 1;

  sblr::SblrOperand operand;
  operand.type = "transaction.begin_options";
  operand.name = "options";
  operand.ordinal = 1;
  operand.value_kind = sblr::SblrValueKind::transaction_begin_options;
  operand.value_body =
      sblr::EncodeSblrTransactionBeginOptionsV1(&options);
  envelope.operands.push_back(std::move(operand));
  return sblr::EncodeSblrEnvelope(envelope);
}

}  // namespace

int main() {
  namespace sblr = scratchbird::engine::sblr;

  Harness harness;
  if (!harness.Open()) return 1;

  const std::string canonical = CanonicalOperationBytes();
  if (canonical.size() < 4 || canonical[0] != 'S' || canonical[1] != 'B' ||
      canonical[2] != 'O' || canonical[3] != 'P') {
    return 2;
  }
  const auto decoded = sblr::DecodeSblrEnvelope(canonical);
  if (!decoded.ok ||
      decoded.envelope.operation_id != "engine.op.txn_begin" ||
      decoded.envelope.opcode != "SBLR_TXN_BEGIN" ||
      std::string(decoded.canonical_bytes.begin(),
                  decoded.canonical_bytes.end()) != canonical ||
      sblr::EncodeSblrEnvelope(decoded.envelope) != canonical) {
    return 3;
  }
  std::string corrupt = canonical;
  corrupt.back() ^= 0x01;
  if (sblr::DecodeSblrEnvelope(corrupt).ok) return 4;

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
