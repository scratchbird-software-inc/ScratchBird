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
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr const char* kMigrationAbiDatabasePath =
    "/tmp/sb_engine_public_sblr_admission_migration.sbdb";
constexpr const char* kMigrationReplayUuid =
    "019f0000-0000-7000-8000-00000000a001";

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
    open_params.database_path_utf8 = kMigrationAbiDatabasePath;
    open_params.database_path_size = std::strlen(kMigrationAbiDatabasePath);
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

std::string payload_text(sb_engine_result_t result) {
  sb_engine_string_view_t view{};
  if (sb_engine_result_payload(result, &view) != SB_ENGINE_STATUS_OK || view.data == nullptr) {
    return {};
  }
  return std::string(view.data, view.data + view.size_bytes);
}

bool payload_contains(sb_engine_result_t result, std::string_view expected) {
  const std::string payload = payload_text(result);
  return std::string_view(payload).find(expected) != std::string_view::npos;
}

void print_result_diagnostics(std::string_view label,
                              sb_engine_status_t status,
                              sb_engine_result_t result) {
  std::cerr << label << " status=" << status << "\n";
  if (result == nullptr) {
    std::cerr << label << " result=null\n";
    return;
  }
  sb_engine_diagnostic_set_view_t diagnostics{};
  if (sb_engine_result_diagnostics(result, &diagnostics) == SB_ENGINE_STATUS_OK) {
    for (std::uint64_t i = 0; i < diagnostics.diagnostic_count; ++i) {
      const auto& diagnostic = diagnostics.diagnostics[i];
      std::cerr << label << " diag="
                << std::string_view(diagnostic.symbolic_code.data,
                                    static_cast<std::size_t>(diagnostic.symbolic_code.size_bytes))
                << " detail="
                << std::string_view(diagnostic.safe_detail.data,
                                    static_cast<std::size_t>(diagnostic.safe_detail.size_bytes))
                << "\n";
    }
  }
  std::cerr << label << " payload=" << payload_text(result) << "\n";
}

sb_engine_status_t dispatch(Harness& harness,
                            const std::vector<std::uint8_t>& envelope,
                            sb_engine_result_t* result,
                            std::uint64_t transaction_ref = 0) {
  sb_engine_request_context_v1_t context{};
  context.struct_size = sizeof(context);
  context.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  context.effective_user_uuid = harness.session_params.effective_user_uuid;
  context.session_uuid = harness.session_params.session_uuid;
  context.trust_mode = SB_ENGINE_TRUST_EMBEDDED_TRUSTED;
  context.rights_set_ref = 1;
  context.capability_set_ref = 1;
  context.transaction_ref = transaction_ref;

  sb_engine_sblr_dispatch_params_v1_t params{};
  params.struct_size = sizeof(params);
  params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  params.envelope_bytes = envelope.data();
  params.envelope_size_bytes = envelope.size();
  return sb_engine_dispatch_sblr(harness.session, nullptr, &context, &params, result);
}

std::vector<std::uint8_t> operation_envelope(std::string_view text,
                                             scratchbird::engine::SblrOperationFamily family =
                                                 scratchbird::engine::SblrOperationFamily::management_control) {
  return scratchbird::engine::sblr::EnvelopeBuilder()
      .operation(family, 1)
      .append_bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())
      .encode();
}

std::string migration_operation_text(std::string_view operation_id,
                                     std::string_view opcode,
                                     std::string_view result_shape,
                                     std::string_view trace_key,
                                     std::string_view operands) {
  std::string text;
  text += "operation_id=";
  text += operation_id;
  text += "\n";
  text += "opcode=";
  text += opcode;
  text += "\n";
  text += "result_shape=";
  text += result_shape;
  text += "\n";
  text += "diagnostic_shape=engine.diagnostic.v1\n";
  text += "trace_key=";
  text += trace_key;
  text += "\n";
  text += "contains_sql_text=false\n";
  text += "parser_resolved_names_to_uuids=true\n";
  text += "requires_security_context=true\n";
  text += "requires_transaction_context=true\n";
  text += "requires_cluster_authority=false\n";
  text += operands;
  return text;
}

bool require_public_abi_migration_route(Harness& harness) {
  std::remove((std::string(kMigrationAbiDatabasePath) + ".sb.api_events").c_str());

  const auto begin = operation_envelope(migration_operation_text(
      "op.migration.begin_from_reference",
      "SBLR_MIGRATION_BEGIN_FROM_REFERENCE",
      "rs.migration.status.v1",
      "public-abi-migration-begin",
      "operand=text\treference_profile\tpostgres\n"
      "operand=text\treference_package\tpg_compat_pack\n"));

  sb_engine_result_t result = nullptr;
  sb_engine_status_t status = dispatch(harness, begin, &result, 101);
  if (status != SB_ENGINE_STATUS_OK || result == nullptr ||
      !payload_contains(result, "operation_id=op.migration.begin_from_reference") ||
      !payload_contains(result, "state=prepared")) {
    print_result_diagnostics("migration_begin", status, result);
    if (result != nullptr) (void)sb_engine_result_release(result);
    return false;
  }
  (void)sb_engine_result_release(result);

  const auto alter = operation_envelope(migration_operation_text(
      "op.migration.alter",
      "SBLR_MIGRATION_ALTER",
      "rs.migration.status.v1",
      "public-abi-migration-alter",
      std::string("operand=text\tmigration_ref\t") + kMigrationReplayUuid +
          "\noperand=text\tmigration_action\tstart\n"));
  result = nullptr;
  status = dispatch(harness, alter, &result, 102);
  if (status != SB_ENGINE_STATUS_OK || result == nullptr ||
      !payload_contains(result, "operation_id=op.migration.alter") ||
      !payload_contains(result, kMigrationReplayUuid) ||
      !payload_contains(result, "state=running")) {
    print_result_diagnostics("migration_alter", status, result);
    if (result != nullptr) (void)sb_engine_result_release(result);
    return false;
  }
  (void)sb_engine_result_release(result);

  const auto show = operation_envelope(migration_operation_text(
      "op.show.migration",
      "SBLR_SHOW_MIGRATION",
      "rs.migration.status.v1",
      "public-abi-migration-show",
      std::string("operand=text\tmigration_ref\t") + kMigrationReplayUuid + "\n"));
  result = nullptr;
  status = dispatch(harness, show, &result, 103);
  if (status != SB_ENGINE_STATUS_OK || result == nullptr ||
      !payload_contains(result, "operation_id=op.show.migration") ||
      !payload_contains(result, kMigrationReplayUuid)) {
    print_result_diagnostics("migration_show", status, result);
    if (result != nullptr) (void)sb_engine_result_release(result);
    return false;
  }
  (void)sb_engine_result_release(result);
  return true;
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

  auto reference = scratchbird::engine::sblr::EnvelopeBuilder()
                   .operation(scratchbird::engine::SblrOperationFamily::reference_meta, 1)
                   .append_bytes(payload, sizeof(payload))
                   .encode();
  result = nullptr;
  if (dispatch(harness, reference, &result) != SB_ENGINE_STATUS_UNSUPPORTED || result == nullptr ||
      !has_diagnostic(result, "SBLR.OPCODE.REFERENCE_META_FORBIDDEN")) {
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

  if (!require_public_abi_migration_route(harness)) {
    return 14;
  }

  return 0;
}
