// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "canonical_sblr_admission_test_helper.hpp"
#include "engine/sblr/sblr_opcode_stream.hpp"
#include "server/sblr_local_gateway.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace sblr = scratchbird::engine::sblr;
namespace server = scratchbird::server;
namespace {
unsigned checks = 0, failures = 0;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) { ++failures; std::cerr << message << '\n'; }
}
void Text(sblr::SblrOperationEnvelope& root, std::string name, std::string value) {
  sblr::SblrOperand operand;
  operand.ordinal = root.operands.size() + 1;
  operand.type = "text";
  operand.name = std::move(name);
  operand.value_kind = sblr::SblrValueKind::literal_typed;
  operand.value_body.assign(root.registry_snapshot_uuid.bytes.begin(),
                            root.registry_snapshot_uuid.bytes.end());
  for (unsigned i = 0; i < 8; ++i)
    operand.value_body.push_back(static_cast<std::uint64_t>(value.size()) >> (8 * i));
  operand.value_body.insert(operand.value_body.end(), value.begin(), value.end());
  root.operands.push_back(std::move(operand));
}
sblr::SblrOperationEnvelope Root(bool bulk, bool packet, bool omitted) {
  auto root = scratchbird::test::sbsql::BuildCanonicalEngineSblrEnvelopeForTest(
      bulk ? "dml.execute_native_bulk_ingest" : "dml.insert_rows",
      bulk ? "SBLR_DML_EXECUTE_NATIVE_BULK_INGEST" : "SBLR_DML_INSERT_ROWS",
      "native.dml.gateway.identity");
  sblr::SblrOperand target;
  target.ordinal = 1;
  target.type = "uuid";
  target.name = "target_object_uuid";
  target.value_kind = sblr::SblrValueKind::uuid_ref;
  target.value_body.assign(root.parser_package_uuid.bytes.begin(),
                           root.parser_package_uuid.bytes.end());
  root.operands.push_back(std::move(target));
  Text(root, "target_object_kind", "table");
  Text(root, "insert_values_row_count", "1");
  Text(root, "insert_values_column_count", "1");
  Text(root, "physical_mga_cow", "false");
  Text(root, "insert_trace.rows", "false");
  Text(root, "sblr.rowset_default_markers_absent", "true");
  Text(root, "insert_values_column_list_present", bulk || omitted ? "false" : "true");
  Text(root, "insert_values_parser_executes_sql", "false");
  Text(root, "sblr.canonical_rowset_shared_shape", "true");
  if (packet) {
    Text(root, "native_row_packet_required", "true");
    Text(root, "native_row_packet_format", "scratchbird.native_rows.v2");
  } else {
    Text(root, "insert_values_compact_format", "sbsql.insert_values.cells.v1");
    // One non-null INT64 cell: column id, value 1, in the frozen compact codec.
    Text(root, "insert_values_compact_payload", "6964|696e743634|31|0");
  }
  if (bulk) {
    Text(root, "native_bulk_ingest", "true");
    Text(root, "native_bulk_ingest_enabled", "true");
    Text(root, "insert_values_descriptor_column_0", "id");
  } else if (omitted) Text(root, "insert_values_descriptor_column_0", "id");
  else Text(root, "sblr.fast_insert_values_lowering", "true");
  return root;
}
server::LocalSblrGatewayDecision Admit(const sblr::SblrOperationEnvelope& root,
                                      unsigned flags = 0) {
  sblr::SblrOpcodeStream stream;
  stream.package_descriptor_uuid = root.parser_package_uuid;
  stream.registry_snapshot_uuid = root.registry_snapshot_uuid;
  const auto frame = [&](bool begin) {
    auto value = scratchbird::test::sbsql::BuildCanonicalEngineSblrEnvelopeForTest(
        begin ? "engine.op.package_begin" : "engine.op.package_end",
        begin ? "SBLR_PACKAGE_BEGIN" : "SBLR_PACKAGE_END", "native.dml.gateway");
    sblr::SblrOperand operand;
    operand.ordinal = 1;
    operand.type = begin ? "package.header" : "package.footer";
    operand.name = "package_descriptor";
    operand.value_kind = sblr::SblrValueKind::descriptor_ref;
    operand.value_body.assign(stream.package_descriptor_uuid.bytes.begin(),
                              stream.package_descriptor_uuid.bytes.end());
    value.operands.push_back(std::move(operand));
    return value;
  };
  stream.operations = {frame(true), root, frame(false)};
  server::LocalSblrGatewayRequest request;
  request.canonical_sbos = sblr::EncodeSblrOpcodeStream(stream);
  request.root_operation_id = root.operation_id;
  request.root_opcode = root.opcode;
  request.root_opcode_code = root.opcode_code;
  request.route_snapshot_uuid = root.registry_snapshot_uuid;
  request.security_snapshot_uuid = root.parser_package_uuid;
  request.route_epoch = request.route_generation = request.security_epoch =
      request.security_observation_generation = 1;
  request.route_snapshot_engine_owned = request.security_snapshot_engine_owned = true;
  request.cluster_context_active = flags & 1;
  request.cluster_transaction_active = flags & 2;
  request.route_fence_present = flags & 4;
  return server::AdmitLocalNoClusterSblrGateway(request);
}
}
int main() {
  for (unsigned profile = 0; profile != 4; ++profile) {
    const auto root = Root(profile == 1 || profile == 2, profile == 2, profile == 3);
    Check(Admit(root).ok, "binary table identity did not pass DML gateway");
    for (unsigned flags = 1; flags != 8; ++flags)
      Check(!Admit(root, flags).ok, "cluster-owned DML fell through locally");
    for (unsigned mutation = 0; mutation != 12; ++mutation) {
      auto invalid = root;
      auto& target = invalid.operands.front();
      switch (mutation) {
        case 0: target.value_body.clear(); break;
        case 1: target.value_body.resize(15); break;
        case 2: target.value_body.push_back(0); break;
        case 3: std::fill(target.value_body.begin(), target.value_body.end(), 0); break;
        case 4: target.value_body[6] = 0x40; break;
        case 5: target.value_body[8] = 0; break;
        case 6: target.value_flags = 1; break;
        case 7: target.ordinal = 2; break;
        case 8: {
          const auto duplicate = target;
          invalid.operands.push_back(duplicate);
          invalid.operands.back().ordinal = invalid.operands.size();
          break;
        }
        case 9: {
          invalid.operands.erase(invalid.operands.begin());
          for (unsigned i = 0; i < invalid.operands.size(); ++i)
            invalid.operands[i].ordinal = i + 1;
          Text(invalid, "target_object_uuid", "01900000-0000-7000-8000-000000000001");
          break;
        }
        case 10: Text(invalid, "unexpected", "value"); break;
        case 11: target.type = "uuid_ref"; break;
      }
      Check(!Admit(invalid).ok, "malformed or textual DML identity passed gateway");
    }
  }
  std::cout << "native_dml_gateway_identity_checks=" << checks
            << " failures=" << failures << " parser_ipc_e2e=false\n";
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
