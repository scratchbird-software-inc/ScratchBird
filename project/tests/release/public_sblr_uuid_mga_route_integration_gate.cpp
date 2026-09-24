#include "../support/engine_evidence_fixture.hpp"
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
#include "catalog/datatype_bootstrap_identity.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "security/security_model.hpp"
#include "server_engine_bridge/statement_context.hpp"
#include "engine/sblr/sblr_literal_runtime.hpp"
#include "hash_digest.hpp"
#include "index_route_capability.hpp"
#include "local_transaction_store.hpp"
#include "memory.hpp"
#include "query/plan_api.hpp"
#include "security/authorization_api.hpp"
#include "sblr_dispatch.hpp"
#include "scratchbird/engine/sblr_envelope.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "relational_descriptor_codec.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include "sblr_transaction_begin_runtime.hpp"
#include "sblr_transaction_commit_runtime.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
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
    if (evidence.evidence_kind == kind && scratchbird::tests::EvidenceTextEquals(evidence.evidence_id, id)) {
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

std::string IdentityBytes(const TypedUuid& typed_uuid) {
  return typed_uuid.valid()
      ? std::string(reinterpret_cast<const char*>(typed_uuid.value.bytes.data()),
                    typed_uuid.value.bytes.size())
      : std::string{};
}

api::EngineDescriptor Descriptor(std::string canonical_type_name,
                                 const TypedUuid& descriptor_uuid = {}) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid = descriptor_uuid.value;
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

void Require(bool ok, std::string_view message);

api::EngineRequestContext Context(const Fixture& fixture,
                                  std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid = fixture.database_uuid.value;
  context.default_root_uuid = fixture.filespace_uuid.value;
  const auto bootstrap = db::ReadDatabaseBootstrapSecurityCatalog(context.database_path);
  Require(bootstrap.ok() && bootstrap.state.present && bootstrap.state.committed_by_inventory,
          "PCR-006 durable bootstrap principal unavailable");
  context.principal_uuid = bootstrap.state.principal_uuid.value;
  context.session_uuid = fixture.session_uuid.value;
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 1;
  context.datatype_catalog_snapshot_uuid = api::kBootstrapDatatypeCatalogUuid;
  context.datatype_catalog_generation = api::kBootstrapDatatypeCatalogGeneration;
  context.datatype_registry_generation = api::kBootstrapDatatypeRegistryGeneration;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  const auto loaded = api::LoadSecurityPrincipalLifecycleState(context);
  Require(loaded.ok, "PCR-006 durable security catalog unavailable");
  const auto& lifecycle = loaded.state;
  api::DurableAuthorizationState authority;
  authority.authority_uuid = context.database_uuid;
  authority.security_context_generation = lifecycle.security_context_generation;
  authority.security_epoch = lifecycle.security_generation;
  authority.policy_epoch = lifecycle.policy_generation;
  authority.catalog_generation_id = 1;
  authority.engine_owned_sysarch_role_uuid = bootstrap.state.sysarch_role_uuid.value;
  for (const auto& principal : lifecycle.principals)
    if (!principal.deleted && principal.lifecycle_state == "active")
      authority.principals.push_back({principal.principal_uuid, "principal", true,
                                     authority.security_epoch});
  for (const auto& role : lifecycle.roles)
    if (!role.deleted && role.lifecycle_state == "active")
      authority.roles.push_back({role.role_uuid, true, authority.security_epoch});
  for (const auto& membership : lifecycle.memberships)
    if (!membership.revoked)
      authority.memberships.push_back({membership.member_principal_uuid, "principal",
          membership.container_uuid, membership.container_kind, true, authority.security_epoch});
  for (const auto& grant : lifecycle.grants)
    if (!grant.revoked)
      authority.grants.push_back({grant.grant_uuid, grant.grantee_uuid, grant.grantee_kind,
          grant.target_object_uuid, grant.privilege, grant.grant_effect == "deny", true,
          authority.security_epoch});
  const auto materialized = api::MaterializeDurableAuthorizationContext(authority,
      {context.principal_uuid, authority.security_epoch, authority.policy_epoch,
       authority.catalog_generation_id});
  Require(materialized.ok, "PCR-006 durable authorization unavailable");
  context.authorization_context = materialized.context;
  context.security_epoch = authority.security_epoch;
  context.authorization_context.security_epoch = authority.security_epoch;
  return context;
}

sblr::SblrOperationEnvelope Envelope(std::string operation_id,
                                     std::string opcode,
                                     std::string trace_key) {
  const auto* registry_entry = sblr::LookupSblrOperation(operation_id);
  if (registry_entry == nullptr) {
    std::cerr << "public route operation is absent from the canonical SBLR "
                 "registry: "
              << operation_id << '\n';
    std::exit(EXIT_FAILURE);
  }
  if (registry_entry->opcode != opcode) {
    std::cerr << "public route opcode mnemonic drifted for " << operation_id
              << ": expected " << registry_entry->opcode << " got " << opcode
              << '\n';
    std::exit(EXIT_FAILURE);
  }
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id),
                                         std::move(opcode),
                                         std::move(trace_key));
  envelope.opcode_code = registry_entry->code;
  envelope.result_shape = registry_entry->result_contract;
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.parser_package_uuid =
      MakeUuid(UuidKind::object, 40).value;
  envelope.registry_snapshot_uuid =
      MakeUuid(UuidKind::object, 41).value;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = true;
  return envelope;
}

sblr::SblrDispatchResult Dispatch(const api::EngineRequestContext& context,
                                  sblr::SblrOperationEnvelope envelope,
                                  api::EngineApiRequest api_request = {}) {
  api_request.context = context;
  api_request.operation_id = envelope.operation_id;
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
  create.bootstrap_principal_name = "public_route_owner";
  create.require_bootstrap_principal = true;
  create.allow_uncredentialed_bootstrap = false;
  create.bootstrap_credential_fingerprint =
      "local-password-pbkdf2-sha256:v1:iterations=600000:"
      "salt=0123456789abcdef0123456789abcdef:"
      "verifier=58a793aad0bd6840ad8d92f6627a23f6142c4ce58210c5f135ea3e2134d43142";
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  return fixture;
}

namespace platform = scratchbird::core::platform;
namespace bridge = scratchbird::server_engine_bridge;
namespace runtime = scratchbird::engine::sblr;
using Bytes = std::vector<std::uint8_t>;
[[noreturn]] void Fail(std::string_view message) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
void Require(bool ok, std::string_view message) { if (!ok) Fail(message); }
std::array<std::uint8_t, 16> RawUuid(const platform::Uuid& value) { return value.bytes; }
void U16(Bytes* out, std::uint16_t value) { for (unsigned i=0;i<2;++i) out->push_back(value>>(8*i)); }
void U32(Bytes* out, std::uint32_t value) { for (unsigned i=0;i<4;++i) out->push_back(value>>(8*i)); }
void U64(Bytes* out, std::uint64_t value) { for (unsigned i=0;i<8;++i) out->push_back(value>>(8*i)); }
struct LiteralBinding {
  Bytes sbxn;
  std::array<std::uint8_t,16> descriptor_uuid{};
  std::uint64_t descriptor_generation = 0;
};
class ProbeSession {
 public:
  explicit ProbeSession(const api::EngineRequestContext& context) {
    sb_engine_open_params_v1_t open{};
    open.struct_size = sizeof(open);
    open.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    open.database_path_utf8 = context.database_path.data();
    open.database_path_size = context.database_path.size();
    open.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
    Check(sb_engine_open(&open, &engine_, nullptr), nullptr, "engine open");
    sb_engine_session_params_v1_t begin{};
    begin.struct_size = sizeof(begin);
    begin.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    std::copy(context.principal_uuid.bytes.begin(), context.principal_uuid.bytes.end(),
              begin.effective_user_uuid.bytes);
    std::copy(context.session_uuid.bytes.begin(), context.session_uuid.bytes.end(),
              begin.session_uuid.bytes);
    begin.default_language_utf8 = "en";
    begin.default_language_size = 2;
    begin.trust_mode = SB_ENGINE_TRUST_SERVER_ISOLATED;
    Check(sb_engine_session_begin(engine_, &begin, &session_, nullptr), nullptr,
          "session begin");
  }
  ~ProbeSession() {
    sb_engine_session_end_params_v1_t end{};
    end.struct_size = sizeof(end);
    end.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    end.rollback_active_transactions = 1;
    end.cancel_open_results = 1;
    (void)sb_engine_session_end(session_, &end, nullptr);
    (void)sb_engine_close(engine_, nullptr);
  }
  ProbeSession(const ProbeSession&) = delete;
  ProbeSession& operator=(const ProbeSession&) = delete;
  sb_engine_session_t get() const { return session_; }
  static void Check(sb_engine_status_t status, sb_engine_result_t result,
                    const char* phase) {
    if (status != SB_ENGINE_STATUS_OK && result != nullptr) {
      sb_engine_diagnostic_set_view_t diagnostics{};
      if (sb_engine_result_diagnostics(result, &diagnostics) == SB_ENGINE_STATUS_OK) {
        for (std::size_t i = 0; i < diagnostics.diagnostic_count; ++i) {
          const auto& d = diagnostics.diagnostics[i];
          for (const auto text : {d.symbolic_code, d.message_key, d.safe_detail}) {
            if (text.data) std::cerr.write(text.data, text.size_bytes);
            std::cerr << ':';
          }
          std::cerr << '\n';
        }
      }
    }
    if (result) sb_engine_result_release(result);
    if (status != SB_ENGINE_STATUS_OK)
      Fail(std::string("public route fixture ") + phase + " failed");
  }
 private:
  sb_engine_handle_t engine_ = nullptr;
  sb_engine_session_t session_ = nullptr;
};

class ProbeStatement {
 public:
  ProbeStatement(const ProbeSession& session, const api::EngineRequestContext& base) {
    namespace bridge = scratchbird::server_engine_bridge;
    bridge::StatementContextAcquireRequest request;
    request.engine_context = &base;
    request.exact_transaction_uuid = base.transaction_uuid;
    sb_engine_result_t result = nullptr;
    const auto status = bridge::AcquireStatementContextReceipt(
        session.get(), &request, &receipt_, &view, &result);
    ProbeSession::Check(status, result, "statement acquisition");
    result = nullptr;
    const auto copied = bridge::CopyStatementContextEngineContextV1(
        receipt_, &context, &result);
    ProbeSession::Check(copied, result, "statement context copy");
  }
  ~ProbeStatement() {
    (void)scratchbird::server_engine_bridge::ReleaseStatementContextReceipt(receipt_);
  }
  ProbeStatement(const ProbeStatement&) = delete;
  ProbeStatement& operator=(const ProbeStatement&) = delete;
  api::EngineRequestContext context;
  bridge::StatementContextReceiptView view;
  bridge::StatementContextReceiptHandle get() const { return receipt_; }
 private:
  scratchbird::server_engine_bridge::StatementContextReceiptHandle receipt_;
};

inline LiteralBinding FinalizeLiteral(bridge::StatementContextReceiptHandle receipt,
                               const bridge::StatementContextReceiptView& view) {
  runtime::SblrLiteralPrebindRequestV1 request;
  request.preliminary_receipt_uuid = RawUuid(view.receipt_uuid);
  request.catalog_snapshot_uuid = RawUuid(view.literal_catalog_snapshot_uuid);
  request.catalog_generation = view.literal_catalog_generation;
  request.security_epoch = view.security_epoch;
  request.resource_epoch = view.resource_epoch;
  request.mga_snapshot_uuid = RawUuid(view.statement_snapshot_uuid);
  runtime::SblrLiteralDemandV1 demand;
  demand.occurrence_id = 1;
  demand.lexical_class = 1;
  demand.context_class = 1;
  const Bytes lexical{'4', '2'};
  demand.lexical_sha256 = scratchbird::core::hash::ComputeSha256Digest(lexical).digest;
  request.demands.push_back(demand);
  request.demand_sha256 = runtime::ComputeSblrLiteralDemandSequenceSha256V1(request.demands);
  const auto sbln = runtime::EncodeSblrLiteralPrebindRequestV1(request);
  Bytes sblq;
  sb_engine_result_t result = nullptr;
  Require(bridge::NegotiateStatementLiteralDescriptorsV1(
              receipt, sbln, &sblq, &result) == SB_ENGINE_STATUS_OK,
          "live literal descriptor negotiation failed");
  if (result) (void)sb_engine_result_release(result);
  Require(sblq.size()==356 && std::equal(sblq.begin(),sblq.begin()+4,"SBLQ") &&
              scratchbird::engine::SblrReadU32(sblq.data()+120)==1 &&
              scratchbird::engine::SblrReadU64(sblq.data()+160)==1 &&
              scratchbird::engine::SblrReadU32(sblq.data()+168)==184,
          "live literal descriptor result was not canonical");
  runtime::SblrLiteralPrebindResultV1 negotiated;
  std::copy_n(sblq.begin()+128,32,negotiated.ordered_profile_sha256.begin());
  runtime::SblrLiteralProfileMappingV1 mapping;
  mapping.occurrence_id=1;mapping.sblp_bytes.assign(sblq.begin()+172,sblq.end());
  negotiated.mappings.push_back(mapping);
  const auto profile = runtime::DecodeSblrLiteralDescriptorProfileV1(
      negotiated.mappings[0].sblp_bytes.data(),
      negotiated.mappings[0].sblp_bytes.size());
  Require(profile.ok, "live literal descriptor profile was not canonical");

  runtime::SblrExpressionNodeTableV1 table;
  runtime::SblrExpressionLiteralNodeV1 node;
  node.node_id = 7;
  node.parent_operand_ordinal = 1;
  node.descriptor_uuid = profile.profile.profile_uuid;
  node.descriptor_generation = profile.profile.descriptor_generation;
  const auto literal_body=runtime::EncodeSblrLiteralInt64LeV1(42);
  node.literal_body.assign(literal_body.begin(),literal_body.end());
  table.nodes.push_back(node);
  const auto sbxn = runtime::EncodeSblrExpressionNodeTableV1(table);
  const auto sbxn_sha = scratchbird::core::hash::ComputeSha256Digest(sbxn).digest;

  runtime::SblrLiteralBoundAstV1 bound;
  bound.preliminary_receipt_uuid = request.preliminary_receipt_uuid;
  bound.demand_sha256 = request.demand_sha256;
  runtime::SblrLiteralBoundAstNodeV1 ast;
  ast.parent_operand_ordinal = 1;
  ast.node_id = 7;
  ast.descriptor_uuid = profile.profile.profile_uuid;
  ast.descriptor_generation = profile.profile.descriptor_generation;
  ast.type_uuid = profile.profile.type_uuid;
  ast.profile_uuid = profile.profile.profile_uuid;
  ast.occurrence_id = 1;
  ast.lexical_sha256 = demand.lexical_sha256;
  bound.nodes.push_back(ast);
  const auto sbba = runtime::EncodeSblrLiteralBoundAstV1(bound);
  const auto bound_sha = runtime::ComputeSblrLiteralBoundAstSha256V1(sbba);

  Bytes sblf(208, 0);
  std::copy_n("SBLF", 4, sblf.begin());
  auto store16 = [&](std::size_t o, std::uint16_t v) { sblf[o]=v; sblf[o+1]=v>>8; };
  auto store32 = [&](std::size_t o, std::uint32_t v) { for(unsigned i=0;i<4;++i)sblf[o+i]=v>>(8*i); };
  auto store64 = [&](std::size_t o, std::uint64_t v) { for(unsigned i=0;i<8;++i)sblf[o+i]=v>>(8*i); };
  store16(4,1); store16(6,208); store32(8,static_cast<std::uint32_t>(208+sbba.size()+sbxn.size()));
  std::copy(request.preliminary_receipt_uuid.begin(),request.preliminary_receipt_uuid.end(),sblf.begin()+16);
  std::copy(request.demand_sha256.begin(),request.demand_sha256.end(),sblf.begin()+32);
  std::copy(negotiated.ordered_profile_sha256.begin(),negotiated.ordered_profile_sha256.end(),sblf.begin()+64);
  std::copy(bound_sha.begin(),bound_sha.end(),sblf.begin()+96);
  std::copy(sbxn_sha.begin(),sbxn_sha.end(),sblf.begin()+128);
  store64(160,request.catalog_generation); store64(168,request.security_epoch); store64(176,request.resource_epoch);
  std::copy(request.mga_snapshot_uuid.begin(),request.mga_snapshot_uuid.end(),sblf.begin()+184);
  store32(200,static_cast<std::uint32_t>(sbba.size())); store32(204,static_cast<std::uint32_t>(sbxn.size()));
  sblf.insert(sblf.end(),sbba.begin(),sbba.end()); sblf.insert(sblf.end(),sbxn.begin(),sbxn.end());
  Bytes sbla;
  result = nullptr;
  const auto finalize_status = bridge::FinalizeStatementLiteralBindingV1(
      receipt, sblf, &sbla, &result);
  if (finalize_status != SB_ENGINE_STATUS_OK) {
    std::cerr << "literal-finalize:" << sb_engine_status_name(finalize_status);
    if (result != nullptr) {
      sb_engine_diagnostic_set_view_t diagnostics{};
      if (sb_engine_result_diagnostics(result, &diagnostics) ==
              SB_ENGINE_STATUS_OK &&
          diagnostics.diagnostic_count != 0) {
        const auto& diagnostic = diagnostics.diagnostics[0];
        std::cerr << ':'
                  << std::string(diagnostic.symbolic_code.data,
                                 diagnostic.symbolic_code.size_bytes)
                  << ':'
                  << std::string(diagnostic.message_key.data,
                                 diagnostic.message_key.size_bytes);
      }
    }
    std::cerr << '\n';
  }
  Require(finalize_status == SB_ENGINE_STATUS_OK,
          "live literal binding finalize failed");
  if(result)(void)sb_engine_result_release(result);
  Require(sbla.size()==264,"live literal admission record size differed");
  LiteralBinding binding; binding.sbxn=sbxn; binding.descriptor_uuid=profile.profile.profile_uuid;
  binding.descriptor_generation=profile.profile.descriptor_generation;
  return binding;
}
sblr::SblrOperand UuidQueryOperand(std::uint32_t ordinal, std::string type,
                                  std::string name, const platform::Uuid& value) {
  Require(type == "uuid", "UUID operand type required");
  sblr::SblrOperand operand;
  operand.ordinal = ordinal; operand.type = std::move(type); operand.name = std::move(name);
  operand.value_kind = sblr::SblrValueKind::uuid_ref;
  operand.value_body.assign(value.bytes.begin(), value.bytes.end());
  return operand;
}

sblr::SblrOperand TypedQueryOperand(std::uint32_t ordinal, std::string type,
                                    std::string name,
                                    std::string_view value) {
  sblr::SblrOperand operand;
  operand.ordinal = ordinal;
  operand.type = std::move(type);
  operand.name = std::move(name);
  operand.value_kind = sblr::SblrValueKind::literal_typed;
  const auto carrier_type_uuid =
      RawUuid(scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d712"));
  operand.value_body.assign(carrier_type_uuid.begin(),
                            carrier_type_uuid.end());
  U64(&operand.value_body, value.size());
  operand.value_body.insert(operand.value_body.end(), value.begin(),
                            value.end());
  return operand;
}

sblr::SblrOperationEnvelope QueryExecuteEnvelope(
    const bridge::StatementContextReceiptView& view,
    const platform::Uuid& parser_uuid,
    const LiteralBinding& literal_binding,
    const bridge::StatementContextReceiptHandle& receipt) {
  api::EngineRequestContext admitted_context;
  Require(bridge::CopyStatementContextEngineContextV1(
              receipt, &admitted_context, nullptr) == SB_ENGINE_STATUS_OK,
          "query fixture live datatype context unavailable");
  api::RelationalTypeDescriptor descriptor_record;
  descriptor_record.descriptor_id = 1;
  descriptor_record.descriptor_uuid.bytes = literal_binding.descriptor_uuid;
  descriptor_record.type_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d712");
  descriptor_record.nullability = api::RelationalNullability::kNonNull;
  descriptor_record.datatype_identity_authoritative = true;
  descriptor_record.descriptor_generation = literal_binding.descriptor_generation;
  descriptor_record.type_generation = 1;
  descriptor_record.codec_id = "datatype.int64.le.v1";
  descriptor_record.codec_version = 1; descriptor_record.codec_generation = 1;
  descriptor_record.statement_receipt_uuid = view.receipt_uuid;
  descriptor_record.datatype_catalog_snapshot_uuid = admitted_context.datatype_catalog_snapshot_uuid;
  descriptor_record.datatype_catalog_generation = admitted_context.datatype_catalog_generation;
  descriptor_record.datatype_registry_generation = admitted_context.datatype_registry_generation;

  auto member = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      "pcr006.query");
  member.opcode_code = 4615;
  member.result_shape = "query_execute_result";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = parser_uuid;
  member.registry_snapshot_uuid = view.catalog_epoch_uuid;
  member.requires_security_context = true;
  member.requires_transaction_context = true;
  member.parser_resolved_names_to_uuids = true;

  std::uint32_t ordinal = 1;
  member.operands.push_back(TypedQueryOperand(
      ordinal++, "uint16", "relational_wire_version", "2"));
  member.operands.push_back(UuidQueryOperand(
      ordinal++, "uuid", "relational_bound_sblr_tree_uuid",
      view.bound_ast_uuid));
  member.operands.push_back(UuidQueryOperand(
      ordinal++, "uuid", "relational_catalog_epoch_uuid",
      view.catalog_epoch_uuid));
  member.operands.push_back(UuidQueryOperand(
      ordinal++, "uuid", "relational_security_context_uuid",
      view.security_context_uuid));
  member.operands.push_back(UuidQueryOperand(
      ordinal++, "uuid", "relational_statement_uuid", view.statement_uuid));
  member.operands.push_back(UuidQueryOperand(
      ordinal++, "uuid", "relational_owning_transaction_uuid",
      view.owning_transaction_uuid));
  member.operands.push_back(UuidQueryOperand(
      ordinal++, "uuid", "relational_statement_snapshot_uuid",
      view.statement_snapshot_uuid));
  member.operands.push_back(UuidQueryOperand(
      ordinal++, "uuid", "relational_statement_metadata_snapshot_uuid",
      view.statement_metadata_snapshot_uuid));
  member.operands.push_back(TypedQueryOperand(
      ordinal++, "uint64", "relational_local_transaction_id",
      std::to_string(view.owning_local_transaction_id)));
  member.operands.push_back(TypedQueryOperand(
      ordinal++, "uint64",
      "relational_snapshot_visible_through_local_transaction_id",
      std::to_string(view.visible_committed_high_watermark)));
  member.operands.push_back(TypedQueryOperand(
      ordinal++, "uint32", "relational_root_node_id", "1"));
  sblr::SblrOperand descriptor_operand;
  descriptor_operand.ordinal = ordinal++;
  descriptor_operand.type = "relational_descriptor_v3";
  descriptor_operand.name = "slot_1";
  descriptor_operand.value_kind = sblr::SblrValueKind::relational_type_descriptor;
  Require(sblr::EncodeRelationalTypeDescriptorV1(descriptor_record, &descriptor_operand.value_body),
          "relational descriptor encode failed");
  member.operands.push_back(std::move(descriptor_operand));

  const auto table_sha = scratchbird::core::hash::ComputeSha256Digest(
      literal_binding.sbxn);
  Require(table_sha.ok(), "PCR-006 literal table hash failed");
  sblr::SblrOperand reference;
  reference.ordinal = ordinal++;
  reference.type = "relational_expression_v1";
  reference.name = "1";
  reference.value_kind = sblr::SblrValueKind::expression_node_ref;
  U16(&reference.value_body, 1);
  U16(&reference.value_body, 0);
  U32(&reference.value_body, 1);
  U64(&reference.value_body, 7);
  reference.value_body.insert(reference.value_body.end(),
                              table_sha.digest.begin(),
                              table_sha.digest.end());
  reference.value_body.insert(reference.value_body.end(),
                              literal_binding.descriptor_uuid.begin(),
                              literal_binding.descriptor_uuid.end());
  U64(&reference.value_body, literal_binding.descriptor_generation);
  member.operands.push_back(std::move(reference));

  member.operands.push_back(TypedQueryOperand(
      ordinal++, "relational_output_v1", "slot_1",
      "1|1|1|1|0|6964"));
  member.operands.push_back(TypedQueryOperand(
      ordinal++, "relational_values_row_v1", "slot_1", "1"));
  member.operands.push_back(TypedQueryOperand(
      ordinal++, "relational_node_v1", "slot_1", "13|0|-|1|1"));
  sblr::SblrOperand binding;
  binding.ordinal = ordinal++;
  binding.type = "relational_node_binding_v2";
  binding.name = "slot_1";
  binding.value_kind = sblr::SblrValueKind::relational_node_binding;
  Require(sblr::EncodeRelationalNodeBindingV1(
              {1, "values.literal-table.v1", {1}, {}, {}, {}}, &binding.value_body),
          "PCR-006 source VALUES binary binding encoding failed");
  member.operands.push_back(std::move(binding));

  sblr::SblrOperand table;
  table.ordinal = ordinal++;
  table.type = "expression.node_table.v1";
  table.name = "expression_nodes";
  table.value_kind = sblr::SblrValueKind::expression_node_table;
  table.value_body = literal_binding.sbxn;
  member.operands.push_back(std::move(table));
  Require(ordinal == 19, "PCR-006 source VALUES operand count drifted");
  return member;
}

bool ProveSblrEnvelopeAuthority() {
  bool ok = true;
  auto accepted = Envelope("engine.op.txn_begin",
                           "SBLR_TXN_BEGIN",
                           "pcr006.accepted");
  accepted.requires_transaction_context = false;
  sblr::SblrTransactionBeginOptionsV1 options;
  options.isolation_profile_uuid[0] = 1;
  options.isolation_profile_generation = 1;
  options.transaction_policy_snapshot_uuid[0] = 2;
  options.transaction_policy_generation = 1;
  options.read_mode = 1;
  options.authority_scope = 1;
  options.wait_policy = 1;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "transaction.begin_options";
  operand.name = "options";
  operand.value_kind = sblr::SblrValueKind::transaction_begin_options;
  operand.value_body = sblr::EncodeSblrTransactionBeginOptionsV1(&options);
  accepted.operands.push_back(std::move(operand));
  ok = Expect(sblr::ValidateSblrEnvelope(accepted).ok,
              "valid SBLR envelope was rejected") &&
       ok;

  auto sql = accepted;
  sql.contains_sql_text = true;
  const auto sql_result = sblr::ValidateSblrEnvelope(sql);
  ok = Expect(!sql_result.ok, "SBLR envelope accepted SQL text") && ok;
  bool sql_diag = false;
  for (const auto& diagnostic : sql_result.diagnostics) {
    sql_diag = sql_diag ||
               diagnostic.code ==
                   "SBLR.OPERATION.DUPLICATE_INGRESS_AUTHORITY";
  }
  ok = Expect(sql_diag, "SBLR SQL-text refusal diagnostic missing") && ok;

  auto identity_mismatch = accepted;
  ++identity_mismatch.opcode_code;
  const auto mismatch_result = sblr::ValidateSblrEnvelope(identity_mismatch);
  ok = Expect(!mismatch_result.ok,
              "SBLR envelope accepted mismatched public opcode identity") &&
       ok;
  bool mismatch_diag = false;
  for (const auto& diagnostic : mismatch_result.diagnostics) {
    mismatch_diag = mismatch_diag ||
                    diagnostic.code ==
                        "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH";
  }
  ok = Expect(mismatch_diag,
              "SBLR public opcode-identity refusal diagnostic missing") &&
       ok;
  return ok;
}

struct RouteTransactionEvidence {
  scratchbird::core::hash::Digest256 begin_admission_sha256{};
};

bool BeginRouteTransaction(Fixture const& fixture,
                           api::EngineRequestContext* context,
                           RouteTransactionEvidence* evidence) {
  auto envelope =
      Envelope("engine.op.txn_begin", "SBLR_TXN_BEGIN", "pcr006.begin");
  envelope.requires_transaction_context = false;

  sblr::SblrTransactionBeginOptionsV1 options;
  options.isolation_profile_uuid[0] = 1;
  options.isolation_profile_generation = 1;
  options.transaction_policy_snapshot_uuid[0] = 2;
  options.transaction_policy_generation = 1;
  options.read_mode = 1;
  options.authority_scope = 1;
  options.wait_policy = 1;
  auto body = sblr::EncodeSblrTransactionBeginOptionsV1(&options);
  if (!Expect(!body.empty(),
              "canonical transaction-begin options failed to encode")) {
    return false;
  }
  const auto admission_sha =
      scratchbird::core::hash::ComputeSha256Digest(body);
  if (!Expect(admission_sha.ok(),
              "canonical transaction-begin evidence hash failed")) {
    return false;
  }
  evidence->begin_admission_sha256 = admission_sha.digest;

  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "transaction.begin_options";
  operand.name = "options";
  operand.value_kind = sblr::SblrValueKind::transaction_begin_options;
  operand.value_body = std::move(body);
  envelope.operands.push_back(std::move(operand));

  const auto admitted = Dispatch(*context, std::move(envelope));
  if (!ExpectDispatchOk(admitted,
                        "canonical SBLR transaction-begin admission failed") ||
      !Expect(admitted.api_result.local_transaction_id == 0 &&
                  admitted.api_result.transaction_uuid.is_nil(),
              "transaction-begin admission published engine MGA state")) {
    return false;
  }

  api::EngineBeginTransactionRequest begin;
  begin.context = *context;
  begin.operation_id = "transaction.begin";
  begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin);
  if (!ExpectApiOk(begun,
                   "engine-owned transaction begin failed after admission") ||
      !Expect(!begun.transaction_uuid.is_nil() &&
                  begun.local_transaction_id != 0,
              "engine-owned begin did not publish an MGA identity") ||
      !Expect(HasEvidence(begun, "mga_authority",
                          "durable_transaction_inventory") &&
                  HasEvidence(begun, "transaction_admission",
                              "engine_mga_admitted"),
              "engine-owned begin did not report durable MGA authority")) {
    return false;
  }
  context->transaction_uuid = begun.transaction_uuid;
  context->local_transaction_id = begun.local_transaction_id;
  context->snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context->transaction_isolation_level = begun.isolation_level;

  // Statement identity and snapshot are issued together by the receipt
  // acquisition in ProveRoutePlanning, after transaction admission.
  (void)fixture;
  return true;
}

bool ProveRoutePlanning(const Fixture& fixture,
                        const api::EngineRequestContext& base) {
  (void)fixture;
  ProbeSession session(base);
  ProbeStatement statement(session, base);
  const auto binding = FinalizeLiteral(statement.get(), statement.view);
  api::EngineRequestContext context;
  Require(bridge::CopyStatementContextEngineContextV1(statement.get(), &context, nullptr) ==
              SB_ENGINE_STATUS_OK, "PCR-006 finalized statement context unavailable");
  Require(!context.statement_uuid.is_nil() &&
              !context.statement_snapshot_uuid.is_nil() &&
              context.statement_metadata_snapshot_engine_owned &&
              context.transaction_uuid == base.transaction_uuid &&
              context.local_transaction_id == base.local_transaction_id,
          "PCR-006 receipt did not retain engine-owned statement and MGA identities");
  const auto parser_uuid = MakeUuid(UuidKind::object, 40).value;
  const auto query = [&] { return QueryExecuteEnvelope(statement.view, parser_uuid,
                                                       binding, statement.get()); };
  auto parser_authority = query();
  parser_authority.contains_sql_text = true;
  const auto parser_refused = Dispatch(context, std::move(parser_authority));
  bool ok = Expect(!parser_refused.accepted &&
                       !parser_refused.dispatched_to_api,
                   "optimizer route did not reject parser SQL execution authority") &&
            Expect(DispatchHasDiagnostic(
                       parser_refused,
                       "SBLR.OPERATION.DUPLICATE_INGRESS_AUTHORITY"),
                   "optimizer route did not preserve parser SQL refusal") ;

  auto transaction_authority = query();
  transaction_authority.requires_transaction_context = true;
  auto transactionless = context;
  transactionless.transaction_uuid = {};
  transactionless.local_transaction_id = 0;
  const auto transaction_refused =
      Dispatch(transactionless, std::move(transaction_authority));
  ok = Expect(!transaction_refused.accepted &&
                  !transaction_refused.dispatched_to_api,
              "optimizer route did not reject parser transaction authority") &&
       ok;

  for (const bool replace_receipt : {false, true}) {
    auto crossed = context;
    if (replace_receipt)
      crossed.statement_receipt_uuid = MakeUuid(UuidKind::object, 70).value;
    else
      crossed.statement_snapshot_uuid = MakeUuid(UuidKind::object, 71).value;
    const auto refused = Dispatch(crossed, query());
    ok = Expect(!refused.api_result.ok && !refused.canonical_result_published,
                "query.execute accepted a crossed binary statement receipt or snapshot UUID") && ok;
  }

  const auto executed = sblr::DispatchSblrOperation(
      {context, query(), api::EngineApiRequest{},
       std::nullopt});
  return ExpectDispatchOk(executed,
                          "canonical query.execute descriptor DAG failed") &&
         Expect(executed.logical_graph_populated &&
                    executed.optimizer_admitted && executed.optimizer_selected &&
                    executed.physical_dag_published &&
                    executed.physical_dag_executed &&
                    executed.runtime_actuals_attached &&
                    executed.canonical_result_published,
                "query.execute did not complete the canonical QOW route") &&
         Expect(executed.api_result.result_shape.rows.size() == 1 &&
                    executed.api_result.result_shape.rows.front().fields.size() ==
                        1 &&
                    executed.api_result.result_shape.rows.front()
                            .fields.front()
                            .first == "id" &&
                    executed.api_result.result_shape.rows.front()
                            .fields.front()
                            .second.descriptor.canonical_type_name == "int64" &&
                    executed.api_result.result_shape.rows.front()
                            .fields.front()
                            .second.encoded_value == "42",
                "query.execute did not publish the independent one-row int64 "
                "result") &&
         ok;
}

bool ProveSecurityAuthorization(const Fixture& fixture,
                                const api::EngineRequestContext& context) {
  api::EngineAuthorizeRequest request;
  request.context = context;
  request.operation_id = "security.authorize";
  request.target_object.uuid = fixture.relation_uuid.value;
  request.target_object.object_kind = "table";
  request.required_right = "OBS_INDEX_PROFILE_READ";
  const auto authorized = api::EngineAuthorize(request);
  bool ok = ExpectApiOk(authorized, "security authorization failed");
  ok = Expect(authorized.authorized &&
                  HasEvidence(authorized, "authorization_authority",
                          "materialized_authorization_context"),
              "authorization did not use materialized engine context") &&
       ok;

  api::EngineAuthorizeRequest cluster_request;
  cluster_request.context = context;
  cluster_request.operation_id = "security.authorize";
  cluster_request.target_object.uuid = fixture.relation_uuid.value;
  cluster_request.target_object.object_kind = "cluster_route";
  cluster_request.required_right = "OBS_CLUSTER_HEALTH_INSPECT";
  cluster_request.require_cluster_authority = true;
  const auto refused = api::EngineAuthorize(cluster_request);
  ok = Expect(!refused.ok && refused.cluster_authority_required &&
                  HasDiagnostic(refused,
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
  agent_request.recommendation_uuid = IdentityBytes(MakeUuid(UuidKind::object, 60));
  agent_request.evidence_uuid = IdentityBytes(MakeUuid(UuidKind::object, 61));
  agent_request.policy_family = "memory_governor_policy";
  agent_request.scope_uuid = IdentityBytes(fixture.database_uuid);
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

bool CommitRouteTransaction(api::EngineRequestContext* context,
                            const RouteTransactionEvidence& evidence) {
  auto envelope = Envelope("engine.op.txn_commit",
                           "SBLR_TXN_COMMIT",
                           "pcr006.commit");
  envelope.requires_transaction_context = true;

  if (!Expect(uuid::IsEngineIdentityUuid(context->transaction_uuid),
              "transaction UUID invalid before commit")) return false;
  sblr::SblrTransactionCommitOptionsV1 options;
  std::copy(context->transaction_uuid.bytes.begin(),
            context->transaction_uuid.bytes.end(), options.transaction_uuid.begin());
  options.local_transaction_id = context->local_transaction_id;
  options.admitted_handle_evidence_sha256 = evidence.begin_admission_sha256;
  options.commit_mode = 1;
  options.authority_scope = 1;
  options.wait_policy = 1;
  auto body = sblr::EncodeSblrTransactionCommitOptionsV1(&options);
  if (!Expect(!body.empty(),
              "canonical transaction-commit options failed to encode")) {
    return false;
  }
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "transaction.commit.options";
  operand.name = "options";
  operand.value_kind = sblr::SblrValueKind::transaction_commit_options;
  operand.value_body = std::move(body);
  envelope.operands.push_back(std::move(operand));

  const auto admitted = Dispatch(*context, std::move(envelope));
  if (!ExpectDispatchOk(admitted,
                        "canonical SBLR transaction-commit admission failed")) {
    return false;
  }

  api::EngineCommitTransactionRequest commit;
  commit.context = *context;
  commit.operation_id = "transaction.commit";
  const auto committed = api::EngineCommitTransaction(commit);
  const bool ok =
      ExpectApiOk(committed,
                  "engine-owned transaction commit failed after admission") &&
      Expect(committed.engine_finality_known &&
                 committed.commit_finality_state ==
                     "committed_by_engine_inventory" &&
                 HasEvidence(committed, "mga_authority",
                             "durable_transaction_inventory"),
             "engine-owned commit did not publish durable MGA finality");
  context->local_transaction_id = 0;
  context->transaction_uuid = {};
  context->statement_snapshot_uuid = {};
  return ok;
}

bool ProveMGAInventoryFinality(const Fixture& fixture, u64 transaction_id,
                              const api::EngineUuid& transaction_uuid) {
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
                 entry.identity.local_id.value == transaction_id &&
                 entry.identity.transaction_uuid.value == transaction_uuid);
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
                                      "PROCESS.CLUSTER_PATH_ABSENT") ||
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
  RouteTransactionEvidence transaction_evidence;

  ok = ProveSblrEnvelopeAuthority() && ok;
  ok = BeginRouteTransaction(fixture, &context, &transaction_evidence) && ok;
  ok = ProveRoutePlanning(fixture, context) && ok;
  ok = ProveSecurityAuthorization(fixture, context) && ok;
  ok = ProveIndexDatatypeAndAgentBoundaries(fixture) && ok;
  const auto transaction_id = context.local_transaction_id;
  const auto transaction_uuid = context.transaction_uuid;
  ok = CommitRouteTransaction(&context, transaction_evidence) && ok;
  ok = ProveMGAInventoryFinality(fixture, transaction_id, transaction_uuid) && ok;
  ok = ProveClusterSblrFailsClosed(context) && ok;

  if (!ok) return 1;
  std::cout << "public_sblr_uuid_mga_route_integration_gate=passed\n";
  return 0;
}
