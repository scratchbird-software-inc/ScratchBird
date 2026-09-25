// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "../../../support/binary_uuid_fixture.hpp"
#include "sblr/sblr_engine_envelope.hpp"
#include "sblr/sblr_opcode_registry.hpp"
#include "registry/function_seed_registry.hpp"
#include "core/datatypes/datatype_catalog_manifest.hpp"
#include "query/projection_api.hpp"

#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace scratchbird::tests::sbsql {

inline engine::internal_api::EngineProjectionFunctionArgument UuidProjectionArgumentForTest(
    std::string name, const core::platform::Uuid& uuid) {
  engine::internal_api::EngineProjectionFunctionArgument value;
  value.name = std::move(name);
  value.type_name = "uuid";
  value.binary_value.assign(uuid.bytes.begin(), uuid.bytes.end());
  return value;
}
inline engine::sblr::SblrOperand UuidProjectionOperandForTest(
    std::string name, const std::vector<std::uint8_t>& bytes) {
  if (bytes.size() != 16) throw std::logic_error("UUID literal requires binary16");
  namespace dt = core::datatypes;
  const auto catalog = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!catalog.ok()) throw std::logic_error("Core datatype catalog unavailable");
  const auto row = dt::LookupDatatypeCatalogRow(catalog.manifest, dt::CanonicalTypeId::uuid);
  if (!row.ok() || row.manifest.descriptor_rows.size() != 1)
    throw std::logic_error("Core UUID descriptor unavailable");
  engine::sblr::SblrOperand operand;
  operand.name = std::move(name);
  operand.type = "uuid";
  operand.value_kind = engine::sblr::SblrValueKind::literal_typed;
  const auto& descriptor = row.manifest.descriptor_rows.front().descriptor_uuid.value;
  operand.value_body.assign(descriptor.bytes.begin(), descriptor.bytes.end());
  operand.value_body.resize(24, 0);
  operand.value_body[16] = 16;
  operand.value_body.insert(operand.value_body.end(), bytes.begin(), bytes.end());
  return operand;
}

inline engine::sblr::SblrOperationEnvelope CanonicalizeProjectionEnvelopeForTest(
    engine::sblr::SblrOperationEnvelope envelope) {
  const auto* opcode = engine::sblr::LookupSblrOpcode(envelope.opcode);
  if (opcode == nullptr || opcode->operation_id != envelope.operation_id) {
    throw std::logic_error("projection test envelope has no canonical opcode identity");
  }

  envelope.opcode_code = opcode->code;
  envelope.parser_package_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-00000000f001");
  envelope.registry_snapshot_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-00000000f002");

  std::string function_id;
  std::size_t function_argument_count = 0;
  bool result_type_present = false;
  for (const auto& operand : envelope.operands) {
    if (operand.name == "projection_0_function_id") function_id = operand.value;
    if (operand.name == "projection_0_function_arg_count") {
      for (const char ch : operand.value) {
        if (ch < '0' || ch > '9') {
          throw std::logic_error(
              "projection test function argument count is malformed");
        }
        function_argument_count =
            function_argument_count * 10 + static_cast<std::size_t>(ch - '0');
      }
    }
    if (operand.name == "projection_0_type") result_type_present = true;
  }
  if (!result_type_present) {
    std::string_view result_type;
    if (function_id == "sb.scalar.mga_snapshot_id" ||
        function_id == "sb.scalar.relation_row_estimate" ||
        function_id == "sb.scalar.table_size" ||
        function_id == "sb.scalar.temp_buffers") {
      result_type = "uint64";
    } else if (function_id == "sb.session.savepoint_active" ||
               function_id == "sb.scalar.has_table_privilege" ||
               function_id == "sb.scalar.has_column_privilege" ||
               function_id == "sb.scalar.has_function_privilege" ||
               function_id == "sb.scalar.has_schema_privilege" ||
               function_id == "sb.scalar.pg_try_advisory_lock_key" ||
               function_id == "sb.scalar.pg_advisory_xact_lock_key") {
      result_type = "boolean";
    } else if (function_id == "sb.session.pg_xact_status" ||
               function_id == "sb.scalar.normalize_text_form" ||
               function_id == "sb.cursor.state" ||
               function_id == "sb.crypto.sha3_256") {
      result_type = "character";
    } else if (function_id == "sb.json.array_to_json" ||
               function_id == "sb.rowset.generate_series") {
      result_type = "json_document";
    } else if (function_id == "sb.lob.size") {
      result_type = "int64";
    } else if (function_id == "sb.scalar.at_time_zone") {
      result_type = "timestamp_tz";
    } else {
      throw std::logic_error(
          "projection test envelope lacks an exact result descriptor");
    }
    envelope.operands.push_back(
        {"text", "projection_0_type", std::string(result_type)});
  }

  // The generated SBSFC fixtures pass concrete scalar values, so each
  // function child is an explicit literal expression in the canonical QOW
  // expression graph.  Older fixtures omitted only this child-node tag.
  for (std::size_t argument = 0; argument < function_argument_count;
       ++argument) {
    const std::string name = "projection_0_arg_" +
                             std::to_string(argument) + "_expr_kind";
    bool present = false;
    for (const auto& operand : envelope.operands) {
      if (operand.name == name) {
        present = true;
        break;
      }
    }
    if (!present) envelope.operands.push_back({"text", name, "literal"});
  }

  for (std::size_t index = 0; index < envelope.operands.size(); ++index) {
    auto& operand = envelope.operands[index];
    if (operand.type == "uuid" && !operand.value_body.empty()) {
      operand.ordinal = static_cast<std::uint32_t>(index + 1);
      if (!operand.value.empty()) throw std::logic_error("UUID fixture has a text mirror");
      continue;
    }
    const auto value = std::move(operand.value);
    operand.ordinal = static_cast<std::uint32_t>(index + 1);
    operand.value_kind = engine::sblr::SblrValueKind::literal_typed;
    operand.value_flags = 0;
    operand.value_body.assign(24 + value.size(), 0);

    // Test-only nonzero Core descriptor identity followed by the canonical
    // little-endian scalar byte count and the exact UTF-8 payload.
    constexpr std::uint8_t kDescriptorUuid[16] = {
        0x01, 0x9f, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
        0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x03};
    for (std::size_t byte = 0; byte < 16; ++byte) {
      operand.value_body[byte] = kDescriptorUuid[byte];
    }
    const auto value_size = static_cast<std::uint64_t>(value.size());
    for (unsigned byte = 0; byte < 8; ++byte) {
      operand.value_body[16 + byte] =
          static_cast<std::uint8_t>((value_size >> (byte * 8)) & 0xffu);
    }
    for (std::size_t byte = 0; byte < value.size(); ++byte) {
      operand.value_body[24 + byte] = static_cast<std::uint8_t>(value[byte]);
    }
  }
  static const auto functions = engine::functions::BuildStandardFunctionSeedPackage();
  const auto* function = functions.registry.Lookup(function_id);
  if (!function) throw std::logic_error("projection fixture function is not registered");
  engine::sblr::SblrOperand identity;
  identity.ordinal = envelope.operands.size() + 1;
  identity.name = "projection_0_function_uuid";
  identity.type = "uuid";
  identity.value_kind = engine::sblr::SblrValueKind::uuid_ref;
  identity.value_body.assign(function->function_uuid.bytes.begin(),
                             function->function_uuid.bytes.end());
  envelope.operands.push_back(std::move(identity));
  return envelope;
}

}  // namespace scratchbird::tests::sbsql
