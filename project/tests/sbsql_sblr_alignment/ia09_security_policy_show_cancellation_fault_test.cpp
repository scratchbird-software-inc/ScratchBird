// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// Reuse the real database/session/receipt/admission fixture. No public ABI,
// package gateway, executor registry, transaction inventory, or security
// policy store is mocked.
#define SCRATCHBIRD_IA05_QUERY_EXPLAIN_FIXTURE_ONLY
#include "ia05_query_explain_cancellation_fault_test.cpp"

#include "security/security_principal_lifecycle.hpp"
#include "security/database_local_security_event_store.hpp"

#include <atomic>
#include <cstdlib>
#include <string>

namespace {

Fixture CreateSecurityFixture() {
  Fixture fixture;
  const auto salt = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  fixture.directory = std::filesystem::temp_directory_path() /
      ("sbsql_sblr_ia09_show_cancel_" + std::to_string(salt));
  std::filesystem::create_directories(fixture.directory);
  fixture.database_path = fixture.directory / "fault.sbdb";
  fixture.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  fixture.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  fixture.principal_uuid = NewUuid(platform::UuidKind::principal, salt + 3);
  fixture.session_uuid = NewUuid(platform::UuidKind::session, salt + 4);
  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = fixture.database_uuid;
  create.filespace_uuid = fixture.filespace_uuid;
  create.creation_unix_epoch_millis =
      1950000000000ull + salt % 1'000'000ull;
  create.page_size = 16384;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "002164 security fixture database creation failed");
  fixture.resource_epoch =
      created.state.resource_seed_catalog.resource_epoch == 0
          ? 1
          : created.state.resource_seed_catalog.resource_epoch;
  return fixture;
}

sblr::SblrOperationEnvelope SecurityPolicyShowMember(
    const bridge::StatementContextReceiptView& view,
    std::string_view parser_uuid,
    const std::array<std::uint8_t, 16>& policy_uuid) {
  auto member = sblr::MakeSblrEnvelope(
      "security.policy.show", "SBLR_SECURITY_POLICY_SHOW",
      "ia09.security_policy_show.cancellation_atomicity");
  member.opcode_code = 1807;
  member.result_shape = "security_policy_result";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = parser_uuid;
  member.registry_snapshot_uuid = view.catalog_epoch_uuid;
  member.requires_security_context = true;
  member.requires_transaction_context = true;
  member.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "security_policy_show_descriptor";
  operand.name = "policy";
  operand.value_kind = sblr::SblrValueKind::uuid_ref;
  operand.value_body.assign(policy_uuid.begin(), policy_uuid.end());
  member.operands.push_back(std::move(operand));
  return member;
}

}  // namespace

int main() {
  auto fixture = CreateSecurityFixture();
  PublicSession session(fixture);
  std::atomic<unsigned> probes{0};
  std::atomic<unsigned> cancel_on_probe{0};
  auto context = BeginTransaction(fixture, &probes);
  context.query_cancellation_requested = {};
  context.current_schema_uuid.canonical =
      Text(NewUuid(platform::UuidKind::schema, 21640));
  const auto security_catalog =
      api::LoadDatabaseLocalSecurityEventStoreV1(context);
  Require(security_catalog.ok &&
              security_catalog.state.security_context_generation != 0,
          "002164 security catalog authority load failed");
  context.authorization_context.security_context_generation =
      security_catalog.state.security_context_generation;

  api::EngineMaterializedAuthorizationGrant policy_admin;
  policy_admin.grant_uuid.canonical =
      Text(NewUuid(platform::UuidKind::object, 21641));
  policy_admin.subject_uuid = context.principal_uuid;
  policy_admin.subject_kind = "principal";
  policy_admin.right = "POLICY_ADMIN";
  policy_admin.security_epoch = context.security_epoch;
  context.authorization_context.grants.push_back(std::move(policy_admin));

  const auto policy_identity =
      NewUuid(platform::UuidKind::object, 21642);
  const auto target_identity =
      NewUuid(platform::UuidKind::object, 21643);
  api::EngineSecurityCreatePolicyRequest create;
  create.context = context;
  create.policy_uuid = Text(policy_identity);
  create.policy_name = "show_cancel_policy";
  create.target_schema_uuid = context.current_schema_uuid.canonical;
  create.target_object_uuid = Text(target_identity);
  create.target_object_kind = "table";
  create.policy_effect = "row_filter";
  create.predicate_envelope = "predicate:true";
  const auto created = api::EngineSecurityCreatePolicy(create);
  if (!created.ok) {
    for (const auto& diagnostic : created.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
                << diagnostic.detail << '\n';
    }
  }
  Require(created.ok && created.policy_created &&
              created.policy_generation != 0,
          "002164 security-policy seed failed");

  const auto seeded_security = api::LoadDatabaseLocalSecurityEventStoreV1(
      context,
      api::DatabaseLocalSecurityEventVisibilityV1::
          include_reader_own_uncommitted);
  Require(seeded_security.ok && !seeded_security.state.events.empty() &&
              seeded_security.state.security_context_generation >
                  security_catalog.state.security_context_generation,
          "002164 policy seed did not publish its MGA-visible event");
  context.authorization_context.security_context_generation =
      seeded_security.state.security_context_generation;

  const auto parser_uuid = Text(NewUuid(platform::UuidKind::object, 21644));
  context.current_package_uuid.canonical = parser_uuid;
  context.query_cancellation_requested = [&] {
    const auto ordinal = probes.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto target = cancel_on_probe.load(std::memory_order_relaxed);
    return target != 0 && ordinal == target;
  };

  bridge::StatementContextAcquireRequest acquire;
  acquire.engine_context = &context;
  acquire.exact_transaction_uuid = context.transaction_uuid.canonical;
  bridge::StatementContextReceiptHandle receipt;
  bridge::StatementContextReceiptView view;
  sb_engine_result_t result = nullptr;
  Require(bridge::AcquireStatementContextReceipt(
              session.session, &acquire, &receipt, &view, &result) ==
              SB_ENGINE_STATUS_OK,
          "002164 live statement receipt acquisition failed");
  if (result != nullptr) (void)sb_engine_result_release(result);
  Require(view.security_policy_show_executor_availability_generation != 0,
          "002164 receipt omitted SHOW POLICY executor evidence");

  const auto submission = PackageWithMember(
      fixture, view, parser_uuid,
      SecurityPolicyShowMember(view, parser_uuid,
                               RawUuid(Text(policy_identity))));

  const auto require_cancelled = [&](unsigned target,
                                     std::string_view expected_key) {
    probes.store(0, std::memory_order_relaxed);
    cancel_on_probe.store(target, std::memory_order_relaxed);
    bridge::StatementPackageAdmissionReservationHandle reservation;
    auto dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                          submission, &reservation);
    result = nullptr;
    const auto status =
        bridge::DispatchStatementContextReceipt(&dispatch, &result);
    const auto code =
        result == nullptr ? std::string{} : DiagnosticCode(result);
    const auto key =
        result == nullptr ? std::string{} : DiagnosticKey(result);
    const auto observed = probes.load(std::memory_order_relaxed);
    if (status != SB_ENGINE_STATUS_TIMEOUT || result == nullptr ||
        code != "PROCESS.CANCELLED" || key != expected_key ||
        observed != target) {
      std::cerr << "002164 cancellation observation: target=" << target
                << ";status=" << sb_engine_status_name(status)
                << ";code=" << code << ";key=" << key
                << ";probes=" << observed << '\n';
    }
    Require(status == SB_ENGINE_STATUS_TIMEOUT && result != nullptr &&
                code == "PROCESS.CANCELLED" && key == expected_key &&
                observed == target,
            "002164 SHOW POLICY cancellation precedence drifted");
    (void)sb_engine_result_release(result);
    result = nullptr;
    const auto current_security =
        api::LoadDatabaseLocalSecurityEventStoreV1(
            context,
            api::DatabaseLocalSecurityEventVisibilityV1::
                include_reader_own_uncommitted);
    Require(current_security.ok &&
                current_security.state.security_context_generation ==
                    seeded_security.state.security_context_generation &&
                current_security.state.events == seeded_security.state.events,
            "002164 cancelled SHOW POLICY mutated MGA-visible policy state");
  };

  require_cancelled(
      1, "sblr.security_policy_show.cancelled_before_executor_evidence");
  require_cancelled(2, "security.policy.show.cancelled_before_snapshot");
  require_cancelled(3, "security.policy.show.cancelled_before_publication");

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(0, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle success_reservation;
  auto success_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                                submission, &success_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&success_dispatch,
                                                  &result) ==
                  SB_ENGINE_STATUS_OK &&
              result != nullptr &&
              probes.load(std::memory_order_relaxed) == 3,
          "002164 SHOW POLICY retry did not cross every checkpoint");
  const auto payload = ResultPayload(result);
  const std::string payload_text(payload.begin(), payload.end());
  Require(payload_text.find("policy_uuid=" + Text(policy_identity)) !=
                  std::string::npos &&
              payload_text.find("policy_generation=" +
                                std::to_string(created.policy_generation)) !=
                  std::string::npos,
          "002164 SHOW POLICY retry omitted the exact policy row");
  (void)sb_engine_result_release(result);

  auto verify_context = context;
  verify_context.query_cancellation_requested = {};
  api::EngineSecurityShowPolicyRequest verify;
  verify.context = std::move(verify_context);
  verify.policy_uuid = Text(policy_identity);
  const auto verified = api::EngineSecurityShowPolicy(verify);
  Require(verified.ok && verified.policy_found &&
              verified.policy.policy_uuid == Text(policy_identity) &&
              verified.current_policy_generation ==
                  created.policy_generation,
          "002164 SHOW POLICY cancellation changed the policy snapshot");
  const auto final_security = api::LoadDatabaseLocalSecurityEventStoreV1(
      context,
      api::DatabaseLocalSecurityEventVisibilityV1::
          include_reader_own_uncommitted);
  Require(final_security.ok &&
              final_security.state.security_context_generation ==
                  seeded_security.state.security_context_generation &&
              final_security.state.events == seeded_security.state.events,
          "002164 SHOW POLICY retry mutated MGA-visible policy state");

  Require(bridge::ReleaseStatementContextReceipt(receipt) ==
              SB_ENGINE_STATUS_OK,
          "002164 statement receipt cleanup failed");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  rollback.context.query_cancellation_requested = {};
  Require(api::EngineRollbackTransaction(rollback).ok,
          "002164 fixture transaction rollback failed");
  return EXIT_SUCCESS;
}
