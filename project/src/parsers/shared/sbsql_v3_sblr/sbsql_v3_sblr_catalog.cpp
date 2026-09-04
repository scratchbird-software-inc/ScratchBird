// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sbsql_v3_sblr_catalog.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <sstream>
#include <string>
#include <utility>

namespace scratchbird::parser::sbsql_v3_sblr {
namespace {

std::vector<std::string_view> Split(std::string_view text, char delimiter) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (start <= text.size()) {
    const auto pos = text.find(delimiter, start);
    if (pos == std::string_view::npos) {
      parts.push_back(text.substr(start));
      break;
    }
    parts.push_back(text.substr(start, pos - start));
    start = pos + 1;
  }
  return parts;
}

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.substr(0, prefix.size()) == prefix;
}

std::uint32_t StableProbeOpcode(std::string_view text) {
  std::uint32_t hash = 2166136261u;
  for (const unsigned char ch : text) {
    hash ^= ch;
    hash *= 16777619u;
  }
  return (hash == 0 ? 1u : hash);
}

const std::array<CommandFamilySblrRoute, 14>& MissingFunctionalityRouteMap() {
  static const std::array<CommandFamilySblrRoute, 14> kRoutes{{
      {"sbsql.migration_management", "sblr.migration.operation.v3",
       "sblr.migration.operation.v3", "engine.op.migration_begin_donor",
       "SBLR_MIGRATION_BEGIN_DONOR", "migration_operation_result",
       "diagnostic_vector", "migration_begin_donor_descriptor", true, true, false},
      {"sbsql.temporal_bitemporal", "sblr.versioned.history.read.v3",
       "sblr.versioned.history.read.v3", "engine.op.bitemporal_as_of",
       "SBLR_BITEMPORAL_AS_OF", "versioned_history_result",
       "diagnostic_vector", "bitemporal_as_of_descriptor", true, true, false},
      {"sbsql.versioned_history_mutate", "sblr.versioned.history.mutate.v3",
       "sblr.versioned.history.mutate.v3", "engine.op.verifiable_history_prove",
       "SBLR_VERIFIABLE_HISTORY_PROVE", "versioned_history_result",
       "diagnostic_vector", "verifiable_history_prove_descriptor", true, true, false},
      {"sbsql.structured_types", "sblr.catalog.mutation.v3",
       "sblr.catalog.mutation.v3", "engine.op.ddl_create_type",
       "SBLR_DDL_CREATE_TYPE", "ddl_result",
       "diagnostic_vector", "create_type_descriptor", true, true, false},
      {"sbsql.kv_structured_read", "sblr.kv.structured.read.v3",
       "sblr.kv.structured.read.v3", "engine.op.kv_structured_read",
       "SBLR_KV_STRUCTURED_READ", "kv_structured_result",
       "diagnostic_vector", "kv_structured_read_descriptor", true, true, false},
      {"sbsql.kv_structured_mutate", "sblr.kv.structured.mutate.v3",
       "sblr.kv.structured.mutate.v3", "engine.op.kv_structured_mutate",
       "SBLR_KV_STRUCTURED_MUTATE", "kv_structured_result",
       "diagnostic_vector", "kv_structured_mutate_descriptor", true, true, false},
      {"sbsql.ddl_catalog_gaps", "sblr.catalog.mutation.v3",
       "sblr.catalog.mutation.v3", "engine.op.ddl_create_type",
       "SBLR_DDL_CREATE_TYPE", "ddl_result",
       "diagnostic_vector", "create_type_descriptor", true, true, false},
      {"sbsql.dml_upsert_variants", "sblr.dml.merge.v3",
       "sblr.dml.merge.v3", "dml.merge_rows",
       "SBLR_DML_MERGE_ROWS", "mutation_result",
       "diagnostic_vector", "dml_merge_rows_descriptor", true, true, false},
      {"sbsql.bulk_import_export", "sblr.bulk.import.v3",
       "sblr.bulk.import.v3", "engine.op.bulk_import_stream",
       "SBLR_BULK_IMPORT_STREAM", "bulk_mutation_result",
       "diagnostic_vector", "bulk_import_stream_descriptor", true, true, false},
      {"sbsql.bulk_export", "sblr.bulk.export.v3",
       "sblr.bulk.export.v3", "bulk.export",
       "SBLR_BULK_EXPORT_STREAM", "bulk_read_result",
       "diagnostic_vector", "bulk_export_stream_descriptor", true, true, false},
      {"sbsql.native_system_variables", "sblr.expression.runtime.v3",
       "sblr.query.relational.v3", "query.evaluate_projection",
       "SBLR_QUERY_EVALUATE_PROJECTION", "scalar_projection_row_v1",
       "diagnostic_vector", "scalar_projection_operand_vector_v1", true, true, false},
      {"sbsql.last_day_builtin", "sblr.expression.runtime.v3",
       "sblr.query.relational.v3", "query.evaluate_projection",
       "SBLR_QUERY_EVALUATE_PROJECTION", "scalar_projection_row_v1",
       "diagnostic_vector", "scalar_projection_operand_vector_v1", true, true, false},
      {"sbsql.acceleration_management", "sblr.acceleration.llvm.v3",
       "sblr.acceleration.llvm.v3", "extensibility.compile_llvm_module",
       "SBLR_EXTENSIBILITY_COMPILE_LLVM_MODULE", "llvm_module_compile_result",
       "diagnostic_vector", "llvm_module_compile_descriptor", false, true, false},
      {"sbsql.reference_command_function_backfill", "sblr.query.relational.v3",
       "sblr.query.relational.v3", "query.evaluate_projection",
       "SBLR_QUERY_EVALUATE_PROJECTION", "scalar_projection_row_v1",
       "diagnostic_vector", "scalar_projection_operand_vector_v1", true, true, false},
  }};
  return kRoutes;
}

}  // namespace

bool IsUuidV7(std::string_view uuid_text) {
  return uuid_text.size() == 36 && uuid_text[8] == '-' && uuid_text[13] == '-' &&
         uuid_text[18] == '-' && uuid_text[23] == '-' && uuid_text[14] == '7';
}

std::optional<std::uint32_t> ParseOpcodeValue(std::string_view text) {
  std::uint32_t value = 0;
  if (StartsWith(text, "0x") || StartsWith(text, "0X")) {
    const auto digits = text.substr(2);
    const auto* begin = digits.data();
    const auto* end = digits.data() + digits.size();
    const auto result = std::from_chars(begin, end, value, 16);
    if (result.ec == std::errc() && result.ptr == end) {
      return value;
    }
    return std::nullopt;
  }
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, value, 10);
  if (result.ec == std::errc() && result.ptr == end) {
    return value;
  }
  return std::nullopt;
}

const CommandFamilySblrRoute* RouteForCommandFamily(std::string_view command_family) {
  for (const auto& route : MissingFunctionalityRouteMap()) {
    if (route.command_family == command_family) return &route;
  }
  if (command_family == "sbsql.dml") return RouteForCommandFamily("sbsql.dml_upsert_variants");
  if (command_family == "sbsql.temporal") return RouteForCommandFamily("sbsql.temporal_bitemporal");
  if (command_family == "sbsql.structured_type") return RouteForCommandFamily("sbsql.structured_types");
  if (command_family == "sbsql.ddl_catalog") return RouteForCommandFamily("sbsql.ddl_catalog_gaps");
  if (command_family == "sbsql.bulk") return RouteForCommandFamily("sbsql.bulk_import_export");
  return nullptr;
}

std::vector<CommandFamilySblrRoute> RequiredMissingFunctionalityRoutes() {
  return {MissingFunctionalityRouteMap().begin(), MissingFunctionalityRouteMap().end()};
}

SblrOpcodeEntry MakeOpcodeEntryForRoute(const CommandFamilySblrRoute& route) {
  SblrOpcodeEntry entry;
  entry.sblr_operation = route.sblr_opcode;
  entry.opcode_value = StableProbeOpcode(route.sblr_opcode);
  entry.payload_class = route.payload_class;
  entry.api_operation_id = route.operation_id;
  entry.cluster_authority_required = false;
  entry.fail_closed_without_cluster_authority = false;
  entry.raw_sql_payload_allowed = false;
  return entry;
}

SblrEnvelope MakeEnvelopeForRoute(const CommandFamilySblrRoute& route,
                                  std::string binding_epoch,
                                  std::string bound_root_uuid,
                                  std::string descriptor_digest) {
  const auto entry = MakeOpcodeEntryForRoute(route);
  SblrEnvelope envelope;
  envelope.sblr_operation = route.sblr_opcode;
  envelope.opcode_value = entry.opcode_value;
  envelope.sblr_version = 3;
  envelope.binding_epoch = std::move(binding_epoch);
  envelope.bound_root_uuid = std::move(bound_root_uuid);
  envelope.descriptor_digest = std::move(descriptor_digest);
  envelope.payload_class = route.payload_class;
  envelope.contains_raw_sql_text = route.contains_raw_sql_text;
  envelope.cluster_authority_present = false;
  return envelope;
}

std::string EncodeRouteForServerAdmission(const CommandFamilySblrRoute& route) {
  std::ostringstream out;
  out << "envelope=SBLRExecutionEnvelope.v3\n";
  out << "envelope_major=3\n";
  out << "sblr_version=sblr_v3\n";
  out << "operation_id=" << route.operation_id << "\n";
  out << "sblr_operation_family=" << route.route_operation_family << "\n";
  out << "result_shape=" << route.result_shape << "\n";
  out << "diagnostic_shape=" << route.diagnostic_shape << "\n";
  out << "parser_resolved_names_to_uuids=true\n";
  out << "contains_sql_text=false\n";
  out << "engine_api_command_route=true\n";
  out << "public_sbsql_exact_command=true\n";
  return out.str();
}

bool ValidateOpcodeEntry(const SblrOpcodeEntry& entry, std::vector<std::string>* errors) {
  const auto before = errors ? errors->size() : 0;
  const auto add = [&](std::string message) {
    if (errors) {
      errors->push_back(std::move(message));
    }
  };
  if (!StartsWith(entry.sblr_operation, "SBLR_")) {
    add("sblr operation must use SBLR_ prefix");
  }
  if (entry.opcode_value == 0) {
    add("opcode value must be non-zero");
  }
  if (entry.payload_class.empty()) {
    add("payload class is required");
  }
  if (entry.raw_sql_payload_allowed) {
    add("raw SQL payloads are forbidden");
  }
  if (entry.cluster_authority_required && !entry.fail_closed_without_cluster_authority) {
    add("cluster authority rows must fail closed without authority");
  }
  return !errors || errors->size() == before;
}

bool ValidateEnvelope(const SblrOpcodeEntry& entry, const SblrEnvelope& envelope, std::vector<std::string>* errors) {
  const auto before = errors ? errors->size() : 0;
  const auto add = [&](std::string message) {
    if (errors) {
      errors->push_back(std::move(message));
    }
  };
  ValidateOpcodeEntry(entry, errors);
  if (envelope.sblr_operation != entry.sblr_operation) {
    add("envelope operation does not match opcode entry");
  }
  if (envelope.opcode_value != entry.opcode_value) {
    add("envelope opcode value does not match opcode entry");
  }
  if (envelope.sblr_version == 0) {
    add("SBLR version must be non-zero");
  }
  if (!IsUuidV7(envelope.bound_root_uuid)) {
    add("bound root UUID must be UUIDv7");
  }
  if (envelope.payload_class != entry.payload_class) {
    add("payload class does not match opcode entry");
  }
  if (envelope.contains_raw_sql_text || entry.raw_sql_payload_allowed) {
    add("engine envelope cannot contain raw SQL text");
  }
  if (entry.cluster_authority_required && !envelope.cluster_authority_present) {
    add("cluster authority token required for cluster envelope");
  }
  return !errors || errors->size() == before;
}

std::string EncodeEnvelopeForProbe(const SblrEnvelope& envelope) {
  std::ostringstream out;
  out << "SBLR3|" << envelope.sblr_version << '|' << envelope.sblr_operation << '|'
      << envelope.opcode_value << '|' << envelope.binding_epoch << '|'
      << envelope.bound_root_uuid << '|' << envelope.descriptor_digest << '|'
      << envelope.payload_class << '|' << (envelope.contains_raw_sql_text ? '1' : '0') << '|'
      << (envelope.cluster_authority_present ? '1' : '0');
  return out.str();
}

std::optional<SblrEnvelope> DecodeEnvelopeForProbe(std::string_view encoded, std::vector<std::string>* errors) {
  const auto parts = Split(encoded, '|');
  if (parts.size() != 10 || parts[0] != "SBLR3") {
    if (errors) {
      errors->push_back("invalid SBLR probe envelope");
    }
    return std::nullopt;
  }
  SblrEnvelope envelope;
  std::uint32_t version = 0;
  auto version_result = std::from_chars(parts[1].data(), parts[1].data() + parts[1].size(), version, 10);
  if (version_result.ec != std::errc() || version_result.ptr != parts[1].data() + parts[1].size()) {
    if (errors) {
      errors->push_back("invalid SBLR version");
    }
    return std::nullopt;
  }
  const auto opcode = ParseOpcodeValue(parts[3]);
  if (!opcode) {
    if (errors) {
      errors->push_back("invalid opcode value");
    }
    return std::nullopt;
  }
  envelope.sblr_version = static_cast<std::uint16_t>(version);
  envelope.sblr_operation = std::string(parts[2]);
  envelope.opcode_value = *opcode;
  envelope.binding_epoch = std::string(parts[4]);
  envelope.bound_root_uuid = std::string(parts[5]);
  envelope.descriptor_digest = std::string(parts[6]);
  envelope.payload_class = std::string(parts[7]);
  envelope.contains_raw_sql_text = parts[8] == "1";
  envelope.cluster_authority_present = parts[9] == "1";
  return envelope;
}

}  // namespace scratchbird::parser::sbsql_v3_sblr
