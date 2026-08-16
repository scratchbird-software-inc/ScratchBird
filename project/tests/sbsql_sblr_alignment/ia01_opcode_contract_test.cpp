// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_opcode_registry.hpp"
#include "engine/sblr/sblr_opcode_stream.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace sblr = scratchbird::engine::sblr;

namespace {
struct Expected {
  std::string_view opcode;
  std::uint16_t code;
  std::string_view operation;
  std::string_view operand;
  std::string_view result;
};

constexpr std::array<Expected, 17> kExpected{{
    {"SBLR_PACKAGE_BEGIN", 0x0001u, "engine.op.package_begin", "package_header", "void"},
    {"SBLR_PACKAGE_END", 0x0002u, "engine.op.package_end", "package_footer", "void"},
    {"SBLR_LITERAL", 0x0003u, "engine.op.literal", "typed_literal", "typed_value"},
    {"SBLR_PARAMETER", 0x0004u, "engine.op.parameter", "parameter_descriptor_ref", "typed_value"},
    {"SBLR_VARIABLE", 0x0005u, "engine.op.variable", "variable_descriptor_ref", "typed_value"},
    {"SBLR_SOURCE_MAP", 0x0006u, "engine.op.source_map", "source_map_entry_vector", "void"},
    {"SBLR_ERROR_VECTOR", 0x0007u, "engine.op.error_vector", "diagnostic_vector", "void"},
    {"SBLR_PROJECT", 0x0500u, "engine.op.project", "projection_descriptor", "rowset_descriptor"},
    {"SBLR_AGGREGATE", 0x0501u, "engine.op.aggregate", "aggregate_descriptor", "rowset_descriptor"},
    {"SBLR_GROUP", 0x0502u, "engine.op.group", "group_descriptor", "rowset_descriptor"},
    {"SBLR_SORT", 0x0503u, "engine.op.sort", "sort_descriptor", "rowset_descriptor"},
    {"SBLR_LIMIT", 0x0504u, "engine.op.limit", "limit_descriptor", "rowset_descriptor"},
    {"SBLR_WINDOW", 0x0505u, "engine.op.window", "window_descriptor", "rowset_descriptor"},
    {"SBLR_RETURN_RESULT_SET", 0x0506u, "engine.op.return_result_set", "result_set_return_descriptor", "result_set_handle"},
    {"SBLR_DIAGNOSTIC_REFUSAL", 0x1900u, "engine.op.diagnostic_refusal", "diagnostic_refusal_descriptor", "diagnostic_refusal_result"},
    {"SBLR_DIAGNOSTIC_RESET", 0x1901u, "engine.op.diagnostic_reset", "diagnostic_reset_descriptor", "diagnostic_reset_result"},
    {"SBLR_DESCRIPTOR_TRANSFORM", 0x1902u, "engine.op.descriptor_transform", "descriptor_transform_descriptor", "descriptor_transform_result"},
}};

using Bytes = std::vector<std::uint8_t>;
void U32(Bytes* out, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8)
    out->push_back(static_cast<std::uint8_t>(value >> shift));
}
void U64(Bytes* out, std::uint64_t value) {
  for (unsigned shift = 0; shift != 64; shift += 8)
    out->push_back(static_cast<std::uint8_t>(value >> shift));
}
void Store32(Bytes* bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8)
    (*bytes)[offset++] = static_cast<std::uint8_t>(value >> shift);
}
void RefreshCrc(Bytes* bytes) {
  Store32(bytes, bytes->size() - 12,
          sblr::SblrCrc32c(bytes->data(), bytes->size() - 12));
}
Bytes EncodeUncheckedStream(
    const std::vector<sblr::SblrOperationEnvelope>& operations,
    const std::array<std::uint8_t, 16>& package,
    const std::array<std::uint8_t, 16>& registry) {
  Bytes records;
  for (const auto& operation : operations) {
    const auto encoded = sblr::EncodeSblrEnvelope(operation);
    U64(&records, encoded.size());
    records.insert(records.end(), encoded.begin(), encoded.end());
  }
  Bytes bytes{'S', 'B', 'O', 'S', 1, 0, 0, 0, 64, 0, 0, 0, 0, 0, 0, 0};
  U32(&bytes, static_cast<std::uint32_t>(operations.size()));
  U32(&bytes, 0);
  bytes.insert(bytes.end(), package.begin(), package.end());
  bytes.insert(bytes.end(), registry.begin(), registry.end());
  U64(&bytes, records.size());
  bytes.insert(bytes.end(), records.begin(), records.end());
  bytes.insert(bytes.end(), {'S', 'B', 'S', 'T'});
  U32(&bytes, sblr::SblrCrc32c(bytes.data(), bytes.size()));
  U64(&bytes, bytes.size() + 8);
  return bytes;
}
}

int main() {
  int failures = 0;
  for (const auto& expected : kExpected) {
    const auto* entry = sblr::LookupSblrOpcode(expected.opcode);
    const bool ok = entry && entry->code == expected.code &&
                    entry->operation_id == expected.operation &&
                    entry->operand_contract == expected.operand &&
                    entry->result_contract == expected.result &&
                    entry->executor_id == expected.operation &&
                    entry->executor_evidence_required &&
                    entry->executor_evidence_accepted ==
                        (expected.opcode == "SBLR_PACKAGE_BEGIN" ||
                         expected.opcode == "SBLR_PACKAGE_END") &&
                    entry->missing_executor_evidence_diagnostic ==
                        "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";
    if (!ok) {
      std::cerr << "IA-01 contract mismatch: " << expected.opcode << '\n';
      ++failures;
    }
    const auto identity = sblr::ValidateSblrOpcodeIdentity(
        expected.code, expected.operation, expected.opcode);
    if (!identity.ok || identity.entry != entry) {
      std::cerr << "IA-01 canonical identity rejected: " << expected.opcode << '\n';
      ++failures;
    }
    const auto wrong_code = sblr::ValidateSblrOpcodeIdentity(
        static_cast<std::uint16_t>(expected.code ^ 0x8000u),
        expected.operation, expected.opcode);
    const auto wrong_operation = sblr::ValidateSblrOpcodeIdentity(
        expected.code, "engine.op.not_admitted", expected.opcode);
    if (wrong_code.ok || wrong_operation.ok ||
        wrong_code.diagnostic_id != "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH" ||
        wrong_operation.diagnostic_id != "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH") {
      std::cerr << "IA-01 malformed identity did not fail closed: "
                << expected.opcode << '\n';
      ++failures;
    }
    sblr::SblrOperationEnvelope envelope;
    envelope.opcode_code = expected.code;
    envelope.operation_id = expected.operation;
    envelope.opcode = expected.opcode;
    envelope.requires_security_context = true;
    envelope.requires_transaction_context = entry && entry->requires_transaction_context;
    const auto admission = sblr::ValidateSblrOpcodeForEnvelope(envelope);
    const bool framing = expected.opcode == "SBLR_PACKAGE_BEGIN" ||
                         expected.opcode == "SBLR_PACKAGE_END";
    if ((framing && (!admission.ok || admission.entry != entry)) ||
        (!framing &&
         (admission.ok || admission.entry != entry ||
          admission.diagnostic_id != "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING" ||
          admission.detail != "executor_evidence_not_accepted:" +
                                  std::string(expected.operation)))) {
      std::cerr << "IA-01 executor evidence state mismatch: "
                << expected.opcode << '\n';
      ++failures;
    }
  }

  constexpr std::string_view package_uuid =
      "018f1234-5678-7abc-8def-0123456789ab";
  constexpr std::string_view registry_uuid =
      "018f4321-8765-7cba-8fed-ba9876543210";
  constexpr std::string_view parser_uuid =
      "018faaaa-bbbb-7ccc-8ddd-eeeeeeeeeeee";
  const std::array<std::uint8_t, 16> package_bytes{
      0x01, 0x8f, 0x12, 0x34, 0x56, 0x78, 0x7a, 0xbc,
      0x8d, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab};
  const std::array<std::uint8_t, 16> registry_bytes{
      0x01, 0x8f, 0x43, 0x21, 0x87, 0x65, 0x7c, 0xba,
      0x8f, 0xed, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};
  const auto frame = [&](bool begin) {
    auto operation = sblr::MakeSblrEnvelope(
        begin ? "engine.op.package_begin" : "engine.op.package_end",
        begin ? "SBLR_PACKAGE_BEGIN" : "SBLR_PACKAGE_END",
        begin ? "ia01.package.begin" : "ia01.package.end");
    operation.opcode_code = begin ? 0x0001u : 0x0002u;
    operation.result_shape = "void";
    operation.diagnostic_shape = "diagnostic_vector";
    operation.parser_package_uuid = parser_uuid;
    operation.registry_snapshot_uuid = registry_uuid;
    operation.parser_resolved_names_to_uuids = true;
    sblr::SblrOperand operand;
    operand.ordinal = 1;
    operand.type = begin ? "package.header" : "package.footer";
    operand.name = "package_descriptor";
    operand.value_kind = sblr::SblrValueKind::descriptor_ref;
    operand.value_body.assign(package_bytes.begin(), package_bytes.end());
    operation.operands.push_back(std::move(operand));
    return operation;
  };
  sblr::SblrOpcodeStream package;
  package.package_descriptor_uuid = package_uuid;
  package.registry_snapshot_uuid = registry_uuid;
  package.operations = {frame(true), frame(false)};
  const auto bytes = sblr::EncodeSblrOpcodeStream(package);
  if (bytes.empty()) {
    std::cerr << "CSC-TEST-002317 canonical atomic package did not encode\n";
    ++failures;
  } else {
    sblr::SblrOpcodeStreamAdmission admitted;
    admitted.admitted_registry_snapshot_uuid = registry_uuid;
    admitted.authenticated = true;
    admitted.descriptor_class_accepted = true;
    admitted.gateway_pass_through = true;
    admitted.executor_evidence_accepted = true;
    const std::string_view encoded(
        reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto canonical = sblr::AdmitSblrOpcodeStream(encoded, admitted);
    if (!canonical.ok || canonical.stream.operations.size() != 2) {
      std::cerr << "CSC-TEST-002317/CSC-TEST-002321 canonical atomic pair refused: "
                << canonical.diagnostic_id << ' ' << canonical.detail << '\n';
      ++failures;
    }
    auto malformed = bytes;
    malformed[16] = 1;
    const auto malformed_result = sblr::DecodeSblrOpcodeStream(
        {reinterpret_cast<const char*>(malformed.data()), malformed.size()});
    if (malformed_result.ok ||
        malformed_result.diagnostic_id != "SBLR.OPERAND_INVALID") {
      std::cerr << "CSC-TEST-002318/CSC-TEST-002322 malformed count did not fail closed\n";
      ++failures;
    }
    auto missing_evidence = admitted;
    missing_evidence.executor_evidence_accepted = false;
    const auto evidence_result =
        sblr::AdmitSblrOpcodeStream(encoded, missing_evidence);
    if (evidence_result.ok || evidence_result.diagnostic_id !=
                                  "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING") {
      std::cerr << "CSC-TEST-002319/CSC-TEST-002323 missing evidence did not fail closed\n";
      ++failures;
    }
    auto cancelled = admitted;
    cancelled.cancelled = true;
    const auto cancelled_result = sblr::AdmitSblrOpcodeStream(encoded, cancelled);
    if (cancelled_result.ok ||
        cancelled_result.diagnostic_id != "PROCESS.CANCELLED") {
      std::cerr << "CSC-TEST-002320/CSC-TEST-002324 cancellation did not remain atomic\n";
      ++failures;
    }

    const auto expect_decode = [&](const Bytes& candidate,
                                   std::string_view diagnostic,
                                   std::string_view case_name) {
      const auto result = sblr::DecodeSblrOpcodeStream(
          {reinterpret_cast<const char*>(candidate.data()), candidate.size()});
      if (result.ok || result.diagnostic_id != diagnostic) {
        std::cerr << "CSC-TEST-002318/CSC-TEST-002322 " << case_name
                  << " expected " << diagnostic << " got "
                  << result.diagnostic_id << '\n';
        ++failures;
      }
    };
    auto bad_version = bytes;
    bad_version[4] = 2;
    RefreshCrc(&bad_version);
    expect_decode(bad_version, "SBLR.OPERAND_INVALID", "unknown version");

    auto bad_crc = bytes;
    bad_crc[bad_crc.size() - 12] ^= 1;
    expect_decode(bad_crc, "SBLR.OPERAND_INVALID", "CRC mismatch");

    auto trailing = bytes;
    trailing.push_back(0);
    expect_decode(trailing, "SBLR.OPERAND_INVALID", "trailing byte");

    auto nested = EncodeUncheckedStream(
        {frame(true), frame(true), frame(false)}, package_bytes, registry_bytes);
    expect_decode(nested, "SBLR.OPERAND_INVALID", "nested package");

    auto mismatched_end = frame(false);
    mismatched_end.operands[0].value_body.back() ^= 1;
    auto mismatched_descriptor = EncodeUncheckedStream(
        {frame(true), mismatched_end}, package_bytes, registry_bytes);
    expect_decode(mismatched_descriptor, "DATATYPE.DESCRIPTOR_INVALID",
                  "mismatched footer descriptor");

    auto stale_member = frame(false);
    stale_member.registry_snapshot_uuid =
        "018f4321-8765-7cba-8fed-ba9876543211";
    auto mismatched_registry = EncodeUncheckedStream(
        {frame(true), stale_member}, package_bytes, registry_bytes);
    expect_decode(mismatched_registry, "DATATYPE.DESCRIPTOR_INVALID",
                  "member registry mismatch");

    auto descriptor_and_auth = admitted;
    descriptor_and_auth.descriptor_class_accepted = false;
    descriptor_and_auth.authenticated = false;
    const auto descriptor_precedence =
        sblr::AdmitSblrOpcodeStream(encoded, descriptor_and_auth);
    if (descriptor_precedence.ok || descriptor_precedence.diagnostic_id !=
                                        "DATATYPE.DESCRIPTOR_INVALID") {
      std::cerr << "CSC-TEST-002318/CSC-TEST-002322 descriptor/security precedence changed\n";
      ++failures;
    }
    auto resource_and_cancel = admitted;
    resource_and_cancel.resource_budget_available = false;
    resource_and_cancel.cancelled = true;
    const auto resource_precedence =
        sblr::AdmitSblrOpcodeStream(encoded, resource_and_cancel);
    if (resource_precedence.ok || resource_precedence.diagnostic_id !=
                                      "RESOURCE.BUDGET_EXCEEDED") {
      std::cerr << "CSC-TEST-002320/CSC-TEST-002324 resource/cancel precedence changed\n";
      ++failures;
    }
  }
  return failures == 0 ? 0 : 1;
}
