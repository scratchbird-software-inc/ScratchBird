// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/engine.h"
#include "scratchbird/engine/sblr/lowering.hpp"
#include "scratchbird/engine/sblr/raising.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

sb_engine_uuid_t test_uuid(unsigned char tail) {
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

  bool open() {
    sb_engine_open_params_v1_t open_params{};
    open_params.struct_size = sizeof(open_params);
    open_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    open_params.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
    if (sb_engine_open(&open_params, &engine, nullptr) != SB_ENGINE_STATUS_OK || engine == nullptr) {
      return false;
    }
    session_params.struct_size = sizeof(session_params);
    session_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    session_params.effective_user_uuid = test_uuid(1);
    session_params.session_uuid = test_uuid(2);
    session_params.trust_mode = SB_ENGINE_TRUST_EMBEDDED_TRUSTED;
    session_params.default_language_utf8 = "en";
    session_params.default_language_size = 2;
    return sb_engine_session_begin(engine, &session_params, &session, nullptr) == SB_ENGINE_STATUS_OK && session != nullptr;
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
    if (engine != nullptr) {
      (void)sb_engine_close(engine, nullptr);
    }
  }
};

bool has_diagnostic(sb_engine_result_t result, std::string_view code) {
  sb_engine_diagnostic_set_view_t diagnostics{};
  if (sb_engine_result_diagnostics(result, &diagnostics) != SB_ENGINE_STATUS_OK) {
    return false;
  }
  for (std::uint64_t i = 0; i < diagnostics.diagnostic_count; ++i) {
    const auto& diagnostic = diagnostics.diagnostics[i];
    if (diagnostic.symbolic_code.data != nullptr &&
        std::string_view(diagnostic.symbolic_code.data, static_cast<std::size_t>(diagnostic.symbolic_code.size_bytes)) == code) {
      return true;
    }
  }
  return false;
}

bool payload_contains(sb_engine_result_t result, std::string_view expected) {
  sb_engine_string_view_t view{};
  if (sb_engine_result_payload(result, &view) != SB_ENGINE_STATUS_OK || view.data == nullptr) {
    return false;
  }
  return std::string_view(view.data, static_cast<std::size_t>(view.size_bytes)).find(expected) != std::string_view::npos;
}

sb_engine_status_t dispatch(Harness& harness, const std::vector<std::uint8_t>& envelope, sb_engine_result_t* result) {
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
  params.envelope_bytes = envelope.data();
  params.envelope_size_bytes = envelope.size();
  return sb_engine_dispatch_sblr(harness.session, nullptr, &context, &params, result);
}

}  // namespace

int main() {
  Harness harness;
  if (!harness.open()) {
    return 1;
  }

  const std::uint8_t payload[] = {'p', 'l', 'a', 'n'};
  auto relational = scratchbird::engine::sblr::EnvelopeBuilder()
                        .operation(scratchbird::engine::SblrOperationFamily::relational_query, 1)
                        .descriptor(1, payload, sizeof(payload))
                        .append_bytes(payload, sizeof(payload))
                        .encode();
  auto decoded = scratchbird::engine::sblr::EnvelopeReader::decode(relational.data(), relational.size());
  if (decoded.status != scratchbird::engine::SblrCodecStatus::ok ||
      decoded.envelope.family != scratchbird::engine::SblrOperationFamily::relational_query) {
    return 2;
  }
  scratchbird::engine::sblr::EnvelopeReader reader(decoded.envelope);
  if (reader.behavior_status() != scratchbird::engine::SblrBehaviorStatus::admission_only || reader.descriptor_count() != 1) {
    return 3;
  }

  sb_engine_result_t result = nullptr;
  if (dispatch(harness, relational, &result) != SB_ENGINE_STATUS_UNSUPPORTED || result == nullptr ||
      !has_diagnostic(result, "SBLR.EXECUTION.ADMISSION_ONLY")) {
    return 4;
  }
  (void)sb_engine_result_release(result);

  const std::string show_version_operation =
      "operation_id=observability.show_version\n"
      "opcode=SBLR_OBSERVABILITY_SHOW_VERSION\n"
      "result_shape=engine.api.result.v1\n"
      "diagnostic_shape=engine.diagnostic.v1\n"
      "parser_package_uuid=019e05b1-f009-7000-8000-000000000020\n"
      "registry_snapshot_uuid=019e05b1-f009-7000-8000-000000000021\n"
      "trace_key=FSPE-009-public-abi\n"
      "contains_sql_text=false\n"
      "parser_resolved_names_to_uuids=true\n"
      "requires_security_context=true\n"
      "requires_transaction_context=false\n"
      "requires_cluster_authority=false\n";
  auto show_version = scratchbird::engine::sblr::EnvelopeBuilder()
                          .operation(scratchbird::engine::SblrOperationFamily::management_inspect, 1)
                          .append_bytes(reinterpret_cast<const std::uint8_t*>(show_version_operation.data()),
                                        show_version_operation.size())
                          .encode();
  result = nullptr;
  if (dispatch(harness, show_version, &result) != SB_ENGINE_STATUS_OK || result == nullptr) {
    return 11;
  }
  sb_engine_result_class_t result_class = SB_ENGINE_RESULT_NONE;
  if (sb_engine_result_class(result, &result_class) != SB_ENGINE_STATUS_OK ||
      result_class != SB_ENGINE_RESULT_ROW_BATCH ||
      !payload_contains(result, "operation_id=observability.show_version") ||
      !payload_contains(result, "product=ScratchBird")) {
    return 12;
  }
  sb_engine_batch_request_v1_t batch_request{};
  batch_request.struct_size = sizeof(batch_request);
  batch_request.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  batch_request.max_rows = 1;
  sb_engine_row_batch_view_v1_t batch{};
  if (sb_engine_result_next_batch(result, &batch_request, &batch) != SB_ENGINE_STATUS_OK ||
      batch.row_count != 1 || batch.end_of_stream != 1 ||
      !payload_contains(result, "product=ScratchBird")) {
    return 121;
  }
  if (sb_engine_result_next_batch(result, &batch_request, &batch) != SB_ENGINE_STATUS_OK ||
      batch.row_count != 0 || batch.end_of_stream != 1) {
    return 122;
  }
  (void)sb_engine_result_release(result);

  const std::string cluster_provider_info_operation =
      "operation_id=cluster.inspect_provider\n"
      "opcode=SBLR_CLUSTER_INSPECT_PROVIDER\n"
      "sblr_operation_family=sblr.cluster.private_operation.v3\n"
      "result_shape=cluster.provider.info.v1\n"
      "diagnostic_shape=engine.diagnostic.v1\n"
      "parser_package_uuid=019e05b1-f009-7000-8000-000000000020\n"
      "registry_snapshot_uuid=019e05b1-f009-7000-8000-000000000021\n"
      "trace_key=cluster-provider-info-public-abi\n"
      "contains_sql_text=false\n"
      "parser_resolved_names_to_uuids=true\n"
      "requires_security_context=true\n"
      "requires_transaction_context=false\n"
      "requires_cluster_authority=false\n";
  auto cluster_provider_info = scratchbird::engine::sblr::EnvelopeBuilder()
                                   .operation(scratchbird::engine::SblrOperationFamily::management_inspect, 1)
                                   .append_bytes(reinterpret_cast<const std::uint8_t*>(
                                                     cluster_provider_info_operation.data()),
                                                 cluster_provider_info_operation.size())
                                   .encode();
  result = nullptr;
  if (dispatch(harness, cluster_provider_info, &result) != SB_ENGINE_STATUS_OK ||
      result == nullptr) {
    return 123;
  }
  if (sb_engine_result_class(result, &result_class) != SB_ENGINE_STATUS_OK ||
      result_class != SB_ENGINE_RESULT_ROW_BATCH ||
      !payload_contains(result, "operation_id=cluster.inspect_provider") ||
      !payload_contains(result, "result_kind=cluster.provider.info.v1") ||
      !payload_contains(result, "provider_name=") ||
      !payload_contains(result, "provider_type=") ||
      !payload_contains(result, "provider_version=") ||
      !payload_contains(result, "support_status=") ||
      !payload_contains(result, "supports_execution=")) {
    return 124;
  }
  if (sb_engine_result_next_batch(result, &batch_request, &batch) != SB_ENGINE_STATUS_OK ||
      batch.row_count != 1 || batch.end_of_stream != 1 ||
      !payload_contains(result, "provider_name=") ||
      !payload_contains(result, "support_status=")) {
    return 125;
  }
  (void)sb_engine_result_release(result);

  const std::string sql_text_operation =
      "operation_id=observability.show_version\n"
      "opcode=SBLR_OBSERVABILITY_SHOW_VERSION\n"
      "result_shape=engine.api.result.v1\n"
      "diagnostic_shape=engine.diagnostic.v1\n"
      "contains_sql_text=true\n"
      "parser_resolved_names_to_uuids=true\n";
  auto sql_text_envelope = scratchbird::engine::sblr::EnvelopeBuilder()
                               .operation(scratchbird::engine::SblrOperationFamily::management_inspect, 1)
                               .append_bytes(reinterpret_cast<const std::uint8_t*>(sql_text_operation.data()),
                                             sql_text_operation.size())
                               .encode();
  result = nullptr;
  if (dispatch(harness, sql_text_envelope, &result) != SB_ENGINE_STATUS_INVALID_ARGUMENT || result == nullptr ||
      !has_diagnostic(result, "SBLR.ENVELOPE.INVALID")) {
    return 13;
  }
  (void)sb_engine_result_release(result);

  int family_gate = 20;
  for (const auto& row : scratchbird::engine::kSblrPriorityDRegistry) {
    auto envelope = scratchbird::engine::sblr::EnvelopeBuilder()
                        .operation(row.family, row.opcode_min)
                        .append_bytes(payload, sizeof(payload))
                        .encode();
    auto row_decoded = scratchbird::engine::sblr::EnvelopeReader::decode(envelope.data(), envelope.size());
    if (row_decoded.status != scratchbird::engine::SblrCodecStatus::ok) {
      return family_gate;
    }
    result = nullptr;
    const auto status = dispatch(harness, envelope, &result);
    if (row.behavior_status == scratchbird::engine::SblrBehaviorStatus::admission_only) {
      if (status != SB_ENGINE_STATUS_UNSUPPORTED || result == nullptr ||
          !has_diagnostic(result, "SBLR.EXECUTION.ADMISSION_ONLY")) {
        return family_gate + 1;
      }
    } else {
      if (status != SB_ENGINE_STATUS_CAPABILITY_DISABLED || result == nullptr ||
          !has_diagnostic(result, row.diagnostic_code)) {
        return family_gate + 2;
      }
    }
    (void)sb_engine_result_release(result);
    family_gate += 3;
  }

  auto acceleration = scratchbird::engine::sblr::EnvelopeBuilder()
                          .operation(scratchbird::engine::kSblrAccelerationRegistryRow.family,
                                     scratchbird::engine::kSblrAccelerationRegistryRow.opcode_min)
                          .append_bytes(payload, sizeof(payload))
                          .encode();
  result = nullptr;
  if (dispatch(harness, acceleration, &result) != SB_ENGINE_STATUS_CAPABILITY_DISABLED || result == nullptr ||
      !has_diagnostic(result, "SBLR.CAPABILITY.FORBIDDEN")) {
    return 5;
  }
  (void)sb_engine_result_release(result);

  auto donor = scratchbird::engine::sblr::EnvelopeBuilder()
                   .operation(scratchbird::engine::SblrOperationFamily::donor_meta, 1)
                   .append_bytes(payload, sizeof(payload))
                   .encode();
  result = nullptr;
  if (dispatch(harness, donor, &result) != SB_ENGINE_STATUS_UNSUPPORTED || result == nullptr ||
      !has_diagnostic(result, "SBLR.OPCODE.DONOR_META_FORBIDDEN")) {
    return 6;
  }
  (void)sb_engine_result_release(result);

  auto bad_checksum = relational;
  bad_checksum.back() ^= 0xffu;
  result = nullptr;
  if (dispatch(harness, bad_checksum, &result) != SB_ENGINE_STATUS_INVALID_ARGUMENT || result == nullptr ||
      !has_diagnostic(result, "SBLR.ENVELOPE.CHECKSUM_INVALID")) {
    return 7;
  }
  (void)sb_engine_result_release(result);

  auto bad_descriptor = scratchbird::engine::sblr::EnvelopeBuilder()
                            .operation(scratchbird::engine::SblrOperationFamily::relational_query, 1)
                            .descriptor(0, payload, sizeof(payload))
                            .append_bytes(payload, sizeof(payload))
                            .encode();
  result = nullptr;
  if (dispatch(harness, bad_descriptor, &result) != SB_ENGINE_STATUS_INVALID_ARGUMENT || result == nullptr ||
      !has_diagnostic(result, "SBLR.DESCRIPTOR.INVALID")) {
    return 8;
  }
  (void)sb_engine_result_release(result);

  auto unknown = scratchbird::engine::sblr::EnvelopeBuilder()
                     .operation(static_cast<scratchbird::engine::SblrOperationFamily>(999), 1)
                     .append_bytes(payload, sizeof(payload))
                     .encode();
  result = nullptr;
  if (dispatch(harness, unknown, &result) != SB_ENGINE_STATUS_UNSUPPORTED || result == nullptr ||
      !has_diagnostic(result, "SBLR.OPCODE.UNKNOWN")) {
    return 9;
  }
  (void)sb_engine_result_release(result);

  auto unsupported_version = scratchbird::engine::sblr::EnvelopeBuilder()
                                 .version(2, 0)
                                 .operation(scratchbird::engine::SblrOperationFamily::relational_query, 1)
                                 .append_bytes(payload, sizeof(payload))
                                 .encode();
  result = nullptr;
  if (dispatch(harness, unsupported_version, &result) != SB_ENGINE_STATUS_UNSUPPORTED || result == nullptr ||
      !has_diagnostic(result, "SBLR.VERSION.UNSUPPORTED")) {
    return 10;
  }
  (void)sb_engine_result_release(result);

  return 0;
}
