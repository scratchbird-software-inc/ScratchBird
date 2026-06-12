// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_policy_recommendation_application.hpp"
#include "api_types.hpp"
#include "catalog/datatype_index_optimizer_admission_api.hpp"
#include "database_lifecycle.hpp"
#include "index_route_capability.hpp"
#include "local_transaction_store.hpp"
#include "memory.hpp"
#include "query/plan_api.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace idx = scratchbird::core::index;
namespace memory = scratchbird::core::memory;
namespace sblr = scratchbird::engine::sblr;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1771200000000ull;
constexpr scratchbird::core::platform::u32 kPageSize = 16384;

struct CleanupDir {
  std::filesystem::path root;
  ~CleanupDir() {
    if (root.empty()) return;
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
};

struct Fixture {
  std::filesystem::path root;
  std::filesystem::path database_path;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid relation_uuid;
  TypedUuid descriptor_uuid;
  TypedUuid index_uuid;
  TypedUuid principal_uuid;
  TypedUuid session_uuid;
  TypedUuid policy_uuid;
};

bool Expect(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

bool Contains(std::string_view value, std::string_view needle) {
  return value.find(needle) != std::string_view::npos;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) {
      return true;
    }
  }
  return false;
}

bool HasEvidenceKind(const api::EngineApiResult& result,
                     std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) return true;
  }
  return false;
}

bool HasDiagnostic(const api::EngineApiResult& result,
                   std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool DispatchHasDiagnostic(const sblr::SblrDispatchResult& result,
                           std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::string DiagnosticText(const api::EngineApiResult& result) {
  if (result.diagnostics.empty()) return {};
  const auto& diagnostic = result.diagnostics.front();
  return diagnostic.code + ":" + diagnostic.message_key + ":" +
         diagnostic.detail;
}

bool ExpectApiOk(const api::EngineApiResult& result,
                 std::string_view message) {
  if (result.ok) return true;
  std::cerr << message << ": " << DiagnosticText(result) << '\n';
  return false;
}

bool ExpectDispatchOk(const sblr::SblrDispatchResult& result,
                      std::string_view message) {
  if (result.accepted && result.envelope_validated && result.dispatched_to_api &&
      result.api_result.ok) {
    return true;
  }
  std::cerr << message << ": " << DiagnosticText(result.api_result) << '\n';
  return false;
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  return generated.ok() ? generated.value : TypedUuid{};
}

std::string UuidText(const TypedUuid& typed_uuid) {
  return typed_uuid.valid() ? uuid::UuidToString(typed_uuid.value)
                            : std::string{};
}

api::EngineDescriptor Descriptor(std::string canonical_type_name,
                                 const TypedUuid& descriptor_uuid = {}) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = UuidText(descriptor_uuid);
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(canonical_type_name);
  descriptor.encoded_descriptor = "canonical=" + descriptor.canonical_type_name;
  return descriptor;
}

api::EngineTypedValue TypedValue(std::string canonical_type_name,
                                 std::string encoded_value) {
  api::EngineTypedValue value;
  value.descriptor = Descriptor(std::move(canonical_type_name));
  value.encoded_value = std::move(encoded_value);
  return value;
}

void AddGrant(api::EngineRequestContext* context,
              const TypedUuid& target_uuid,
              std::string right,
              u64 offset) {
  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical = UuidText(MakeUuid(UuidKind::object, offset));
  grant.subject_uuid = context->principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid.canonical = UuidText(target_uuid);
  grant.right = std::move(right);
  grant.security_epoch = context->security_epoch;
  context->authorization_context.grants.push_back(std::move(grant));
}

api::EngineRequestContext Context(const Fixture& fixture,
                                  std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = UuidText(fixture.database_uuid);
  context.node_uuid.canonical = UuidText(MakeUuid(UuidKind::object, 10));
  context.principal_uuid.canonical = UuidText(fixture.principal_uuid);
  context.session_uuid.canonical = UuidText(fixture.session_uuid);
  context.statement_uuid.canonical = UuidText(MakeUuid(UuidKind::object, 11));
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 7;
  context.security_epoch = 11;
  context.resource_epoch = 13;
  context.name_resolution_epoch = 17;
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      UuidText(MakeUuid(UuidKind::object, 12));
  context.authorization_context.principal_uuid = context.principal_uuid;
  context.authorization_context.security_epoch = context.security_epoch;
  context.authorization_context.policy_epoch = 19;
  context.authorization_context.catalog_generation_id =
      context.catalog_generation_id;
  context.authorization_context.effective_subjects.push_back(
      {context.principal_uuid, "principal"});
  context.authorization_context.evidence_tags.push_back(
      "public_sblr_uuid_mga_route_integration_gate");
  AddGrant(&context, fixture.relation_uuid, "OBS_INDEX_PROFILE_READ", 20);
  AddGrant(&context, fixture.relation_uuid, "OBS_AGENT_STATE_READ", 21);
  return context;
}

sblr::SblrOperationEnvelope Envelope(std::string operation_id,
                                     std::string opcode,
                                     std::string trace_key) {
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id),
                                         std::move(opcode),
                                         std::move(trace_key));
  envelope.source_artifact_map.policy_status =
      "non_authoritative_render_metadata";
  envelope.source_artifact_map.source_hash = "sha256:pcr006-render-metadata";
  envelope.source_artifact_map.render_metadata_only = true;
  envelope.source_artifact_map.symbols.push_back(
      {"object_display_name",
       "route_target",
       "",
       "customer_lookup",
       "local",
       "sha256:pcr006-symbol",
       false,
       false});
  return envelope;
}

sblr::SblrDispatchResult Dispatch(const api::EngineRequestContext& context,
                                  sblr::SblrOperationEnvelope envelope,
                                  api::EngineApiRequest api_request = {}) {
  sblr::SblrDispatchRequest request;
  request.context = context;
  request.envelope = std::move(envelope);
  request.api_request = std::move(api_request);
  return sblr::DispatchSblrOperation(request);
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "public_sblr_uuid_mga_route_integration_gate";
  policy.hard_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 24ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 16ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 8ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

bool ConfigureMemoryFixture() {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(), "public_sblr_uuid_mga_route_integration_gate");
  return Expect(configured.ok(), "memory fixture configuration failed") &&
         Expect(configured.fixture_mode,
                "memory fixture must not use production lazy defaults");
}

Fixture CreateFixture(const std::filesystem::path& root) {
  std::filesystem::create_directories(root);
  Fixture fixture;
  fixture.root = root;
  fixture.database_path = root / "pcr006_route_integration.sbdb";
  fixture.database_uuid = MakeUuid(UuidKind::database, 1);
  fixture.filespace_uuid = MakeUuid(UuidKind::filespace, 2);
  fixture.relation_uuid = MakeUuid(UuidKind::object, 3);
  fixture.descriptor_uuid = MakeUuid(UuidKind::object, 4);
  fixture.index_uuid = MakeUuid(UuidKind::object, 5);
  fixture.principal_uuid = MakeUuid(UuidKind::principal, 6);
  fixture.session_uuid = MakeUuid(UuidKind::object, 7);
  fixture.policy_uuid = MakeUuid(UuidKind::object, 8);

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = fixture.database_uuid;
  create.filespace_uuid = fixture.filespace_uuid;
  create.page_size = kPageSize;
  create.creation_unix_epoch_millis = kBaseMillis;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  return fixture;
}

api::EngineApiRequest PlanApiRequest(const Fixture& fixture) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = UuidText(fixture.relation_uuid);
  request.target_object.object_kind = "table";
  request.descriptors.push_back(Descriptor("int64", fixture.descriptor_uuid));
  request.option_envelopes.push_back("target_object_uuid:" +
                                     UuidText(fixture.relation_uuid));
  request.option_envelopes.push_back("target_object_kind:table");
  request.option_envelopes.push_back("query_operation:descriptor_validation");
  request.option_envelopes.push_back("execute:true");
  request.option_envelopes.push_back("catalog_stats_digest:pcr006.catalog.stats");
  request.option_envelopes.push_back("stats_epoch:7");
  request.option_envelopes.push_back("sbsfc085_surface_id:PCR-006");
  request.option_envelopes.push_back(
      "sbsfc085_runtime_evidence_kind:public_sblr_uuid_mga_route");
  request.option_envelopes.push_back(
      "sbsfc085_runtime_evidence_id:pcr006.integration");
  request.option_envelopes.push_back(
      "sbsfc085_descriptor_role:query_plan_descriptor");
  request.option_envelopes.push_back(
      "sbsfc085_descriptor_ref:sys.query.plan_descriptor");

  api::EngineRowValue row;
  row.requested_row_uuid.canonical =
      "relation-0-row-" + UuidText(MakeUuid(UuidKind::object, 30));
  row.fields.push_back({"id", TypedValue("int64", "42")});
  request.rows.push_back(std::move(row));
  return request;
}

bool ProveSblrEnvelopeAuthority() {
  bool ok = true;
  auto accepted = Envelope("query.plan_operation",
                           "SBLR_QUERY_PLAN_OPERATION",
                           "pcr006.accepted");
  accepted.requires_transaction_context = true;
  ok = Expect(sblr::ValidateSblrEnvelope(accepted).ok,
              "valid SBLR envelope was rejected") &&
       ok;

  auto sql = accepted;
  sql.contains_sql_text = true;
  const auto sql_result = sblr::ValidateSblrEnvelope(sql);
  ok = Expect(!sql_result.ok, "SBLR envelope accepted SQL text") && ok;
  bool sql_diag = false;
  for (const auto& diagnostic : sql_result.diagnostics) {
    sql_diag = sql_diag || diagnostic.code == "SB_SBLR_SQL_TEXT_FORBIDDEN";
  }
  ok = Expect(sql_diag, "SBLR SQL-text refusal diagnostic missing") && ok;

  auto unresolved_names = accepted;
  unresolved_names.parser_resolved_names_to_uuids = false;
  const auto unresolved_result = sblr::ValidateSblrEnvelope(unresolved_names);
  ok = Expect(!unresolved_result.ok,
              "SBLR envelope accepted unresolved parser names") &&
       ok;
  bool name_diag = false;
  for (const auto& diagnostic : unresolved_result.diagnostics) {
    name_diag = name_diag ||
                diagnostic.code == "SB_SBLR_NAMES_NOT_RESOLVED_TO_UUIDS";
  }
  ok = Expect(name_diag, "SBLR unresolved-name diagnostic missing") && ok;
  return ok;
}

bool BeginRouteTransaction(Fixture const& fixture,
                           api::EngineRequestContext* context) {
  auto envelope =
      Envelope("transaction.begin", "SBLR_TRANSACTION_BEGIN", "pcr006.begin");
  envelope.requires_transaction_context = false;

  api::EngineApiRequest api_request;
  api_request.option_envelopes.push_back("transaction_read_mode:read_write");
  api_request.option_envelopes.push_back("fail_closed:true");
  const auto begun = Dispatch(*context, std::move(envelope), api_request);
  if (!ExpectDispatchOk(begun, "SBLR transaction.begin dispatch failed")) {
    return false;
  }
  if (!Expect(HasEvidence(begun.api_result, "mga_authority",
                          "durable_transaction_inventory"),
              "begin did not report durable MGA inventory authority") ||
      !Expect(HasEvidence(begun.api_result, "transaction_admission",
                          "engine_mga_admitted"),
              "begin did not report engine MGA admission") ||
      !Expect(!begun.api_result.transaction_uuid.canonical.empty(),
              "begin did not allocate transaction UUID") ||
      !Expect(begun.api_result.local_transaction_id != 0,
              "begin did not allocate local transaction id")) {
    return false;
  }
  context->transaction_uuid = begun.api_result.transaction_uuid;
  context->local_transaction_id = begun.api_result.local_transaction_id;
  context->snapshot_visible_through_local_transaction_id =
      begun.api_result.local_transaction_id;
  (void)fixture;
  return true;
}

bool ProveRoutePlanning(const Fixture& fixture,
                        const api::EngineRequestContext& context) {
  auto envelope = Envelope("query.plan_operation",
                           "SBLR_QUERY_PLAN_OPERATION",
                           "pcr006.plan");
  envelope.requires_transaction_context = true;
  const auto planned = Dispatch(context, std::move(envelope),
                                PlanApiRequest(fixture));
  return ExpectDispatchOk(planned, "SBLR query.plan_operation dispatch failed") &&
         Expect(HasEvidenceKind(planned.api_result, "optimizer_metric_input"),
                "optimizer did not consume catalog/runtime statistics") &&
         Expect(HasEvidenceKind(planned.api_result,
                                "optimizer_selected_candidate"),
                "optimizer did not select a physical candidate") &&
         Expect(HasEvidence(planned.api_result, "parser_executes_sql", "false"),
                "optimizer route did not reject parser SQL execution authority") &&
         Expect(HasEvidence(planned.api_result, "parser_claims_transaction_finality",
                            "false"),
                "optimizer route did not reject parser transaction authority");
}

bool ProveSecurityAuthorization(const Fixture& fixture,
                                const api::EngineRequestContext& context) {
  auto envelope =
      Envelope("security.authorize", "SBLR_SECURITY_AUTHORIZE", "pcr006.auth");
  envelope.requires_transaction_context = true;
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = UuidText(fixture.relation_uuid);
  request.target_object.object_kind = "table";
  request.option_envelopes.push_back("right:OBS_INDEX_PROFILE_READ");
  const auto authorized = Dispatch(context, std::move(envelope), request);
  bool ok = ExpectDispatchOk(authorized, "security authorize dispatch failed");
  ok = Expect(HasEvidence(authorized.api_result, "authorization_authority",
                          "materialized_authorization_context"),
              "authorization did not use materialized engine context") &&
       ok;

  auto cluster_envelope = Envelope("security.authorize",
                                   "SBLR_SECURITY_AUTHORIZE",
                                   "pcr006.cluster_auth");
  cluster_envelope.requires_transaction_context = true;
  api::EngineApiRequest cluster_request;
  cluster_request.target_object.uuid.canonical = UuidText(fixture.relation_uuid);
  cluster_request.target_object.object_kind = "cluster_route";
  cluster_request.option_envelopes.push_back("right:OBS_CLUSTER_HEALTH_INSPECT");
  const auto refused = Dispatch(context, std::move(cluster_envelope),
                                cluster_request);
  ok = Expect(refused.accepted && refused.dispatched_to_api,
              "cluster authorization did not reach engine API") &&
       ok;
  ok = Expect(!refused.api_result.ok &&
                  HasDiagnostic(refused.api_result,
                                "SECURITY.CLUSTER.AUTHORITY_REQUIRED"),
              "cluster authorization did not fail closed without provider") &&
       ok;
  return ok;
}

bool ProveIndexDatatypeAndAgentBoundaries(const Fixture& fixture) {
  bool ok = true;
  const auto* route = idx::FindBuiltinIndexRouteCapabilityState(
      idx::IndexRouteKind::sql_select, idx::IndexFamily::btree);
  ok = Expect(route != nullptr && route->route_complete(),
              "btree sql_select route capability is not complete") &&
       ok;
  ok = Expect(route != nullptr && route->requires_mga_recheck &&
                  route->requires_security_recheck,
              "index route does not require MGA and security rechecks") &&
       ok;

  api::EngineDatatypeIndexOptimizerAdmissionRequest datatype_request;
  datatype_request.type_group = "scalar";
  datatype_request.descriptor = Descriptor("int64", fixture.descriptor_uuid);
  datatype_request.support_path = "scalar_family:canonical_descriptor:btree";
  datatype_request.index_stats_status = "validated";
  datatype_request.reference_label = "postgres_bigint";
  const auto datatype = api::EvaluateDatatypeIndexOptimizerAdmission(
      datatype_request);
  ok = Expect(datatype.ok && datatype.index_admitted &&
                  datatype.statistics_admitted,
              "datatype/index/optimizer admission failed") &&
       ok;
  ok = Expect(datatype.optimizer_uses_canonical_descriptor &&
                  datatype.canonical_descriptor_used == "int64",
              "datatype/index/optimizer admission used reference label authority") &&
       ok;

  agents::AgentPolicyRecommendationApplicationRequest agent_request;
  agent_request.recommendation_uuid = UuidText(MakeUuid(UuidKind::object, 60));
  agent_request.evidence_uuid = UuidText(MakeUuid(UuidKind::object, 61));
  agent_request.policy_family = "memory_governor_policy";
  agent_request.scope_uuid = UuidText(fixture.database_uuid);
  agent_request.metric_digest = "sha256:pcr006-memory-metric";
  agent_request.proposed_field_name = "emergency_reserve_percent";
  agent_request.proposed_field_value = "25";
  agent_request.policy_generation = 3;
  agent_request.observed_policy_generation = 3;
  agent_request.durable_catalog_state = true;
  agent_request.strict_metric_snapshot = true;
  agent_request.metric_trusted = true;
  agent_request.metric_fresh = true;
  const auto agent = agents::EvaluateAgentPolicyRecommendationApplication(
      agent_request);
  ok = Expect(agent.ok && agent.recommendation_record_created,
              "agent recommendation was not accepted as pending-review evidence") &&
       ok;
  ok = Expect(agent.auto_apply_blocked,
              "agent recommendation unexpectedly allowed auto-apply") &&
       ok;
  return ok;
}

bool CommitRouteTransaction(api::EngineRequestContext* context) {
  auto envelope = Envelope("transaction.commit",
                           "SBLR_TRANSACTION_COMMIT",
                           "pcr006.commit");
  envelope.requires_transaction_context = true;
  const auto committed = Dispatch(*context, std::move(envelope));
  const bool ok =
      ExpectDispatchOk(committed, "SBLR transaction.commit dispatch failed") &&
      Expect(HasEvidence(committed.api_result, "mga_authority",
                         "durable_transaction_inventory"),
             "commit did not report durable MGA inventory authority");
  context->local_transaction_id = 0;
  context->transaction_uuid.canonical.clear();
  return ok;
}

bool ProveMGAInventoryFinality(const Fixture& fixture) {
  const auto loaded =
      db::LoadLocalTransactionInventoryFromDatabase(
          fixture.database_path.string());
  bool ok = Expect(loaded.ok(), "could not load transaction inventory") &&
            Expect(!loaded.inventory.entries.empty(),
                   "transaction inventory contained no entries");
  bool committed = false;
  for (const auto& entry : loaded.inventory.entries) {
    committed = committed ||
                (entry.state == txn::TransactionState::committed &&
                 entry.identity.local_id.value == 1 &&
                 entry.identity.transaction_uuid.valid());
  }
  ok = Expect(committed,
              "committed transaction was not recorded in MGA inventory") &&
       ok;
  return ok;
}

bool ProveClusterSblrFailsClosed(const api::EngineRequestContext& context) {
  auto envelope = Envelope("cluster.inspect_state",
                           "SBLR_CLUSTER_INSPECT_STATE",
                           "pcr006.cluster");
  envelope.requires_cluster_authority = true;
  const auto refused = Dispatch(context, std::move(envelope));
  return Expect(refused.accepted && refused.dispatched_to_api,
                "cluster SBLR route did not reach cluster provider boundary") &&
         Expect(!refused.api_result.ok,
                "cluster SBLR route succeeded without provider") &&
         Expect(DispatchHasDiagnostic(refused,
                                      "SBLR.CLUSTER.SUPPORT_NOT_ENABLED") ||
                    DispatchHasDiagnostic(
                        refused,
                        "SBLR.CLUSTER.HANDSHAKE.STUB_COMPILE_LINK_ONLY"),
                "cluster SBLR route did not fail closed at provider boundary");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: public_sblr_uuid_mga_route_integration_gate <work-dir>\n";
    return 2;
  }

  CleanupDir cleanup{std::filesystem::path(argv[1])};
  bool ok = ConfigureMemoryFixture();
  const Fixture fixture = CreateFixture(cleanup.root);
  api::EngineRequestContext context =
      Context(fixture, "public-sblr-uuid-mga-route-integration");

  ok = ProveSblrEnvelopeAuthority() && ok;
  ok = BeginRouteTransaction(fixture, &context) && ok;
  ok = ProveRoutePlanning(fixture, context) && ok;
  ok = ProveSecurityAuthorization(fixture, context) && ok;
  ok = ProveIndexDatatypeAndAgentBoundaries(fixture) && ok;
  ok = CommitRouteTransaction(&context) && ok;
  ok = ProveMGAInventoryFinality(fixture) && ok;
  ok = ProveClusterSblrFailsClosed(context) && ok;

  if (!ok) return 1;
  std::cout << "public_sblr_uuid_mga_route_integration_gate=passed\n";
  return 0;
}
