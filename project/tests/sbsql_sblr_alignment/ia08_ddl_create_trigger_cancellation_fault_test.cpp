// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// Reuse the real database/session/receipt/admission fixture. No statement
// receipt, trigger binder, opcode-stream gateway, executor registry, catalog,
// or MGA service is mocked.
#define SCRATCHBIRD_IA05_QUERY_EXPLAIN_FIXTURE_ONLY
#include "ia05_query_explain_cancellation_fault_test.cpp"

#include "catalog/name_registry.hpp"
#include "engine/internal_api/mga_relation_store/mga_relation_store.hpp"
#include "engine/sblr/sblr_ddl_create_trigger_runtime.hpp"

#include <fstream>
#include <map>

namespace {

using ArtifactSnapshot =
    std::map<std::string, std::vector<std::uint8_t>>;

struct TriggerTarget {
  platform::TypedUuid schema_uuid;
  platform::TypedUuid relation_uuid;
  api::MgaRelationStorageDescriptor descriptor;
};

ArtifactSnapshot SnapshotFixture(const Fixture& fixture) {
  ArtifactSnapshot snapshot;
  for (const auto& entry :
       std::filesystem::directory_iterator(fixture.directory)) {
    if (!entry.is_regular_file()) continue;
    std::ifstream input(entry.path(), std::ios::binary);
    Require(static_cast<bool>(input),
            "002624 fixture artifact could not be opened");
    snapshot.emplace(
        entry.path().filename().string(),
        std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                  std::istreambuf_iterator<char>()));
  }
  return snapshot;
}

std::string DiagnosticDetail(sb_engine_result_t result) {
  sb_engine_diagnostic_set_view_t diagnostics{};
  if (result == nullptr ||
      sb_engine_result_diagnostics(result, &diagnostics) !=
          SB_ENGINE_STATUS_OK ||
      diagnostics.diagnostic_count != 1) {
    return {};
  }
  return {diagnostics.diagnostics[0].safe_detail.data,
          diagnostics.diagnostics[0].safe_detail.size_bytes};
}

TriggerTarget PrepareTriggerTarget(const Fixture& fixture,
                                   api::EngineRequestContext* context) {
  Require(context != nullptr, "002624 trigger context is required");
  TriggerTarget target{
      NewUuid(platform::UuidKind::schema, 26240),
      NewUuid(platform::UuidKind::object, 26241), {}};
  context->database_page_size_bytes = 16384;
  context->default_root_uuid.canonical = Text(fixture.filespace_uuid);
  context->current_schema_uuid.canonical = Text(target.schema_uuid);
  context->identifier_profile_uuid = "sbsql_v3";
  context->language_context.language_tag = "en";
  context->language_context.default_language_tag = "en";

  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical =
      Text(NewUuid(platform::UuidKind::object, 26242));
  grant.subject_uuid = context->principal_uuid;
  grant.subject_kind = "principal";
  // The fixture principal is the database-local catalog administrator. The
  // empty target is the materialized wildcard used by the security model, so
  // the same frozen grant authorizes the engine-allocated trigger UUID after
  // the database-level bind check.
  grant.target_uuid.canonical.clear();
  grant.right = "CATALOG_MUTATE";
  grant.security_epoch = context->security_epoch;
  context->authorization_context.grants.push_back(std::move(grant));

  api::CrudTableRecord table;
  table.table_uuid = Text(target.relation_uuid);
  table.default_name = "trig_items";
  table.columns = {
      {"item_id",
       "type=int64;datatype_descriptor_uuid="
       "019d0000-0000-7000-8000-00000000d716;type_uuid="
       "019d0000-0000-7000-8000-00000000d717;nullable=false"},
      {"item_price",
       "type=int64;datatype_descriptor_uuid="
       "019d0000-0000-7000-8000-00000000d716;type_uuid="
       "019d0000-0000-7000-8000-00000000d717;nullable=false"}};
  Require(!api::AppendMgaTableMetadata(*context, table).error,
          "002624 target-table metadata publication failed");
  Require(!api::EnsureMgaRelationStorageDescriptor(
               *context, table, {}, &target.descriptor)
               .error,
          "002624 target-table descriptor publication failed");
  api::EngineLocalizedName name;
  name.name = table.default_name;
  name.raw_name_text = table.default_name;
  name.display_name = table.default_name;
  name.language_tag = "en";
  name.name_class = "primary";
  name.default_name = true;
  name.identifier_profile_uuid = "sbsql_v3";
  Require(!api::PersistNameRegistryEntriesForObject(
               *context, "test.ddl_create_trigger.cancellation",
               table.table_uuid, "table", Text(target.schema_uuid), {name},
               table.default_name)
               .error,
          "002624 target-table name publication failed");
  return target;
}

bridge::StatementDdlCreateTriggerBindRequestV1 TriggerDemand(
    const bridge::StatementContextReceiptView& view) {
  sblr::SblrDdlCreateTriggerRequestV1 request;
  request.receipt = RawUuid(view.receipt_uuid);
  request.occurrence = 1;
  request.trigger_occurrence = 1;
  request.command_identity = 1;
  request.timing = sblr::DdlCreateTriggerTimingV1::kAfter;
  request.event = sblr::DdlCreateTriggerEventV1::kInsert;
  request.scope = sblr::DdlCreateTriggerScopeV1::kRow;
  request.target_kind = sblr::DdlCreateTriggerTargetKindV1::kTable;
  request.body_profile =
      sblr::DdlCreateTriggerBodyProfileV1::kAuditAfterInsertRow;
  request.trigger_name_atoms = {{"trig_items_ai", false}};
  request.target_name_atoms = {{"trig_items", false}};
  request.security_mode = sblr::DdlCreateTriggerSecurityModeV1::kInvoker;
  request.image_mode =
      sblr::DdlCreateTriggerImageModeV1::kPolicyRedacted;
  request.execution_mode =
      sblr::DdlCreateTriggerExecutionModeV1::kSameTransaction;
  request.enabled_state =
      sblr::DdlCreateTriggerEnabledStateV1::kEnabled;
  request.recursion_mode =
      sblr::DdlCreateTriggerRecursionModeV1::kForbidSelf;
  const auto bytes = sblr::EncodeSblrDdlCreateTriggerRequestV1(request);
  sblr::SblrDdlCreateTriggerRequestV1 decoded;
  std::string detail;
  Require(bytes.size() == sblr::kSblrDdlCreateTriggerRequestV1Bytes &&
              sblr::DecodeSblrDdlCreateTriggerRequestV1(
                  bytes.data(), bytes.size(), &decoded, &detail),
          "002624 canonical TVQX construction failed");

  bridge::StatementDdlCreateTriggerBindRequestV1 demand;
  demand.authenticated_receipt_uuid = view.receipt_uuid;
  demand.occurrence = decoded.occurrence;
  demand.trigger_occurrence = decoded.trigger_occurrence;
  demand.command_identity = decoded.command_identity;
  demand.timing = static_cast<std::uint8_t>(decoded.timing);
  demand.event = static_cast<std::uint8_t>(decoded.event);
  demand.scope = static_cast<std::uint8_t>(decoded.scope);
  demand.target_kind = static_cast<std::uint8_t>(decoded.target_kind);
  demand.body_profile = static_cast<std::uint16_t>(decoded.body_profile);
  for (const auto& atom : decoded.trigger_name_atoms) {
    demand.trigger_name_atoms.push_back({atom.raw_utf8, atom.quoted});
  }
  for (const auto& atom : decoded.target_name_atoms) {
    demand.target_name_atoms.push_back({atom.raw_utf8, atom.quoted});
  }
  demand.position = decoded.position;
  demand.security_mode = static_cast<std::uint8_t>(decoded.security_mode);
  demand.image_mode = static_cast<std::uint8_t>(decoded.image_mode);
  demand.execution_mode = static_cast<std::uint8_t>(decoded.execution_mode);
  demand.enabled_state = static_cast<std::uint8_t>(decoded.enabled_state);
  demand.recursion_mode = static_cast<std::uint8_t>(decoded.recursion_mode);
  demand.request_evidence_sha256 = decoded.evidence;
  demand.exact_bind_request_bytes = bytes;
  return demand;
}

sblr::SblrOperationEnvelope TriggerMember(
    const bridge::StatementContextReceiptView& view,
    std::string_view parser_uuid,
    const bridge::StatementDdlCreateTriggerAuthorityV1& authority) {
  auto operand_bytes = authority.canonical_descriptor_bytes;
  Require(operand_bytes.size() ==
              sblr::kSblrDdlCreateTriggerDescriptorV1Bytes,
          "002624 receipt did not retain exact TVDX");
  std::copy_n("TVDO", 4, operand_bytes.begin());
  sblr::SblrDdlCreateTriggerDescriptorV1 operand_descriptor;
  std::string detail;
  Require(sblr::DecodeSblrDdlCreateTriggerDescriptorV1(
              operand_bytes.data(), operand_bytes.size(),
              &operand_descriptor, &detail, true),
          "002624 TVDX to TVDO projection was not canonical");

  auto member = sblr::MakeSblrEnvelope(
      "engine.op.ddl_create_trigger", "SBLR_DDL_CREATE_TRIGGER",
      "ia08.ddl_create_trigger.cancellation_atomicity");
  member.opcode_code = 1551;
  member.result_shape = "ddl_result";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = parser_uuid;
  member.registry_snapshot_uuid = view.catalog_epoch_uuid;
  member.requires_security_context = true;
  member.requires_transaction_context = true;
  member.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "create_trigger_descriptor";
  operand.name = "trigger";
  operand.value_kind = sblr::SblrValueKind::create_trigger_descriptor;
  operand.value_body = std::move(operand_bytes);
  member.operands.push_back(std::move(operand));
  return member;
}

void RequireCancellation(
    const Fixture& fixture, PublicSession& session,
    const bridge::StatementContextReceiptView& view,
    bridge::StatementContextReceiptHandle receipt,
    std::string_view parser_uuid, const Submission& submission,
    std::atomic<unsigned>* probes, std::atomic<unsigned>* cancel_on_probe,
    unsigned expected_probe, std::string_view expected_key) {
  probes->store(0, std::memory_order_relaxed);
  cancel_on_probe->store(expected_probe, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle reservation;
  auto dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                        submission, &reservation);
  const auto before = SnapshotFixture(fixture);
  sb_engine_result_t result = nullptr;
  const auto status =
      bridge::DispatchStatementContextReceipt(&dispatch, &result);
  if (status != SB_ENGINE_STATUS_TIMEOUT || result == nullptr) {
    std::cerr << "002624 cancellation observation: status="
              << sb_engine_status_name(status)
              << ";probes=" << probes->load(std::memory_order_relaxed);
    if (result != nullptr) {
      std::cerr << ";code=" << DiagnosticCode(result)
                << ";key=" << DiagnosticKey(result)
                << ";detail=" << DiagnosticDetail(result);
    }
    std::cerr << '\n';
  }
  Require(status == SB_ENGINE_STATUS_TIMEOUT && result != nullptr,
          "002624 CREATE TRIGGER cancellation did not stop dispatch");
  Require(DiagnosticCode(result) == "PROCESS.CANCELLED" &&
              DiagnosticKey(result) == expected_key &&
              probes->load(std::memory_order_relaxed) == expected_probe,
          "002624 CREATE TRIGGER cancellation boundary drifted");
  sb_engine_command_completion_view_v1_t completion{};
  Require(sb_engine_result_completion(result, &completion) !=
                  SB_ENGINE_STATUS_OK ||
              (completion.operation_id.size_bytes == 0 &&
               completion.affected_rows == 0),
          "002624 cancelled CREATE TRIGGER published completion");
  (void)sb_engine_result_release(result);
  Require(SnapshotFixture(fixture) == before,
          "002624 cancelled CREATE TRIGGER changed durable state");
}

}  // namespace

int main() {
  auto fixture = CreateFixture();
  PublicSession session(fixture);
  std::atomic<unsigned> probes{0};
  std::atomic<unsigned> cancel_on_probe{0};
  auto context = BeginTransaction(fixture, &probes);
  context.query_cancellation_requested = [&] {
    const auto ordinal = probes.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto target = cancel_on_probe.load(std::memory_order_relaxed);
    return target != 0 && ordinal == target;
  };
  const auto target = PrepareTriggerTarget(fixture, &context);
  (void)target;

  const auto parser_uuid = Text(NewUuid(platform::UuidKind::object, 26243));
  context.current_package_uuid.canonical = parser_uuid;
  bridge::StatementContextAcquireRequest acquire;
  acquire.engine_context = &context;
  acquire.exact_transaction_uuid = context.transaction_uuid.canonical;
  bridge::StatementContextReceiptHandle receipt;
  bridge::StatementContextReceiptView view;
  sb_engine_result_t result = nullptr;
  Require(bridge::AcquireStatementContextReceipt(
              session.session, &acquire, &receipt, &view, &result) ==
              SB_ENGINE_STATUS_OK,
          "002624 live statement receipt acquisition failed");
  if (result != nullptr) (void)sb_engine_result_release(result);
  Require(view.ddl_create_trigger_executor_availability_generation != 0,
          "002624 receipt omitted CREATE TRIGGER availability");

  const auto demand = TriggerDemand(view);
  bridge::StatementDdlCreateTriggerAuthorityV1 authority;
  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(1, std::memory_order_relaxed);
  result = nullptr;
  Require(bridge::BindStatementDdlCreateTriggerAuthorityV1(
              receipt, &demand, &authority, &result) ==
                  SB_ENGINE_STATUS_TIMEOUT &&
              result != nullptr &&
              DiagnosticCode(result) == "PROCESS.CANCELLED" &&
              DiagnosticKey(result) ==
                  "sblr.ddl_create_trigger.cancelled_before_resolution",
          "002624 bind cancellation did not stop before authority");
  (void)sb_engine_result_release(result);
  result = nullptr;
  Require(bridge::CopyStatementDdlCreateTriggerAuthorityV1(
              receipt, 1, 1, &authority, &result) ==
              SB_ENGINE_STATUS_SECURITY_DENIED,
          "002624 cancelled bind published receipt-private authority");
  if (result != nullptr) (void)sb_engine_result_release(result);

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(0, std::memory_order_relaxed);
  result = nullptr;
  Require(bridge::BindStatementDdlCreateTriggerAuthorityV1(
              receipt, &demand, &authority, &result) == SB_ENGINE_STATUS_OK &&
              result == nullptr &&
              !authority.canonical_descriptor_bytes.empty(),
          "002624 retry did not publish exact trigger authority");
  Require(bridge::CopyStatementDdlCreateTriggerAuthorityV1(
              receipt, 1, 1, &authority, &result) == SB_ENGINE_STATUS_OK &&
              !authority.terminal_result_published,
          "002624 trigger authority publication drifted");
  if (result != nullptr) (void)sb_engine_result_release(result);

  const auto submission = PackageWithMember(
      fixture, view, parser_uuid, TriggerMember(view, parser_uuid, authority));
  RequireCancellation(
      fixture, session, view, receipt, parser_uuid, submission, &probes,
      &cancel_on_probe, 1,
      "sblr.ddl_create_trigger.cancelled_before_catalog_mutation");
  RequireCancellation(
      fixture, session, view, receipt, parser_uuid, submission, &probes,
      &cancel_on_probe, 2,
      "sblr.ddl_create_trigger.cancelled_before_publication");
  result = nullptr;
  Require(bridge::CopyStatementDdlCreateTriggerAuthorityV1(
              receipt, 1, 1, &authority, &result) == SB_ENGINE_STATUS_OK &&
              !authority.terminal_result_published &&
              authority.canonical_terminal_result_bytes.empty(),
          "002624 prepublication cancellation changed terminal authority");
  if (result != nullptr) (void)sb_engine_result_release(result);

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(0, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle success_reservation;
  auto success_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                                submission, &success_reservation);
  result = nullptr;
  const auto success_status =
      bridge::DispatchStatementContextReceipt(&success_dispatch, &result);
  if (success_status != SB_ENGINE_STATUS_OK || result == nullptr) {
    std::cerr << "002624 success observation: status="
              << sb_engine_status_name(success_status)
              << ";probes=" << probes.load(std::memory_order_relaxed);
    if (result != nullptr) {
      std::cerr << ";code=" << DiagnosticCode(result)
                << ";key=" << DiagnosticKey(result)
                << ";detail=" << DiagnosticDetail(result);
    }
    std::cerr << '\n';
  }
  Require(success_status == SB_ENGINE_STATUS_OK && result != nullptr,
          "002624 cancellation retry did not create the trigger");
  const auto first_result = ResultPayload(result);
  sblr::SblrDdlCreateTriggerResultV1 decoded_result;
  std::string detail;
  Require(first_result.size() ==
                  sblr::kSblrDdlCreateTriggerResultV1Bytes &&
              sblr::DecodeSblrDdlCreateTriggerResultV1(
                  first_result.data(), first_result.size(), &decoded_result,
                  &detail),
          "002624 successful retry omitted exact TVRS");
  (void)sb_engine_result_release(result);

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(1, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle replay_reservation;
  auto replay_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                               submission, &replay_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&replay_dispatch, &result) ==
                  SB_ENGINE_STATUS_OK &&
              result != nullptr && ResultPayload(result) == first_result &&
              probes.load(std::memory_order_relaxed) == 0,
          "002624 postpublication cancellation retracted exact TVRS replay");
  (void)sb_engine_result_release(result);

  Require(bridge::ReleaseStatementContextReceipt(receipt) ==
              SB_ENGINE_STATUS_OK,
          "002624 statement receipt cleanup failed");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  Require(api::EngineRollbackTransaction(rollback).ok,
          "002624 fixture transaction rollback failed");
  return EXIT_SUCCESS;
}
