// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Actual engine session, receipt, literal binding and admitted SBLR dispatch.
// This is public-ABI coverage, not ordinary SQL/IPC/native-wire conformance.
#define main RetainedResultPageFixtureMain
#include "ia05_result_page_cancellation_fault_test.cpp"
#undef main
#include "wire/typed_result_transport_codec.hpp"
#include <new>
#include <thread>

namespace descriptor_fault {
thread_local long remaining = -1;
thread_local unsigned observed = 0;
}
void* operator new(std::size_t size) {
  if (descriptor_fault::remaining == 0) {
    ++descriptor_fault::observed;
    throw std::bad_alloc();
  }
  if (descriptor_fault::remaining > 0) --descriptor_fault::remaining;
  if (auto* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
// ASan supplies its own nothrow allocator; route it through this fixture's
// matching malloc/free family instead of mixing allocator families at delete.
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try { return ::operator new(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  try { return ::operator new[](size); } catch (...) { return nullptr; }
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {
std::atomic<unsigned> descriptor_checks{0};
std::size_t expected_columns = 1;
void CheckDescriptor(bool ok, const char* detail) {
  ++descriptor_checks;
  Require(ok, detail);
}
sb_engine_result_descriptor_view_v1_t DescriptorView() {
  sb_engine_result_descriptor_view_v1_t out{};
  out.struct_size = sizeof(out);
  out.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  return out;
}
void VerifyDescriptor(sb_engine_result_t result,
                      const api::EngineRequestContext& receipt,
                      const bridge::StatementQueryExecuteResultHandleView& handle,
                      std::vector<std::uint8_t>* exact) {
  auto out = DescriptorView();
  CheckDescriptor(sb_engine_result_descriptor_v1(result, &out) == SB_ENGINE_STATUS_OK,
                  "real canonical query descriptor is not available through public ABI");
  const auto* bytes = out.result_descriptor_vector.bytes;
  std::vector<std::uint8_t> packet(bytes, bytes + out.result_descriptor_vector.size_bytes);
  CheckDescriptor(packet.size() > 128 &&
                      std::equal(packet.begin(), packet.begin() + 8, "SBTRDS01"),
                  "descriptor is not the canonical independent schema packet");
  const auto decoded = scratchbird::wire::DecodeTypedResultRowDescriptor(packet);
  CheckDescriptor(decoded.ok(), "public descriptor cannot be decoded canonically");
  const auto& schema = decoded.descriptor;
  CheckDescriptor(schema.descriptor_uuid == RawUuid(handle.row_descriptor_uuid) &&
                      std::equal(schema.descriptor_uuid.begin(), schema.descriptor_uuid.end(),
                                 out.row_descriptor_uuid.bytes) &&
                      schema.descriptor_generation == out.row_descriptor_generation &&
                      schema.descriptor_generation != 0,
                  "public descriptor substituted the engine result shape identity");
  CheckDescriptor(schema.datatype_catalog_snapshot_uuid ==
                      RawUuid(receipt.datatype_catalog_snapshot_uuid.canonical) &&
                      schema.datatype_catalog_generation == receipt.datatype_catalog_generation &&
                      schema.datatype_registry_generation == receipt.datatype_registry_generation,
                  "public descriptor lost its actual datatype catalog cohort");
  CheckDescriptor(schema.columns.size() == expected_columns && schema.columns[0].name == "value" &&
                      schema.columns[0].ordinal == 0 && schema.columns[0].name_occurrence == 0,
                  "public descriptor lost the bound output occurrence");
  if (expected_columns == 2)
    CheckDescriptor(schema.columns[1].name == "value" && schema.columns[1].ordinal == 1 &&
                        schema.columns[1].name_occurrence == 1 &&
                        schema.columns[1].descriptor_uuid == schema.columns[0].descriptor_uuid,
                    "public descriptor merged or renamed repeated output occurrences");
  // The fixture explicitly negotiates the Core INT64 literal profile.
  CheckDescriptor(schema.columns[0].canonical_value_bytes == 8 &&
                      schema.columns[0].nullability ==
                          scratchbird::wire::TypedResultNullability::not_null,
                  "public descriptor changed literal width or bound nullability");
  CheckDescriptor(std::equal(schema.descriptor_evidence_sha256.begin(),
                             schema.descriptor_evidence_sha256.end(),
                             out.descriptor_evidence_sha256),
                  "public descriptor evidence differs from its canonical bytes");
  if (exact->empty()) *exact = packet;
  else CheckDescriptor(*exact == packet, "immutable public result schema changed");
}

// An independent oracle for the CURRENT legacy carrier, not approval of that
// carrier as the typed wire contract. Schema is still checked above separately.
void VerifyBatchBudget(sb_engine_result_t result, bool faults_only) {
  auto payload = [&] {
    sb_engine_string_view_t view{};
    CheckDescriptor(sb_engine_result_payload(result, &view) == SB_ENGINE_STATUS_OK,
                    "batch payload unavailable");
    return std::string(view.data ? view.data : "", view.size_bytes);
  };
  const auto initial = payload();
  std::string prefix, evidence;
  std::vector<std::string> rows, metadata;
  std::istringstream lines(initial);
  for (std::string line; std::getline(lines, line);) {
    if (line.starts_with("operation_id=") || line.starts_with("result_kind="))
      prefix += line + '\n';
    else if (line.starts_with("evidence=")) evidence += line + '\n';
    else if (line.starts_with("row[")) rows.push_back(line);
    else if (line.starts_with("row_meta[")) metadata.push_back(line);
  }
  CheckDescriptor(rows.size() == 24 && metadata.size() == 24 && !evidence.empty(),
                  "batch fixture lacks actual rows, metadata or evidence");
  for (unsigned i = 0; i != 24; ++i) {
    CheckDescriptor(rows[i] == "row[" + std::to_string(i) + "]=value=1",
                    "actual bounded VALUES query returned a false row");
    CheckDescriptor(metadata[i] == "row_meta[" + std::to_string(i) +
                        "]=value:int64:not_null",
                    "actual bounded VALUES query lost its row metadata");
  }
  auto expected = [&](unsigned first, unsigned count) {
    std::string bytes = prefix + "row_count=" + std::to_string(count) + '\n';
    for (unsigned i = first; i < first + count; ++i)
      bytes += rows[i] + '\n' + metadata[i] + '\n';
    return bytes + evidence;
  };
  sb_engine_batch_request_v1_t request{};
  request.struct_size = sizeof(request);
  request.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  request.max_rows = 1;
  sb_engine_row_batch_view_v1_t output{};
  output.row_count = 777;
  const auto sentinel = output;
  request.reserved1 = 1;
  CheckDescriptor(sb_engine_result_next_batch(result, &request, &output) ==
                      SB_ENGINE_STATUS_INVALID_ARGUMENT &&
                      std::memcmp(&output, &sentinel, sizeof(output)) == 0 && payload() == initial,
                  "invalid request changed the result or caller output");
  request.reserved1 = 0;
  unsigned faults = 0;
  unsigned first = 0;
  for (unsigned count : {1U, 8U, 10U, 5U}) {
    const auto before = payload();
    sb_engine_string_view_t borrowed{};
    CheckDescriptor(sb_engine_result_payload(result, &borrowed) == SB_ENGINE_STATUS_OK,
                    "cannot retain the pre-pull borrowed payload");
    const auto intact = [&] {
      sb_engine_string_view_t current{};
      return sb_engine_result_payload(result, &current) == SB_ENGINE_STATUS_OK &&
          current.data == borrowed.data && current.size_bytes == borrowed.size_bytes &&
          std::string_view(current.data, current.size_bytes) == before;
    };
    const auto exact = expected(first, count);
    // Preserve the existing zero/default and saturating-row-limit semantics.
    request.max_rows = count == 5 ? (faults_only ? UINT64_MAX : 0) : count;
    if (!faults_only) {
      // Include the whole envelope, evidence, metadata and decimal ordinals.
      // Even if one row fits, a failed multirow request must publish no prefix.
      for (std::uint64_t limit : {std::uint64_t{1}, std::uint64_t(exact.size() - 1)}) {
        request.max_bytes = limit;
        output = sentinel;
        CheckDescriptor(sb_engine_result_next_batch(result, &request, &output) ==
                            SB_ENGINE_STATUS_RESOURCE_EXHAUSTED,
                        "byte limit was ignored without allocation pressure");
        CheckDescriptor(std::memcmp(&output, &sentinel, sizeof(output)) == 0 && intact(),
                        "ordinary byte refusal invalidated the borrowed payload or output");
        descriptor_fault::observed = 0;
        descriptor_fault::remaining = 0;
        sb_engine_status_t status = SB_ENGINE_STATUS_INTERNAL_ERROR;
        bool escaped = false;
        try { status = sb_engine_result_next_batch(result, &request, &output); }
        catch (...) { escaped = true; }
        descriptor_fault::remaining = -1;
        CheckDescriptor(!escaped && status == SB_ENGINE_STATUS_RESOURCE_EXHAUSTED,
                        "over-budget batch was admitted or escaped across C ABI");
        CheckDescriptor(descriptor_fault::observed == 0,
                        "over-budget batch allocated before admission");
        CheckDescriptor(std::memcmp(&output, &sentinel, sizeof(output)) == 0 && intact(),
                        "byte refusal changed payload, output or cursor");
      }
    }
    request.max_bytes = count == 5 && faults_only ? 0 : exact.size();
    bool completed = false;
    for (long index = 0; index != 128; ++index) {
      output = sentinel;
      descriptor_fault::observed = 0;
      descriptor_fault::remaining = index;
      sb_engine_status_t status = SB_ENGINE_STATUS_INTERNAL_ERROR;
      bool escaped = false;
      try { status = sb_engine_result_next_batch(result, &request, &output); }
      catch (...) { escaped = true; }
      descriptor_fault::remaining = -1;
      CheckDescriptor(!escaped, "batch allocation exception crossed the C ABI");
      if (descriptor_fault::observed != 0) {
        faults += descriptor_fault::observed;
        CheckDescriptor(status == SB_ENGINE_STATUS_RESOURCE_EXHAUSTED,
                        "batch allocation failure returned false success");
        CheckDescriptor(std::memcmp(&output, &sentinel, sizeof(output)) == 0 && intact(),
                        "allocation refusal published a partial packet or advanced the cursor");
        continue;
      }
      CheckDescriptor(status == SB_ENGINE_STATUS_OK && output.row_count == count &&
                          output.end_of_stream == (first + count == 24) && payload() == exact,
                      "exact-fit retry changed bytes, row position or end state");
      first += count;
      completed = true;
      break;
    }
    CheckDescriptor(completed, "batch allocation sweep failed to reach success");
  }
  CheckDescriptor(faults != 0 && first == 24, "batch fault/row oracle was not exercised");
  request.max_bytes = 1;
  CheckDescriptor(sb_engine_result_next_batch(result, &request, &output) == SB_ENGINE_STATUS_OK &&
                      output.row_count == 0 && output.end_of_stream && payload().empty(),
                  "exhausted result manufactured another packet or exceeded byte bound");
  CheckDescriptor(sb_engine_result_next_batch(result, nullptr, &output) == SB_ENGINE_STATUS_OK &&
                      output.row_count == 0 && output.end_of_stream && payload().empty(),
                  "default request changed EOS state");
  std::cout << "actual batch budget checks=" << descriptor_checks << " faults=" << faults << " PASS\n";
}
}

int main(int argc, char** argv) {
  const std::string_view mode = argc > 1 ? argv[1] : "";
  const bool empty_query = mode == "--empty";
  const bool duplicate_names = mode == "--duplicate";
  const bool batch_budget = mode == "--batch-budget" || mode == "--batch-fault";
  expected_columns = duplicate_names ? 2 : 1;
  auto fixture = CreateFixture();
  PublicSession session(fixture);
  std::atomic<unsigned> probes{0};
  auto context = BeginTransaction(fixture, &probes);
  context.query_cancellation_requested = [&] { ++probes; return false; };
  bridge::StatementContextAcquireRequest acquire;
  acquire.engine_context = &context;
  acquire.exact_transaction_uuid = context.transaction_uuid.canonical;
  bridge::StatementContextReceiptHandle receipt;
  bridge::StatementContextReceiptView view;
  CheckDescriptor(bridge::AcquireStatementContextReceipt(
      session.session, &acquire, &receipt, &view, nullptr) == SB_ENGINE_STATUS_OK,
      "descriptor fixture receipt acquisition failed");
  api::EngineRequestContext admitted_context;
  CheckDescriptor(bridge::CopyStatementContextEngineContextV1(
      receipt, &admitted_context, nullptr) == SB_ENGINE_STATUS_OK,
      "descriptor fixture live datatype context unavailable");
  const auto parser_uuid = Text(NewUuid(platform::UuidKind::object, 36503));
  auto literal = literal_fixture::FinalizeLiteral(receipt, view);
  auto member = ValuesQueryMember(view, parser_uuid, literal, admitted_context);
  if (batch_budget) {
    std::string ids = "1";
    for (unsigned i = 2; i <= 24; ++i) {
      ids += ',' + std::to_string(i);
      member.operands.push_back(TypedQueryOperand(17 + i, "relational_values_row_v1",
                                                  "slot_" + std::to_string(i), "1"));
    }
    member.operands[15] = TypedQueryOperand(16, "relational_node_v1", "slot_1", "13|0|-|1|" + ids);
  }
  if (duplicate_names) {
    member.operands[14] = TypedQueryOperand(15, "relational_values_row_v1", "slot_1", "1,2");
    member.operands[15] = TypedQueryOperand(16, "relational_node_v1", "slot_1", "13|0|-|1,2|1");
    member.operands[16] = TypedQueryOperand(17, "relational_node_binding_v1", "slot_1",
        "76616c7565732e6c69746572616c2d7461626c652e7631|1,2|-|-|-");
    auto second_descriptor = member.operands[11];
    second_descriptor.ordinal = 19;
    second_descriptor.name = "slot_2";
    member.operands.push_back(std::move(second_descriptor));
    member.operands.push_back(TypedQueryOperand(20, "relational_output_v1", "slot_2",
                                               "1|2|2|1|1|76616c7565"));
    member.operands.push_back(TypedQueryOperand(21, "relational_expression_v1", "slot_2",
                                               "7|1|2|-|-|-|-|-"));
  }
  if (empty_query) {
    // One actual VALUES row, LIMIT 1 OFFSET 1: the engine must produce a
    // genuinely empty rowset with its independent one-column schema.
    member.operands[10] = TypedQueryOperand(11, "uint32", "relational_root_node_id", "2");
    member.operands.push_back(TypedQueryOperand(19, "relational_node_v1", "slot_2", "7|0|1|1|-"));
    member.operands.push_back(TypedQueryOperand(20, "relational_node_binding_v1", "slot_2",
        "6c696d69742e626f756e642d636f756e742d6f66667365742e7631|1,2|-|-|-"));
    member.operands.push_back(TypedQueryOperand(21, "relational_expression_v1", "slot_2",
                                               "7|1|1|-|-|-|-|-"));
  }
  const auto submission = PackageWithMember(fixture, view, parser_uuid, std::move(member));
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(submission.stream);
  CheckDescriptor(digest.ok() && literal.sbel.size() == 176,
                  "descriptor fixture literal binding evidence failed");
  std::copy(digest.digest.begin(), digest.digest.end(), literal.sbel.begin() + 144);
  bridge::StatementPackageAdmissionReservationHandle reservation;
  auto dispatch = Admit(fixture, session, view, receipt, parser_uuid, submission, &reservation);
  dispatch.literal_execution_binding = literal.sbel;
  sb_engine_result_t result = nullptr;
  const auto dispatched = bridge::DispatchStatementContextReceipt(&dispatch, &result);
  if (dispatched != SB_ENGINE_STATUS_OK && result) {
    sb_engine_diagnostic_set_view_t diagnostics{};
    (void)sb_engine_result_diagnostics(result, &diagnostics);
    for (std::uint64_t i = 0; i < diagnostics.diagnostic_count; ++i) {
      const auto& d = diagnostics.diagnostics[i];
      std::cerr << std::string_view(d.symbolic_code.data, d.symbolic_code.size_bytes)
                << ':' << std::string_view(d.message_key.data, d.message_key.size_bytes)
                << ':' << std::string_view(d.safe_detail.data, d.safe_detail.size_bytes) << '\n';
    }
  }
  CheckDescriptor(dispatched == SB_ENGINE_STATUS_OK && result,
                  "descriptor fixture canonical query did not execute");
  bridge::StatementQueryExecuteResultHandleView handle;
  CheckDescriptor(bridge::ReadStatementQueryExecuteResultHandle(result, &handle) ==
                      SB_ENGINE_STATUS_OK, "descriptor fixture result handle missing");
  std::vector<std::uint8_t> packet;
  VerifyDescriptor(result, admitted_context, handle, &packet);
  unsigned observed_faults = 0;
  for (long index = 0; index != 128; ++index) {
    auto output = DescriptorView();
    output.row_descriptor_generation = 777;
    const auto before = output;
    descriptor_fault::observed = 0;
    descriptor_fault::remaining = index;
    sb_engine_status_t status = SB_ENGINE_STATUS_INTERNAL_ERROR;
    bool escaped = false;
    try { status = sb_engine_result_descriptor_v1(result, &output); }
    catch (...) { escaped = true; }
    descriptor_fault::remaining = -1;
    CheckDescriptor(!escaped, "descriptor allocation exception crossed the C ABI");
    if (!descriptor_fault::observed) {
      CheckDescriptor(status == SB_ENGINE_STATUS_OK, "descriptor read failed without injected allocation fault");
      break;
    }
    observed_faults += descriptor_fault::observed;
    CheckDescriptor(status == SB_ENGINE_STATUS_RESOURCE_EXHAUSTED,
                    "descriptor pressure returned a false success or stale-authority refusal");
    CheckDescriptor(std::memcmp(&before, &output, sizeof(output)) == 0,
                    "descriptor refusal partially published the output view");
    VerifyDescriptor(result, admitted_context, handle, &packet);
    CheckDescriptor(index != 127, "descriptor allocation sweep did not reach its terminal success");
  }
  std::vector<std::thread> readers;
  for (unsigned i = 0; i != 16; ++i) readers.emplace_back([&] {
    for (unsigned j = 0; j != 8; ++j)
      VerifyDescriptor(result, admitted_context, handle, &packet);
  });
  for (auto& reader : readers) reader.join();
  auto invalid = DescriptorView();
  invalid.struct_size = 1;
  CheckDescriptor(sb_engine_result_descriptor_v1(result, &invalid) ==
                      SB_ENGINE_STATUS_INVALID_ARGUMENT && invalid.struct_size == 1,
                  "invalid public descriptor output was accepted or overwritten");
  CheckDescriptor(sb_engine_result_descriptor_v1(result, nullptr) ==
                      SB_ENGINE_STATUS_INVALID_ARGUMENT,
                  "null descriptor output admitted");
  auto nil = DescriptorView();
  CheckDescriptor(sb_engine_result_descriptor_v1(nullptr, &nil) ==
                      SB_ENGINE_STATUS_INVALID_HANDLE,
                  "null engine result admitted");
  const auto snapshot_uuid = uuid::ParseUuid(view.statement_snapshot_uuid);
  CheckDescriptor(snapshot_uuid.ok(), "retained snapshot identity invalid");
  const auto typed_snapshot = uuid::MakeTypedUuid(platform::UuidKind::object, snapshot_uuid.value);
  CheckDescriptor(typed_snapshot.ok(), "retained snapshot kind invalid");
  const auto snapshot = scratchbird::transaction::mga::ResolvePublishedSnapshotVector(typed_snapshot.value);
  CheckDescriptor(snapshot.ok(), "actual published snapshot unavailable");
  CheckDescriptor(bridge::ReleaseStatementContextReceipt(receipt) == SB_ENGINE_STATUS_OK,
                  "ordinary public receipt retirement failed");
  VerifyDescriptor(result, admitted_context, handle, &packet);
  // Reading schema must not consume rows or make the existing renderer falsely
  // claim typed-batch adoption. This compatibility assertion is not approval of
  // the legacy response transport.
  sb_engine_batch_request_v1_t batch_request{};
  batch_request.struct_size = sizeof(batch_request);
  batch_request.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  batch_request.max_rows = 1;
  batch_request.max_bytes = 1048576;
  sb_engine_row_batch_view_v1_t batch{};
  if (batch_budget) VerifyBatchBudget(result, mode == "--batch-fault");
  CheckDescriptor(sb_engine_result_next_batch(result, &batch_request, &batch) ==
                      SB_ENGINE_STATUS_OK && batch.row_count == (empty_query || batch_budget ? 0U : 1U) && batch.end_of_stream,
                  "descriptor read consumed or changed the actual rowset");
  VerifyDescriptor(result, admitted_context, handle, &packet);
  CheckDescriptor(sb_engine_result_next_batch(result, &batch_request, &batch) ==
                      SB_ENGINE_STATUS_OK && batch.row_count == 0 && batch.end_of_stream,
                  "actual source did not reach EOS");
  VerifyDescriptor(result, admitted_context, handle, &packet);
  // A retired public snapshot is intentionally undiscoverable. Revoke via
  // its actual owning transaction, whose identity was pinned by the receipt.
  scratchbird::transaction::mga::RevokePublishedSnapshotVectorsForTransaction(
      snapshot.descriptor.owning_transaction_uuid, snapshot.descriptor.owning_transaction);
  auto revoked = DescriptorView();
  revoked.row_descriptor_generation = 777;
  const auto before_revoked = revoked;
  CheckDescriptor(sb_engine_result_descriptor_v1(result, &revoked) == SB_ENGINE_STATUS_CONFLICT &&
                      std::memcmp(&before_revoked, &revoked, sizeof(revoked)) == 0,
                  "revoked result snapshot published schema or changed its output");
  CheckDescriptor(sb_engine_result_release(result) == SB_ENGINE_STATUS_OK,
                  "descriptor result owner release failed");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  CheckDescriptor(api::EngineRollbackTransaction(rollback).ok, "fixture rollback failed");
  std::cout << "query public descriptor empty=" << empty_query << " duplicate=" << duplicate_names
            << " checks=" << descriptor_checks << " faults=" << observed_faults << " PASS\n";
}
