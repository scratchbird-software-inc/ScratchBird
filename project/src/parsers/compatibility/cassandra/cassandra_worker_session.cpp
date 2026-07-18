// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cassandra_worker_session.hpp"

#include "cassandra_dialect.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace scratchbird::parser::cassandra {
namespace {

constexpr std::uint8_t kOpcodeError = 0x00;
constexpr std::uint8_t kOpcodeStartup = 0x01;
constexpr std::uint8_t kOpcodeReady = 0x02;
constexpr std::uint8_t kOpcodeOptions = 0x05;
constexpr std::uint8_t kOpcodeSupported = 0x06;
constexpr std::uint8_t kOpcodeQuery = 0x07;
constexpr std::uint8_t kOpcodeResult = 0x08;
constexpr std::uint8_t kOpcodeRegister = 0x0b;

constexpr std::uint32_t kResultVoid = 0x0001;
constexpr std::uint32_t kResultRows = 0x0002;
constexpr std::uint32_t kRowsGlobalTablesSpec = 0x0001;
constexpr std::uint16_t kTypeVarchar = 0x000d;

struct FrameHeader {
  std::uint8_t version{0};
  std::uint8_t flags{0};
  std::int16_t stream{0};
  std::uint8_t opcode{0};
  std::uint32_t length{0};
};

#ifndef _WIN32
bool ReadExact(int fd, void* out, std::size_t size) {
  auto* bytes = static_cast<std::uint8_t*>(out);
  std::size_t read_total = 0;
  while (read_total < size) {
    const auto rc = ::read(fd, bytes + read_total, size - read_total);
    if (rc > 0) {
      read_total += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

bool WriteAll(int fd, const void* data, std::size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::size_t written = 0;
  while (written < size) {
    const auto rc = ::write(fd, bytes + written, size - written);
    if (rc > 0) {
      written += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}
#endif

void AppendU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
  out->push_back(static_cast<std::uint8_t>(value & 0xff));
}

void AppendI16(std::vector<std::uint8_t>* out, std::int16_t value) {
  AppendU16(out, static_cast<std::uint16_t>(value));
}

void AppendU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  out->push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
  out->push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
  out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
  out->push_back(static_cast<std::uint8_t>(value & 0xff));
}

std::uint32_t ReadU32(const std::uint8_t* data) {
  return (static_cast<std::uint32_t>(data[0]) << 24) |
         (static_cast<std::uint32_t>(data[1]) << 16) |
         (static_cast<std::uint32_t>(data[2]) << 8) |
         static_cast<std::uint32_t>(data[3]);
}

void AppendString(std::vector<std::uint8_t>* out, std::string_view value) {
  AppendU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

void AppendStringList(std::vector<std::uint8_t>* out, std::span<const std::string_view> values) {
  AppendU16(out, static_cast<std::uint16_t>(values.size()));
  for (const auto value : values) AppendString(out, value);
}

void AppendStringMultimap(std::vector<std::uint8_t>* out) {
  AppendU16(out, 2);
  AppendString(out, "CQL_VERSION");
  AppendStringList(out, std::array<std::string_view, 1>{"3.4.6"});
  AppendString(out, "COMPRESSION");
  AppendStringList(out, std::array<std::string_view, 1>{""});
}

void AppendBytes(std::vector<std::uint8_t>* out, std::string_view value) {
  AppendU32(out, static_cast<std::uint32_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

void AppendColumnSpec(std::vector<std::uint8_t>* out, std::string_view name) {
  AppendString(out, name);
  AppendU16(out, kTypeVarchar);
}

std::string ToUpper(std::string_view value) {
  std::string out(value);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return out;
}

std::string TrimSemicolon(std::string_view query) {
  std::string out = TrimAscii(query);
  while (!out.empty() && out.back() == ';') out.pop_back();
  return TrimAscii(out);
}

std::string QueryText(const std::vector<std::uint8_t>& body) {
  if (body.size() < 4) return {};
  const auto size = ReadU32(body.data());
  if (size > body.size() - 4) return {};
  return std::string(reinterpret_cast<const char*>(body.data() + 4), size);
}

std::string ExtractDiagnosticMessage(std::string_view json) {
  const std::string marker = "\"message\":\"";
  const auto pos = json.find(marker);
  if (pos == std::string_view::npos) return "Cassandra compatibility parser refused the query";
  const auto start = pos + marker.size();
  const auto end = json.find('"', start);
  if (end == std::string_view::npos) return std::string(json.substr(start));
  return std::string(json.substr(start, end - start));
}

std::vector<std::uint8_t> RowsResult(std::string_view keyspace,
                                     std::string_view table,
                                     std::span<const std::string_view> columns,
                                     std::span<const std::vector<std::string_view>> rows) {
  std::vector<std::uint8_t> body;
  AppendU32(&body, kResultRows);
  AppendU32(&body, kRowsGlobalTablesSpec);
  AppendU32(&body, static_cast<std::uint32_t>(columns.size()));
  AppendString(&body, keyspace);
  AppendString(&body, table);
  for (const auto column : columns) AppendColumnSpec(&body, column);
  AppendU32(&body, static_cast<std::uint32_t>(rows.size()));
  for (const auto& row : rows) {
    for (const auto value : row) AppendBytes(&body, value);
  }
  return body;
}

std::vector<std::uint8_t> SystemLocalResult() {
  static constexpr std::array<std::string_view, 9> kColumns{
      "key", "cluster_name", "data_center", "rack", "release_version",
      "cql_version", "native_protocol_version", "partitioner", "host_id"};
  const std::vector<std::string_view> row{
      "local", "ScratchBirdReference", "datacenter1", "rack1", "5.0.8-scratchbird-reference",
      "3.4.6", "5", "org.apache.cassandra.dht.Murmur3Partitioner",
      "00000000-0000-7000-8000-000000000001"};
  const std::array<std::vector<std::string_view>, 1> rows{row};
  return RowsResult("system", "local", kColumns, rows);
}

std::vector<std::uint8_t> SystemPeersResult() {
  static constexpr std::array<std::string_view, 3> kColumns{"peer", "data_center", "rack"};
  const std::array<std::vector<std::string_view>, 0> rows{};
  return RowsResult("system", "peers_v2", kColumns, rows);
}

std::vector<std::uint8_t> ProbeResult() {
  static constexpr std::array<std::string_view, 1> kColumns{"sb_reference_probe"};
  const std::vector<std::string_view> row{"1"};
  const std::array<std::vector<std::string_view>, 1> rows{row};
  return RowsResult("system", "local", kColumns, rows);
}

std::vector<std::uint8_t> KeyspacesResult() {
  static constexpr std::array<std::string_view, 3> kColumns{
      "keyspace_name", "durable_writes", "replication"};
  const std::vector<std::string_view> system_row{
      "system", "true", "{'class':'org.apache.cassandra.locator.LocalStrategy'}"};
  const std::vector<std::string_view> schema_row{
      "system_schema", "true", "{'class':'org.apache.cassandra.locator.LocalStrategy'}"};
  const std::array<std::vector<std::string_view>, 2> rows{system_row, schema_row};
  return RowsResult("system_schema", "keyspaces", kColumns, rows);
}

std::vector<std::uint8_t> LegacyKeyspacesResult() {
  static constexpr std::array<std::string_view, 4> kColumns{
      "keyspace_name", "durable_writes", "strategy_class", "strategy_options"};
  const std::vector<std::string_view> system_row{
      "system", "true", "org.apache.cassandra.locator.LocalStrategy", "{}"};
  const std::vector<std::string_view> schema_row{
      "system_schema", "true", "org.apache.cassandra.locator.LocalStrategy", "{}"};
  const std::array<std::vector<std::string_view>, 2> rows{system_row, schema_row};
  return RowsResult("system", "schema_keyspaces", kColumns, rows);
}

std::vector<std::uint8_t> EmptySchemaResult(std::string_view table,
                                            std::span<const std::string_view> columns) {
  const std::array<std::vector<std::string_view>, 0> rows{};
  return RowsResult("system_schema", table, columns, rows);
}

std::vector<std::uint8_t> EmptyLegacyResult(std::string_view table,
                                            std::span<const std::string_view> columns) {
  const std::array<std::vector<std::string_view>, 0> rows{};
  return RowsResult("system", table, columns, rows);
}

std::vector<std::uint8_t> EmptyVirtualResult(std::string_view table,
                                             std::span<const std::string_view> columns) {
  const std::array<std::vector<std::string_view>, 0> rows{};
  return RowsResult("system_virtual_schema", table, columns, rows);
}

std::vector<std::uint8_t> TablesResult() {
  static constexpr std::array<std::string_view, 9> kColumns{
      "keyspace_name", "table_name", "bloom_filter_fp_chance", "caching",
      "comment", "compaction", "compression", "crc_check_chance", "default_time_to_live"};
  const std::vector<std::string_view> local_row{
      "system", "local", "0.01", "{}", "ScratchBird reference Cassandra local metadata projection",
      "{}", "{}", "1.0", "0"};
  const std::vector<std::string_view> peers_row{
      "system", "peers", "0.01", "{}", "ScratchBird reference Cassandra peers projection",
      "{}", "{}", "1.0", "0"};
  const std::vector<std::string_view> peers_v2_row{
      "system", "peers_v2", "0.01", "{}", "ScratchBird reference Cassandra peers_v2 projection",
      "{}", "{}", "1.0", "0"};
  const std::array<std::vector<std::string_view>, 3> rows{local_row, peers_row, peers_v2_row};
  return RowsResult("system_schema", "tables", kColumns, rows);
}

std::vector<std::uint8_t> ColumnsResult() {
  static constexpr std::array<std::string_view, 7> kColumns{
      "keyspace_name", "table_name", "column_name", "clustering_order",
      "kind", "position", "type"};
  const std::vector<std::vector<std::string_view>> rows{
      {"system", "local", "key", "none", "partition_key", "0", "text"},
      {"system", "local", "cluster_name", "none", "regular", "-1", "text"},
      {"system", "local", "data_center", "none", "regular", "-1", "text"},
      {"system", "local", "rack", "none", "regular", "-1", "text"},
      {"system", "local", "release_version", "none", "regular", "-1", "text"},
      {"system", "local", "cql_version", "none", "regular", "-1", "text"},
      {"system", "local", "native_protocol_version", "none", "regular", "-1", "text"},
      {"system", "local", "partitioner", "none", "regular", "-1", "text"},
      {"system", "local", "host_id", "none", "regular", "-1", "uuid"},
      {"system", "peers", "peer", "none", "partition_key", "0", "inet"},
      {"system", "peers", "data_center", "none", "regular", "-1", "text"},
      {"system", "peers", "rack", "none", "regular", "-1", "text"},
      {"system", "peers_v2", "peer", "none", "partition_key", "0", "inet"},
      {"system", "peers_v2", "data_center", "none", "regular", "-1", "text"},
      {"system", "peers_v2", "rack", "none", "regular", "-1", "text"}};
  return RowsResult("system_schema", "columns", kColumns, rows);
}

std::vector<std::uint8_t> IndexesResult() {
  static constexpr std::array<std::string_view, 5> kColumns{
      "keyspace_name", "table_name", "index_name", "kind", "options"};
  return EmptySchemaResult("indexes", kColumns);
}

std::vector<std::uint8_t> ViewsResult() {
  static constexpr std::array<std::string_view, 5> kColumns{
      "keyspace_name", "view_name", "base_table_name", "where_clause", "include_all_columns"};
  return EmptySchemaResult("views", kColumns);
}

std::vector<std::uint8_t> TypesResult() {
  static constexpr std::array<std::string_view, 4> kColumns{
      "keyspace_name", "type_name", "field_names", "field_types"};
  return EmptySchemaResult("types", kColumns);
}

std::vector<std::uint8_t> FunctionsResult() {
  static constexpr std::array<std::string_view, 8> kColumns{
      "keyspace_name", "function_name", "argument_names", "argument_types",
      "body", "called_on_null_input", "language", "return_type"};
  return EmptySchemaResult("functions", kColumns);
}

std::vector<std::uint8_t> AggregatesResult() {
  static constexpr std::array<std::string_view, 7> kColumns{
      "keyspace_name", "aggregate_name", "argument_types", "final_func",
      "initcond", "return_type", "state_func"};
  return EmptySchemaResult("aggregates", kColumns);
}

std::vector<std::uint8_t> TriggersResult() {
  static constexpr std::array<std::string_view, 4> kColumns{
      "keyspace_name", "table_name", "trigger_name", "options"};
  return EmptySchemaResult("triggers", kColumns);
}

std::vector<std::uint8_t> DroppedColumnsResult() {
  static constexpr std::array<std::string_view, 5> kColumns{
      "keyspace_name", "table_name", "column_name", "dropped_time", "type"};
  return EmptySchemaResult("dropped_columns", kColumns);
}

std::vector<std::uint8_t> LegacyColumnFamiliesResult() {
  static constexpr std::array<std::string_view, 8> kColumns{
      "keyspace_name", "columnfamily_name", "comment", "read_repair_chance",
      "gc_grace_seconds", "bloom_filter_fp_chance", "caching", "compression_parameters"};
  return EmptyLegacyResult("schema_columnfamilies", kColumns);
}

std::vector<std::uint8_t> LegacyColumnsResult() {
  static constexpr std::array<std::string_view, 8> kColumns{
      "keyspace_name", "columnfamily_name", "column_name", "component_index",
      "index_name", "index_options", "index_type", "type"};
  return EmptyLegacyResult("schema_columns", kColumns);
}

std::vector<std::uint8_t> LegacyTriggersResult() {
  static constexpr std::array<std::string_view, 4> kColumns{
      "keyspace_name", "columnfamily_name", "trigger_name", "trigger_options"};
  return EmptyLegacyResult("schema_triggers", kColumns);
}

std::vector<std::uint8_t> LegacyTypesResult() {
  static constexpr std::array<std::string_view, 4> kColumns{
      "keyspace_name", "type_name", "field_names", "field_types"};
  return EmptyLegacyResult("schema_usertypes", kColumns);
}

std::vector<std::uint8_t> LegacyFunctionsResult() {
  static constexpr std::array<std::string_view, 8> kColumns{
      "keyspace_name", "function_name", "signature", "argument_names",
      "body", "called_on_null_input", "language", "return_type"};
  return EmptyLegacyResult("schema_functions", kColumns);
}

std::vector<std::uint8_t> LegacyAggregatesResult() {
  static constexpr std::array<std::string_view, 8> kColumns{
      "keyspace_name", "aggregate_name", "signature", "state_func",
      "state_type", "final_func", "initcond", "return_type"};
  return EmptyLegacyResult("schema_aggregates", kColumns);
}

std::vector<std::uint8_t> VirtualKeyspacesResult() {
  static constexpr std::array<std::string_view, 2> kColumns{"keyspace_name", "comment"};
  return EmptyVirtualResult("keyspaces", kColumns);
}

std::vector<std::uint8_t> VirtualTablesResult() {
  static constexpr std::array<std::string_view, 5> kColumns{
      "keyspace_name", "table_name", "comment", "kind", "partitioner"};
  return EmptyVirtualResult("tables", kColumns);
}

std::vector<std::uint8_t> VirtualColumnsResult() {
  static constexpr std::array<std::string_view, 6> kColumns{
      "keyspace_name", "table_name", "column_name", "kind", "position", "type"};
  return EmptyVirtualResult("columns", kColumns);
}

std::vector<std::uint8_t> VoidResult() {
  std::vector<std::uint8_t> body;
  AppendU32(&body, kResultVoid);
  return body;
}

#ifndef _WIN32
bool ReadHeader(int fd, FrameHeader* header) {
  std::uint8_t bytes[9] = {};
  if (!ReadExact(fd, bytes, sizeof(bytes))) return false;
  header->version = bytes[0];
  header->flags = bytes[1];
  header->stream = static_cast<std::int16_t>((static_cast<std::uint16_t>(bytes[2]) << 8) |
                                             static_cast<std::uint16_t>(bytes[3]));
  header->opcode = bytes[4];
  header->length = ReadU32(bytes + 5);
  return header->length <= 16 * 1024 * 1024;
}

bool SendFrame(int fd, const FrameHeader& request, std::uint8_t opcode,
               const std::vector<std::uint8_t>& body) {
  std::vector<std::uint8_t> out;
  out.reserve(9 + body.size());
  out.push_back(static_cast<std::uint8_t>(request.version | 0x80));
  out.push_back(0);
  AppendI16(&out, request.stream);
  out.push_back(opcode);
  AppendU32(&out, static_cast<std::uint32_t>(body.size()));
  out.insert(out.end(), body.begin(), body.end());
  return WriteAll(fd, out.data(), out.size());
}

bool SendReady(int fd, const FrameHeader& request) {
  return SendFrame(fd, request, kOpcodeReady, {});
}

bool SendSupported(int fd, const FrameHeader& request) {
  std::vector<std::uint8_t> body;
  AppendStringMultimap(&body);
  return SendFrame(fd, request, kOpcodeSupported, body);
}

bool SendError(int fd, const FrameHeader& request, std::string_view message) {
  std::vector<std::uint8_t> body;
  AppendU32(&body, 0);
  AppendString(&body, message);
  return SendFrame(fd, request, kOpcodeError, body);
}

bool HandleQuery(int fd, const FrameHeader& request, const std::string& query) {
  const auto trimmed = TrimSemicolon(query);
  const auto upper = ToUpper(trimmed);
  const auto parsed = ParseStatement(trimmed);
  if (!parsed.ok) {
    return SendError(fd, request, ExtractDiagnosticMessage(parsed.message_vector_json));
  }
  if (upper.find("SYSTEM.PEERS") != std::string::npos) {
    return SendFrame(fd, request, kOpcodeResult, SystemPeersResult());
  }
  if (upper.find("SYSTEM.LOCAL") != std::string::npos) {
    return SendFrame(fd, request, kOpcodeResult, SystemLocalResult());
  }
  if (upper.find("SYSTEM.SCHEMA_KEYSPACES") != std::string::npos) {
    return SendFrame(fd, request, kOpcodeResult, LegacyKeyspacesResult());
  }
  if (upper.find("SYSTEM.SCHEMA_COLUMNFAMILIES") != std::string::npos) {
    return SendFrame(fd, request, kOpcodeResult, LegacyColumnFamiliesResult());
  }
  if (upper.find("SYSTEM.SCHEMA_COLUMNS") != std::string::npos) {
    return SendFrame(fd, request, kOpcodeResult, LegacyColumnsResult());
  }
  if (upper.find("SYSTEM.SCHEMA_TRIGGERS") != std::string::npos) {
    return SendFrame(fd, request, kOpcodeResult, LegacyTriggersResult());
  }
  if (upper.find("SYSTEM.SCHEMA_USERTYPES") != std::string::npos) {
    return SendFrame(fd, request, kOpcodeResult, LegacyTypesResult());
  }
  if (upper.find("SYSTEM.SCHEMA_FUNCTIONS") != std::string::npos) {
    return SendFrame(fd, request, kOpcodeResult, LegacyFunctionsResult());
  }
  if (upper.find("SYSTEM.SCHEMA_AGGREGATES") != std::string::npos) {
    return SendFrame(fd, request, kOpcodeResult, LegacyAggregatesResult());
  }
  if (upper.find("SYSTEM_VIRTUAL_SCHEMA.KEYSPACES") != std::string::npos) {
    return SendFrame(fd, request, kOpcodeResult, VirtualKeyspacesResult());
  }
  if (upper.find("SYSTEM_VIRTUAL_SCHEMA.TABLES") != std::string::npos) {
    return SendFrame(fd, request, kOpcodeResult, VirtualTablesResult());
  }
  if (upper.find("SYSTEM_VIRTUAL_SCHEMA.COLUMNS") != std::string::npos) {
    return SendFrame(fd, request, kOpcodeResult, VirtualColumnsResult());
  }
  if (upper.find("SYSTEM_SCHEMA.KEYSPACES") != std::string::npos ||
      upper.find("DESCRIBE KEYSPACES") != std::string::npos) {
    return SendFrame(fd, request, kOpcodeResult, KeyspacesResult());
  }
  if (upper.find("SYSTEM_SCHEMA.TABLES") != std::string::npos) {
    return SendFrame(fd, request, kOpcodeResult, TablesResult());
  }
  if (upper.find("SYSTEM_SCHEMA.COLUMNS") != std::string::npos) {
    return SendFrame(fd, request, kOpcodeResult, ColumnsResult());
  }
  if (upper.find("SYSTEM_SCHEMA.INDEXES") != std::string::npos) {
    return SendFrame(fd, request, kOpcodeResult, IndexesResult());
  }
  if (upper.find("SYSTEM_SCHEMA.VIEWS") != std::string::npos) {
    return SendFrame(fd, request, kOpcodeResult, ViewsResult());
  }
  if (upper.find("SYSTEM_SCHEMA.TYPES") != std::string::npos) {
    return SendFrame(fd, request, kOpcodeResult, TypesResult());
  }
  if (upper.find("SYSTEM_SCHEMA.FUNCTIONS") != std::string::npos) {
    return SendFrame(fd, request, kOpcodeResult, FunctionsResult());
  }
  if (upper.find("SYSTEM_SCHEMA.AGGREGATES") != std::string::npos) {
    return SendFrame(fd, request, kOpcodeResult, AggregatesResult());
  }
  if (upper.find("SYSTEM_SCHEMA.TRIGGERS") != std::string::npos) {
    return SendFrame(fd, request, kOpcodeResult, TriggersResult());
  }
  if (upper.find("SYSTEM_SCHEMA.DROPPED_COLUMNS") != std::string::npos) {
    return SendFrame(fd, request, kOpcodeResult, DroppedColumnsResult());
  }
  if (upper.starts_with("SELECT")) {
    return SendFrame(fd, request, kOpcodeResult, ProbeResult());
  }
  return SendFrame(fd, request, kOpcodeResult, VoidResult());
}
#endif

} // namespace

int ServeCassandraWorkerSession(int fd) {
#ifdef _WIN32
  (void)fd;
  return 1;
#else
  for (;;) {
    FrameHeader header;
    if (!ReadHeader(fd, &header)) return 0;
    std::vector<std::uint8_t> body(header.length);
    if (!body.empty() && !ReadExact(fd, body.data(), body.size())) return 1;
    switch (header.opcode) {
      case kOpcodeOptions:
        if (!SendSupported(fd, header)) return 1;
        break;
      case kOpcodeStartup:
      case kOpcodeRegister:
        if (!SendReady(fd, header)) return 1;
        break;
      case kOpcodeQuery:
        if (!HandleQuery(fd, header, QueryText(body))) return 1;
        break;
      default:
        if (!SendError(fd, header, "unsupported Cassandra native protocol opcode")) return 1;
        break;
    }
  }
#endif
}

} // namespace scratchbird::parser::cassandra
