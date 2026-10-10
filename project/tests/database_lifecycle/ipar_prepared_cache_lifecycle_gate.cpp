#include "dml/mga_relation_read_view.hpp"
#include "../support/engine_evidence_fixture.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "../support/published_mga_table_fixture.hpp"
#include "../support/catalog_column_binding_fixture.hpp"
#include "core/memory/memory.hpp"
#include "database_lifecycle.hpp"
#include "dml/insert_batch.hpp"
#include "ddl/create_api.hpp"
#include "ddl/alter_api.hpp"
#include "domain_support/domain_store.hpp"
#include "domain_support/domain_base_descriptor_codec.hpp"
#include "crud_support/bound_ordered_index_key.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"
#include "catalog/column_metadata_codec.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <utility>
#include <unistd.h>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr auto kLiveSchemaUuid = scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000001");
constexpr auto kLiveTableUuid = scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000101");
constexpr auto kLiveIndexUuid = scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000201");
constexpr auto kLiveDomainUuid = scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000301");
constexpr auto kLivePrincipalUuid = scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000401");
constexpr auto kLiveGroupUuid = scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000402");
api::EngineRequestContext live_owner;

[[noreturn]] void Fail(std::string_view message) {
  throw std::runtime_error(std::string(message));
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool DecodeInheritedBase(std::string_view bytes, api::EngineDescriptor* output) {
  api::DomainInheritedBaseBindingV1 binding;
  if (!api::DecodeDomainInheritedBaseBindingV1(bytes, &binding)) return false;
  *output = binding.base;
  return true;
}

api::EngineAuthorizationSubject Subject(api::EngineUuid uuid, std::string kind) {
  api::EngineAuthorizationSubject subject;
  subject.subject_uuid = std::move(uuid);
  subject.subject_kind = std::move(kind);
  return subject;
}

api::EngineRequestContext Context(std::string request_id,
                                  api::EngineUuid principal = scratchbird::tests::FixtureUuid(1558, 1),
                                  api::EngineUuid session = scratchbird::tests::FixtureUuid(1558, 2),
                                  api::EngineUuid role = scratchbird::tests::FixtureUuid(1558, 3),
                                  api::EngineUuid group = scratchbird::tests::FixtureUuid(1558, 4),
                                  std::uint64_t catalog_epoch = 101,
                                  std::uint64_t security_epoch = 201,
                                  std::uint64_t policy_epoch = 301) {
  api::EngineRequestContext context;
  context.request_id = std::move(request_id);
  context.database_uuid = scratchbird::tests::FixtureUuid(1208, 2001);
  context.principal_uuid = std::move(principal);
  context.session_uuid = std::move(session);
  context.current_role_uuid = std::move(role);
  context.transaction_uuid = scratchbird::tests::FixtureUuid(1208, 2002);
  // Component-only memory coordinates, not storage admission authority.
  context.statement_uuid = scratchbird::tests::FixtureUuid(1558, 31);
  context.statement_snapshot_uuid = scratchbird::tests::FixtureUuid(1558, 32);
  context.statement_metadata_snapshot_uuid = scratchbird::tests::FixtureUuid(1558, 33);
  context.optimizer_resource_snapshot_uuid = scratchbird::tests::FixtureUuid(1558, 34);
  context.local_transaction_id = 77;
  context.snapshot_visible_through_local_transaction_id = 77;
  context.catalog_generation_id = catalog_epoch;
  context.security_epoch = security_epoch;
  context.resource_epoch = policy_epoch;
  context.name_resolution_epoch = 401;
  context.security_context_present = true;

  context.authorization_context.present = true;
  context.authorization_context.authority_uuid = scratchbird::tests::FixtureUuid(1558, 5);
  context.authorization_context.principal_uuid = context.principal_uuid;
  context.authorization_context.catalog_generation_id = catalog_epoch;
  context.authorization_context.security_epoch = security_epoch;
  context.authorization_context.policy_epoch = policy_epoch;
  context.authorization_context.effective_subjects.push_back(
      Subject(context.principal_uuid, "principal"));
  context.authorization_context.effective_subjects.push_back(
      Subject(std::move(group), "group"));
  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid = scratchbird::tests::FixtureUuid(1558, 6);
  grant.subject_uuid = context.authorization_context.effective_subjects.back().subject_uuid;
  grant.subject_kind = "group";
  grant.target_uuid = scratchbird::tests::FixtureUuid(1558, 7);
  grant.right = "INSERT";
  grant.security_epoch = security_epoch;
  context.authorization_context.grants.push_back(std::move(grant));
  api::EngineMaterializedAuthorizationPolicy policy;
  policy.policy_uuid = scratchbird::tests::FixtureUuid(1558, 8);
  policy.subject_uuid = context.authorization_context.effective_subjects.back().subject_uuid;
  policy.subject_kind = "group";
  policy.target_uuid = scratchbird::tests::FixtureUuid(1558, 7);
  policy.right = "INSERT";
  policy.policy_kind = "rls_filter";
  policy.requires_runtime_recheck = true;
  policy.policy_epoch = policy_epoch;
  policy.canonical_policy_envelope = "sblr_predicate:tenant_visible";
  context.authorization_context.policies.push_back(std::move(policy));
  context.authorization_context.evidence_tags.push_back("group_chain_depth=1");
  return context;
}

std::string DomainColumnMetadata(const api::EngineUuid& domain) {
  api::CatalogColumnMetadata fields;
  fields.text = {{"canonical", "int64"}, {"primary_key", "true"},
                 {"not_null", "true"}, {"check", "gte:0"}};
  fields.identities.emplace("domain_uuid", domain);
  std::string bytes;
  Require(api::EncodeCatalogColumnMetadata(fields, &bytes),
          "cache fixture domain metadata encoding failed");
  return bytes;
}

api::CrudTableRecord Table(api::EngineUuid table_uuid = scratchbird::tests::FixtureUuid(1558, 7)) {
  api::CrudTableRecord table;
  table.creator_tx = 77;
  table.table_uuid = std::move(table_uuid);
  table.default_name = "ipar_cache_table";
  table.columns.push_back({"id", DomainColumnMetadata(scratchbird::tests::FixtureUuid(1558, 20))});
  table.columns.push_back({"payload", "canonical=character;default=literal:empty;check=length_lte:256"});
  return table;
}

api::CrudIndexRecord Index(const api::EngineUuid& table_uuid) {
  api::CrudIndexRecord index;
  index.creator_tx = 77;
  const std::array tables{scratchbird::tests::FixtureUuid(1558, 7),
                          scratchbird::tests::FixtureUuid(1558, 14),
                          scratchbird::tests::FixtureUuid(1558, 15)};
  const auto found = std::find(tables.begin(), tables.end(), table_uuid);
  Require(found != tables.end(), "unmapped cache fixture table identity");
  index.index_uuid = scratchbird::tests::FixtureUuid(1558, 100 + (found - tables.begin()));
  index.table_uuid = table_uuid;
  index.column_name = "id";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = true;
  index.key_envelopes.push_back("unique");
  return index;
}

api::MgaRelationReadView State(const api::CrudTableRecord& table) {
  api::MgaRelationReadView state;
  state.transactions[77] = "active";
  state.tables.push_back(table);
  return state;
}

api::EngineInsertRowsRequest InsertRequest(api::EngineRequestContext context,
                                           std::vector<std::string> options = {}) {
  api::EngineInsertRowsRequest request;
  request.context = std::move(context);
  request.target_table.uuid = scratchbird::tests::FixtureUuid(1558, 7);
  request.target_schema.uuid = scratchbird::tests::FixtureUuid(1558, 9);
  request.target_object.uuid = scratchbird::tests::FixtureUuid(1558, 7);
  request.bound_object_identity.object_uuid = request.target_table.uuid;
  request.bound_object_identity.catalog_generation_id = request.context.catalog_generation_id;
  request.bound_object_identity.security_epoch = request.context.security_epoch;
  request.bound_object_identity.resource_epoch = request.context.resource_epoch;
  request.estimated_row_count = 1;
  request.input_rows.push_back({});
  request.option_envelopes = std::move(options);
  return request;
}

void BindExpectedAuthority(api::EngineInsertRowsRequest* request,
                           const api::InsertBatchContext& context,
                           bool bind_content = true) {
  auto& expected = request->prepared_descriptor_expectation;
  expected.principal_uuid = context.prepared_descriptor_principal_uuid;
  expected.role_uuid = context.prepared_descriptor_role_uuid;
  expected.session_uuid = context.prepared_descriptor_session_uuid;
  if (bind_content) expected.content_key = context.prepared_descriptor_content_key;
}

std::vector<std::string> ExpectedAuthorityOptions(const api::InsertBatchContext& context) {
  return {
      "prepared_descriptor.expected_generation=" +
          std::to_string(context.prepared_descriptor_generation),
      "prepared_descriptor.expected_catalog_epoch=" +
          std::to_string(context.prepared_descriptor_catalog_epoch),
      "prepared_descriptor.expected_security_epoch=" +
          std::to_string(context.prepared_descriptor_security_epoch),
      "prepared_descriptor.expected_policy_epoch=" +
          std::to_string(context.prepared_descriptor_policy_epoch),
      "prepared_descriptor.expected_authorization_digest=" +
          context.prepared_descriptor_authorization_digest};
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& entry : evidence) {
    if (entry.evidence_kind == kind && scratchbird::tests::EvidenceTextEquals(entry.evidence_id, id)) {
      return true;
    }
  }
  return false;
}

bool EvidenceContains(const api::EngineApiResult& result, std::string_view kind,
                      const api::EngineUuid& identity) {
  for (const auto& item : result.evidence) {
    const auto* value = std::get_if<api::EngineUuid>(&item.evidence_id);
    if (item.evidence_kind == kind && value && *value == identity) return true;
  }
  return false;
}

bool EvidenceContains(const api::EngineApiResult& result,
                      std::string_view kind,
                      std::string_view needle = {}) {
  for (const auto& entry : result.evidence) {
    if (entry.evidence_kind != kind) {
      continue;
    }
    if (needle.empty() ||
        scratchbird::tests::EvidenceTextFind(entry.evidence_id, needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

template <typename TResult>
bool HasDiagnostic(const TResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

api::EngineTypedValue Value(std::string encoded) {
  api::EngineTypedValue value;
  value.encoded_value = std::move(encoded);
  return value;
}

api::EngineTypedValue TextValue(std::string encoded, bool is_null = false) {
  api::EngineTypedValue value;
  value.descriptor.descriptor_kind = "scalar";
  value.descriptor.canonical_type_name = "character";
  value.descriptor.encoded_descriptor = "canonical=character";
  value.encoded_value = std::move(encoded);
  value.is_null = is_null;
  value.state = is_null ? api::EngineValueState::sql_null
                        : api::EngineValueState::value;
  return value;
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

api::EngineUuid CreateLiveDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779800300000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779800300001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779800300002;
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ":"
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "IPAR prepared validator database create failed");
  live_owner = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  return create.database_uuid.value;
}

api::EngineRequestContext LiveBaseContext(const std::filesystem::path& path,
                                          const api::EngineUuid& database_uuid,
                                          std::string request_id,
                                          api::EngineUuid session_uuid) {
  auto context = live_owner;
  Require(context.database_uuid == database_uuid && context.database_path == path.string(),
          "live fixture owner database mismatch");
  context.request_id = std::move(request_id);
  context.session_uuid = std::move(session_uuid);
  context.current_schema_uuid = scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000001");
  scratchbird::tests::MaterializeBootstrapFixtureAuthorization(context);
  context.resource_epoch = context.authorization_context.policy_epoch;
  const auto security = api::LoadSecurityPrincipalLifecycleState(context);
  Require(security.ok, "live policy catalog read");
  for (const auto& stored : security.state.row_policies) {
    if (stored.deleted || stored.target_object_uuid != kLiveTableUuid) continue;
    api::EngineMaterializedAuthorizationPolicy policy;
    policy.policy_uuid = stored.policy_uuid;
    policy.subject_uuid = context.principal_uuid;
    policy.subject_kind = "principal";
    policy.target_uuid = stored.target_object_uuid;
    policy.right = "INSERT";
    policy.policy_kind = "rls_filter";
    policy.requires_runtime_recheck = true;
    policy.policy_epoch = stored.policy_generation;
    policy.canonical_policy_envelope = stored.predicate_envelope;
    context.authorization_context.policies.push_back(std::move(policy));
  }
  return context;
}

api::EngineRequestContext BeginLiveTransaction(const std::filesystem::path& path,
                                               const api::EngineUuid& database_uuid,
                                               std::string request_id,
                                               api::EngineUuid session_uuid) {
  api::EngineBeginTransactionRequest request;
  request.context = LiveBaseContext(path,
                                    database_uuid,
                                    std::move(request_id),
                                    std::move(session_uuid));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  if (!begun.ok) {
    for (const auto& diagnostic : begun.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(begun.ok, "IPAR prepared validator transaction begin failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void CommitLiveTransaction(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  const auto committed = api::EngineCommitTransaction(request);
  if (!committed.ok) {
    for (const auto& diagnostic : committed.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(committed.ok, "IPAR prepared validator transaction commit failed");
}

api::CrudTableRecord LiveTable() {
  api::CrudTableRecord table;
  table.table_uuid = scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000101");
  table.default_name = "ipar_prepared_validator";
  table.columns.push_back(
      {"id",
       DomainColumnMetadata(kLiveDomainUuid)});
  table.columns.push_back(
      {"payload", "canonical=character;default=literal:empty;check=length_lte:16"});
  table.columns.push_back({"tenant", "canonical=character;not_null=true"});
  return table;
}

api::CrudIndexRecord LiveUniqueIndex() {
  api::CrudIndexRecord index;
  index.index_uuid = scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000201");
  index.table_uuid = scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000101");
  index.default_name = "ipar_prepared_validator_pk";
  index.column_name = "id";
  index.key_envelopes.push_back("id");
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = true;
  return index;
}

api::DomainRecord LiveDomain(std::uint64_t creator_tx) {
  api::DomainRecord record;
  record.creator_tx = creator_tx;
  record.domain_uuid = scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000301");
  record.catalog_row_uuid = scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000302");
  record.schema_uuid = scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000001");
  record.default_name = "ipar_non_negative_int";
  record.base_descriptor_uuid = scratchbird::tests::FixtureUuid(1558, 10);
  record.base_descriptor_kind = "scalar";
  record.base_canonical_type_name = "int64";
  record.base_encoded_descriptor = "canonical=int64";
  record.nullable = false;
  record.check_constraint_envelope = "gte:0";
  record.validation_hook_status = "builtin";
  return record;
}

void SeedLiveValidatorMetadata(api::EngineRequestContext& context) {
  api::EngineCatalogCreateObjectRequest schema;
  schema.context = context;
  schema.target_object.uuid = kLiveSchemaUuid;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back({"en", "primary", "", "prepared_fixture", true});
  Require(api::EngineCatalogCreateObject(schema).ok, "live fixture schema publication");
  auto domain = LiveDomain(context.local_transaction_id);
  api::EngineColumnDefinition column;
  scratchbird::tests::BindFixtureColumnDatatype(context,
      scratchbird::core::datatypes::CanonicalTypeId::int64, column);
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "int64";
  column.descriptor.encoded_descriptor = "nullable=false";
  const auto bound = api::BindDomainScalarBaseDescriptor(context, column.descriptor, &domain);
  if (bound.error) std::cerr << bound.code << ':' << bound.detail << '\n';
  Require(!bound.error, "domain native base descriptor binding");
  api::EngineCreateDomainRequest create_domain;
  create_domain.context = context;
  create_domain.target_object = {scratchbird::tests::FixtureUuid(1558, 3000), "domain"};
  create_domain.target_schema.uuid = kLiveSchemaUuid;
  create_domain.descriptors.push_back(column.descriptor);
  create_domain.localized_names.push_back({"en", "primary", "", "native_bound_domain", true});
  create_domain.option_envelopes = {"nullable:false", "check_constraint:gte:0"};
  const auto created_domain = api::EngineCreateDomain(create_domain);
  if (!created_domain.ok) for (const auto& diagnostic : created_domain.diagnostics)
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  Require(created_domain.ok, "native domain creation with complete base binding");
  auto created_record = api::FindVisibleDomain(context, create_domain.target_object.uuid, context.local_transaction_id);
  api::EngineDescriptor created_base;
  Require(created_record && DecodeInheritedBase(created_record->base_encoded_descriptor, &created_base) &&
          created_base == column.descriptor, "native CREATE DOMAIN persisted full binary binding");
  api::EngineAlterObjectRequest alter_domain;
  alter_domain.context = context;
  alter_domain.target_object = create_domain.target_object;
  alter_domain.descriptors.push_back(column.descriptor);
  ++alter_domain.descriptors.front().datatype_descriptor_generation;
  Require(!api::EngineAlterObject(alter_domain).ok, "native ALTER DOMAIN rejects stale datatype generation");
  const auto after_refusal = api::FindVisibleDomain(context, create_domain.target_object.uuid, context.local_transaction_id);
  Require(after_refusal && after_refusal->base_encoded_descriptor ==
          created_record->base_encoded_descriptor, "failed native domain alteration has no binding effect");
  alter_domain.descriptors.front() = column.descriptor;
  alter_domain.descriptors.front().descriptor_uuid = scratchbird::tests::FixtureUuid(1558, 3001);
  Require(api::EngineAlterObject(alter_domain).ok, "native ALTER DOMAIN admits new exact occurrence binding");
  created_record = api::FindVisibleDomain(context, create_domain.target_object.uuid, context.local_transaction_id);
  Require(created_record && DecodeInheritedBase(created_record->base_encoded_descriptor, &created_base) &&
          created_base == alter_domain.descriptors.front(), "native ALTER DOMAIN persisted complete successor binding");
  auto unchanged = domain;
  auto stale_context = context;
  ++stale_context.datatype_catalog_generation;
  Require(api::BindDomainScalarBaseDescriptor(stale_context, column.descriptor, &unchanged).error &&
          unchanged.base_encoded_descriptor == domain.base_encoded_descriptor,
          "stale cohort must not change a domain binding");
  auto stale_descriptor = column.descriptor;
  ++stale_descriptor.datatype_descriptor_generation;
  Require(api::BindDomainScalarBaseDescriptor(context, stale_descriptor, &unchanged).error &&
          unchanged.base_encoded_descriptor == domain.base_encoded_descriptor,
          "stale datatype generation must not change a domain binding");
  const auto domain_status =
      api::AppendDomainEvent(context, api::MakeDomainCreateEvent(
                                          domain));
  Require(!domain_status.error, "IPAR prepared validator domain seed failed");
  const auto persisted = api::FindVisibleDomain(context, domain.domain_uuid, context.local_transaction_id);
  api::EngineDescriptor recovered;
  Require(persisted && DecodeInheritedBase(persisted->base_encoded_descriptor, &recovered) &&
          recovered == column.descriptor, "full native base descriptor catalog readback");
  api::EngineTypedValue native;
  native.descriptor = recovered;
  native.binary_value = {42, 0, 0, 0, 0, 0, 0, 0};
  const auto valid = api::ValidateDomainTypedValue(context, api::DomainDescriptor(domain), native,
                                                  context.local_transaction_id);
  if (!valid.ok) std::cerr << valid.diagnostic.code << ':' << valid.diagnostic.detail << '\n';
  Require(valid.ok && valid.value.binary_value == native.binary_value && valid.value.encoded_value.empty(),
          "native domain validation must preserve all integer bytes");
  auto malformed = native;
  malformed.binary_value.pop_back();
  Require(!api::ValidateDomainTypedValue(context, api::DomainDescriptor(domain), malformed,
                                        context.local_transaction_id).ok, "reject truncated integer domain value");
  auto with_default = domain;
  with_default.default_expression_envelope = "literal:17";
  Require(!api::AppendDomainEvent(context, api::MakeDomainAlterEvent(with_default)).error,
          "publish exact integer domain default");
  const auto defaulted = api::ApplyDomainRulesToCrudValues(context,
      {{"value", api::DomainColumnDescriptor(domain.domain_uuid)}}, {}, context.local_transaction_id);
  if (!defaulted.ok) std::cerr << defaulted.diagnostic.code << ':' << defaulted.diagnostic.detail << '\n';
  Require(defaulted.ok && defaulted.values.size() == 1 &&
          defaulted.values.front().second.bytes == std::string("\x11\0\0\0\0\0\0\0", 8),
          "integer default must be cast once and stored as LE8");
  with_default.default_expression_envelope = "literal:17.5";
  Require(!api::AppendDomainEvent(context, api::MakeDomainAlterEvent(with_default)).error,
          "publish invalid integer default fixture");
  Require(!api::ApplyDomainRulesToCrudValues(context,
      {{"value", api::DomainColumnDescriptor(domain.domain_uuid)}}, {}, context.local_transaction_id).ok,
      "fractional integer default must not be rounded");
  Require(!api::AppendDomainEvent(context, api::MakeDomainAlterEvent(domain)).error,
          "restore published domain without default");
  const auto table_status = scratchbird::tests::PublishMgaTableFixture(context,
      LiveTable(), {"int64", "character", "character"}, {LiveUniqueIndex()});
  if (table_status.error) std::cerr << table_status.code << ':' << table_status.detail << '\n';
  Require(!table_status.error, "IPAR prepared validator table publication failed");
  Require(!api::AppendMgaIndexMetadata(context, LiveUniqueIndex()).error,
          "IPAR prepared validator index seed failed");
  // Exercise fresh effect admission independently of row-encoder validators.
  const auto batch_for = [&](const api::CrudStoredValue& value) {
    api::MgaExactIndexEntryAppendBatch batch;
    batch.table_uuid=kLiveTableUuid;batch.index=LiveUniqueIndex();
    batch.entries.push_back({api::EncodeStoredLogicalKey({value}), {},
        scratchbird::tests::FixtureUuid(1558,3100), scratchbird::tests::FixtureUuid(1558,3101)});
    return batch;
  };
  const auto admitted_key = [&](const api::CrudStoredValue& value, bool expected) {
    auto batch=batch_for(value);const auto original=batch.entries.front().encoded_key;
    api::EngineApiDiagnostic diagnostic;
    const bool accepted=api::bound_index_key::CanonicalizePublicationBatch(context,&batch,&diagnostic);
    if(accepted!=expected)std::cerr<<diagnostic.code<<':'<<diagnostic.detail<<'\n';
    Require(accepted==expected,"domain inherited key admission result");
    Require(expected ? batch.entries.front().encoded_key.starts_with("SBKOBIN:") :
                       batch.entries.front().encoded_key==original,"domain key output is atomic");
  };
  const api::CrudStoredValue positive(std::string("\x2a\0\0\0\0\0\0\0",8));
  admitted_key(positive,true);
  admitted_key(api::CrudStoredValue(std::string(8,'\xff')),false);
  admitted_key(api::CrudStoredValue::SqlNull(),false);
  auto inner=domain;inner.domain_uuid=scratchbird::tests::FixtureUuid(1558,3102);
  inner.catalog_row_uuid=scratchbird::tests::FixtureUuid(1558,3103);
  inner.check_constraint_envelope="lt:100";
  Require(!api::AppendDomainEvent(context,api::MakeDomainCreateEvent(inner)).error,"publish inherited domain ancestor");
  auto outer=domain;
  auto forged=inner;forged.check_constraint_envelope="lt:200";
  Require(api::BindDomainInnerBaseDescriptor(context,forged,&outer).error &&
          outer.base_encoded_descriptor==domain.base_encoded_descriptor,"forged ancestor cannot bind");
  Require(!api::BindDomainInnerBaseDescriptor(context,inner,&outer).error,"bind actual ancestor");
  Require(!api::AppendDomainEvent(context,api::MakeDomainAlterEvent(outer)).error,"publish domain chain");
  const auto chain=api::ResolveDomainInheritedProfile(context,outer.domain_uuid,context.local_transaction_id);
  Require(chain.ok&&chain.domain_chain==std::vector<api::EngineUuid>{outer.domain_uuid,inner.domain_uuid},
          "complete binary inheritance chain");
  auto permissive=domain;
  permissive.domain_uuid=scratchbird::tests::FixtureUuid(1558,3120);
  permissive.catalog_row_uuid=scratchbird::tests::FixtureUuid(1558,3121);
  permissive.check_constraint_envelope.clear();permissive.nullable=true;
  auto nullable_base=column.descriptor;nullable_base.encoded_descriptor="nullable=true";
  Require(!api::BindDomainScalarBaseDescriptor(context,nullable_base,&permissive).error&&
          !api::AppendDomainEvent(context,api::MakeDomainCreateEvent(permissive)).error,
          "publish permissive target for source-domain admission tests");
  auto tagged=native;tagged.descriptor=api::DomainDescriptor(outer);
  Require(api::ValidateDomainTypedValue(context,api::DomainDescriptor(outer),tagged,context.local_transaction_id).ok,
          "valid self-domain cast");
  Require(api::ValidateDomainTypedValue(context,api::DomainDescriptor(inner),tagged,context.local_transaction_id).ok,
          "valid cast into overlapping ancestor");
  tagged.binary_value.assign(8,0xff);
  Require(!api::ValidateDomainTypedValue(context,api::DomainDescriptor(permissive),tagged,context.local_transaction_id).ok,
          "invalid claimed source domain must not enter permissive target");
  tagged.descriptor=column.descriptor;
  const auto native_negative=api::ValidateDomainTypedValue(context,api::DomainDescriptor(permissive),tagged,context.local_transaction_id);
  if(!native_negative.ok)std::cerr<<native_negative.diagnostic.code<<':'<<native_negative.diagnostic.detail<<'\n';
  Require(native_negative.ok,
          "negative native value is valid for the permissive target");
  tagged=native;tagged.binary_value[0]=200;tagged.descriptor=api::DomainDescriptor(outer);
  Require(!api::ValidateDomainTypedValue(context,api::DomainDescriptor(permissive),tagged,context.local_transaction_id).ok,
          "source ancestor constraint is enforced before target cast");
  tagged.binary_value.clear();tagged.setState(api::EngineValueState::sql_null);
  Require(!api::ValidateDomainTypedValue(context,api::DomainDescriptor(permissive),tagged,context.local_transaction_id).ok,
          "forged NULL source domain cannot bypass source nullability");
  tagged.descriptor=nullable_base;
  Require(api::ValidateDomainTypedValue(context,api::DomainDescriptor(permissive),tagged,context.local_transaction_id).ok,
          "native NULL positive control for nullable target");
  auto restricted_source=inner;restricted_source.cast_policy_envelope="require_right:DOMAIN_TEST_UNGRANTED";
  Require(!api::AppendDomainEvent(context,api::MakeDomainAlterEvent(restricted_source)).error,"publish source cast restriction");
  tagged=native;tagged.descriptor=api::DomainDescriptor(outer);auto denied=context;denied.security_context_present=false;
  Require(!api::ValidateDomainTypedValue(denied,api::DomainDescriptor(permissive),tagged,context.local_transaction_id).ok,
          "source ancestor cast rights cannot be bypassed through permissive target");
  Require(!api::AppendDomainEvent(context,api::MakeDomainAlterEvent(inner)).error,"restore source cast restriction");
  auto deep=inner;
  for(unsigned depth=0;depth<64;++depth) {
    auto next=domain;next.domain_uuid=scratchbird::tests::FixtureUuid(1558,3200+depth*2);
    next.catalog_row_uuid=scratchbird::tests::FixtureUuid(1558,3201+depth*2);
    Require(!api::BindDomainInnerBaseDescriptor(context,deep,&next).error&&
            !api::AppendDomainEvent(context,api::MakeDomainCreateEvent(next)).error,"publish deep chain layer");
    deep=std::move(next);
  }
  Require(api::ValidateDomainTypedValue(context,api::DomainDescriptor(deep),native,context.local_transaction_id).ok,
          "iterative validation covers a chain beyond the former cycle-search cutoff");
  Require(api::DomainChainContainsUuid(context,deep.domain_uuid,inner.domain_uuid,context.local_transaction_id),
          "DDL cycle search traverses every ancestor beyond the former cutoff");
  auto deep_cycle=inner;deep_cycle.base_descriptor_kind="domain";deep_cycle.base_descriptor_uuid=deep.domain_uuid;
  Require(!api::AppendDomainEvent(context,api::MakeDomainAlterEvent(deep_cycle)).error,"publish deep cycle fixture");
  Require(!api::ValidateDomainTypedValue(context,api::DomainDescriptor(deep),native,context.local_transaction_id).ok,
          "deep cycle cannot escape complete chain validation");
  Require(!api::AppendDomainEvent(context,api::MakeDomainAlterEvent(inner)).error,"restore deep chain anchor");
  admitted_key(positive,true);
  admitted_key(api::CrudStoredValue(std::string("\xc8\0\0\0\0\0\0\0",8)),false);
  {
    auto reentrant=context;bool fired=false;
    reentrant.query_cancellation_requested=[&] {
      if(!fired) {
        fired=true;auto restricted=inner;restricted.mutation_policy_envelope="require_right:DOMAIN_TEST_UNGRANTED";
        Require(!api::AppendDomainEvent(context,api::MakeDomainAlterEvent(restricted)).error,"reentrant policy change");
      }
      return false;
    };
    const auto refused=api::ApplyDomainRulesToCrudValues(reentrant,
        {{"value",api::DomainColumnDescriptor(outer.domain_uuid)}},{{"value",positive}},context.local_transaction_id);
    Require(fired&&!refused.ok,"row validation cannot publish a value after reentrant policy change");
    Require(!api::AppendDomainEvent(context,api::MakeDomainAlterEvent(inner)).error,"restore reentrant policy fixture");
    fired=false;auto batch=batch_for(positive);const auto original=batch.entries.front().encoded_key;
    api::EngineApiDiagnostic diagnostic;
    Require(!api::bound_index_key::CanonicalizePublicationBatch(reentrant,&batch,&diagnostic)&&fired&&
            batch.entries.front().encoded_key==original,"index rejects reentrant domain policy change without output");
    Require(!api::AppendDomainEvent(context,api::MakeDomainAlterEvent(inner)).error,"restore reentrant index fixture");
  }
  {
    std::promise<void> started;
    auto started_future=started.get_future();
    std::future<api::EngineApiDiagnostic> publisher;
    bool launched=false, excluded=false;
    auto concurrent=context;
    concurrent.query_cancellation_requested=[&] {
      if(!launched) {
        launched=true;
        publisher=std::async(std::launch::async,[&] {
          auto changed=inner;changed.numeric_metadata="concurrent_override";
          started.set_value();
          return api::AppendDomainEvent(context,api::MakeDomainAlterEvent(changed));
        });
        started_future.wait();
        excluded=publisher.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout;
      }
      return false;
    };
    auto batch=batch_for(positive);api::EngineApiDiagnostic diagnostic;
    const bool accepted=api::bound_index_key::CanonicalizePublicationBatch(concurrent,&batch,&diagnostic);
    Require(accepted&&launched&&excluded,"concurrent domain publication must wait for admitted key derivation");
    Require(publisher.wait_for(std::chrono::seconds(10))==std::future_status::ready&&!publisher.get().error,
            "domain publisher completes after admission guard release");
    admitted_key(positive,false);
    Require(!api::AppendDomainEvent(context,api::MakeDomainAlterEvent(inner)).error,"restore concurrent profile fixture");
  }
  for(const auto member:{&api::DomainRecord::charset_or_collation_ref,&api::DomainRecord::numeric_metadata,
                         &api::DomainRecord::method_binding_envelope}) {
    for(bool ancestor:{false,true}) {
      auto overridden=ancestor?inner:outer;overridden.*member="unresolved_override";
      Require(!api::AppendDomainEvent(context,api::MakeDomainAlterEvent(overridden)).error,"publish override fixture");
      admitted_key(positive,false);
      Require(!api::ValidateDomainTypedValue(context,api::DomainDescriptor(outer),native,context.local_transaction_id).ok,
              "target native validation cannot discard an explicit ancestor override");
      auto source=native;source.descriptor=api::DomainDescriptor(ancestor?outer:overridden);
      Require(!api::ValidateDomainTypedValue(context,api::DomainDescriptor(permissive),source,context.local_transaction_id).ok,
              "source native validation cannot discard an explicit ancestor override");
      Require(!api::AppendDomainEvent(context,api::MakeDomainAlterEvent(ancestor?inner:outer)).error,"restore inherited profile");
    }
  }
  for(bool ancestor:{false,true}) {
    for(const std::string policy:{"require_right:DOMAIN_TEST_UNGRANTED", "unknown_policy", "require_right:"}) {
      auto restricted=ancestor?inner:outer;restricted.mutation_policy_envelope=policy;
      Require(!api::AppendDomainEvent(context,api::MakeDomainAlterEvent(restricted)).error,"publish restriction fixture");
      // Drop ambient privileges for the permission assertion; catalog identity
      // and transaction observation remain the real owning context.
      auto denied=context;denied.security_context_present=false;
      Require(api::AdmitDomainMutationChain(denied,outer.domain_uuid,context.local_transaction_id).error,
              "every domain ancestor mutation restriction is enforced");
      if(policy!="require_right:DOMAIN_TEST_UNGRANTED")admitted_key(positive,false);
      Require(!api::AppendDomainEvent(context,api::MakeDomainAlterEvent(ancestor?inner:outer)).error,"restore domain restriction");
    }
  }
  auto cycle=inner;cycle.base_descriptor_kind="domain";cycle.base_descriptor_uuid=outer.domain_uuid;
  Require(!api::AppendDomainEvent(context,api::MakeDomainAlterEvent(cycle)).error,"publish cyclic domain fixture");
  admitted_key(positive,false);
  Require(!api::AppendDomainEvent(context,api::MakeDomainAlterEvent(inner)).error,"restore acyclic chain");
  {
    api::MgaRelationHotAppendContext pending(context);
    Require(!pending.AppendExactIndexEntryBatches({batch_for(positive)}).error,"stage inherited key");
    auto changed=inner;changed.check_constraint_envelope="lt:40";
    Require(!api::AppendDomainEvent(context,api::MakeDomainAlterEvent(changed)).error,"change ancestor after staging");
    const auto refused=pending.FlushIndexEntries();
    Require(refused.error&&refused.detail.find("binding_changed_before_flush")!=std::string::npos,
            "staged binding must not authorize flush after ancestor change");
    Require(!api::AppendDomainEvent(context,api::MakeDomainAlterEvent(inner)).error,"restore ancestor after flush refusal");
  }
  auto stale=inner;api::DomainInheritedBaseBindingV1 stale_binding;
  Require(api::DecodeDomainInheritedBaseBindingV1(stale.base_encoded_descriptor,&stale_binding),"decode profile for stale test");
  ++stale_binding.registry_generation;
  Require(api::EncodeDomainInheritedBaseBindingV1(stale_binding,&stale.base_encoded_descriptor),"encode structurally valid stale profile");
  Require(!api::AppendDomainEvent(context,api::MakeDomainAlterEvent(stale)).error,"publish stale profile fixture");
  admitted_key(positive,false);
  Require(!api::AppendDomainEvent(context,api::MakeDomainAlterEvent(inner)).error,"restore registered profile");
  Require(!api::AppendDomainEvent(context,api::MakeDomainAlterEvent(domain)).error,"restore scalar domain after chain tests");
  api::EngineSecurityPutRowPolicyRequest policy;
  policy.context = context;
  policy.policy_uuid = scratchbird::tests::FixtureUuid(1558, 502);
  policy.target_object_uuid = kLiveTableUuid;
  policy.target_object_kind = "table";
  policy.policy_effect = "row_filter";
  policy.predicate_envelope = "sblr_predicate:column_equals:tenant:tenant_a";
  policy.definer_principal_uuid = context.principal_uuid;
  const auto created = api::EngineSecurityPutRowPolicy(policy);
  if (!created.ok) for (const auto& diagnostic : created.diagnostics)
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  Require(created.ok && created.policy_persisted, "live row policy publication failed");
}

api::EngineRowValue LiveRow(api::EngineUuid row_uuid,
                            std::vector<std::pair<std::string, api::EngineTypedValue>> fields) {
  api::EngineRowValue row;
  row.requested_row_uuid = std::move(row_uuid);
  row.fields = std::move(fields);
  return row;
}

scratchbird::tests::FixtureEngineRequest<api::EngineInsertRowsRequest> LiveInsertRequest(
                                       const scratchbird::tests::FixtureEngineSession& session,
                                       const api::EngineRequestContext& context,
                                       api::EngineRowValue row) {
  scratchbird::tests::FixtureEngineRequest<api::EngineInsertRowsRequest> request(session, context);
  request.target_schema.uuid = scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000001");
  request.target_table.uuid = scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000101");
  request.target_table.object_kind = "table";
  request.target_object.uuid = scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000101");
  request.target_object.object_kind = "table";
  request.bound_object_identity.object_uuid = request.target_table.uuid;
  request.bound_object_identity.catalog_generation_id =
      context.catalog_generation_id;
  request.bound_object_identity.security_epoch = context.security_epoch;
  request.bound_object_identity.resource_epoch = context.resource_epoch;
  request.estimated_row_count = 1;
  // This gate asserts generic prepared-descriptor validator evidence.  Keep
  // it on that route; direct physical bulk insertion has distinct,
  // engine-owned preflight-proof evidence and is covered separately.
  request.option_envelopes.push_back("direct_physical_insert=disabled");
  request.input_rows.push_back(std::move(row));
  return request;
}

api::EngineInsertRowsResult LiveInsert(const scratchbird::tests::FixtureEngineSession& session,
                                      const api::EngineRequestContext& context, api::EngineRowValue row) {
  const auto request=LiveInsertRequest(session,context,std::move(row));
  return api::EngineInsertRows(request);
}

void RequireRefusal(const api::InsertBatchContext& context,
                    std::string_view reason) {
  Require(!context.accepted, "IPAR prepared descriptor stale handle was accepted");
  Require(context.prepared_descriptor_authority_refused,
          "IPAR prepared descriptor did not mark authority refusal");
  if (context.prepared_descriptor_refusal_reason != reason)
    std::cerr << "expected refusal " << reason << ", got "
              << context.prepared_descriptor_refusal_reason << '\n';
  Require(context.prepared_descriptor_refusal_reason == reason,
          "IPAR prepared descriptor refusal reason mismatch");
  api::EngineApiResult result;
  api::AddInsertBatchEvidenceToResult(context, &result);
  Require(HasEvidence(result.evidence, "prepared_descriptor_authority_refusal", reason),
          "IPAR prepared descriptor refusal evidence missing");
  Require(HasEvidence(result.evidence, "prepared_descriptor_refused_before_execution", "true"),
          "IPAR prepared descriptor did not prove before-execution refusal");
}

api::InsertBatchContext Begin(const api::EngineInsertRowsRequest& request,
                              const api::CrudTableRecord& table) {
  const auto state = State(table);
  const std::vector<api::CrudIndexRecord> indexes{Index(table.table_uuid)};
  return api::BeginInsertBatchContext(request, state, table, indexes);
}

void ValidateSameGroupChainReusesAuthorizationDescriptor() {
  const auto table = Table();
  const auto request = InsertRequest(Context("ipar-prepared-cache-reuse"));
  const auto first = Begin(request, table);
  if (!first.accepted) for (const auto& diagnostic : first.diagnostics)
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  Require(first.accepted, "IPAR prepared descriptor first bind refused");
  Require(!first.prepared_descriptor_cache_hit,
          "IPAR prepared descriptor first bind unexpectedly hit cache");

  const auto second = Begin(request, table);
  Require(second.accepted, "IPAR prepared descriptor second bind refused");
  Require(second.prepared_descriptor_cache_hit,
          "IPAR prepared descriptor did not reuse same user/role/group chain");
  Require(first.prepared_descriptor_authorization_digest ==
              second.prepared_descriptor_authorization_digest,
          "IPAR prepared descriptor authorization digest drifted for same group chain");
  Require(first.row_encoder_plan.plan_id == second.row_encoder_plan.plan_id,
          "IPAR row encoder plan was not reused with prepared descriptor");
  Require(first.row_encoder_plan.row_shape_signature ==
              second.row_encoder_plan.row_shape_signature,
          "IPAR row encoder shape signature drifted on cache reuse");
  Require(first.row_encoder_plan.validator_signature ==
              second.row_encoder_plan.validator_signature,
          "IPAR row validator signature drifted on cache reuse");
  Require(first.row_encoder_plan.column_count == 2,
          "IPAR row encoder did not bind expected column count");
  Require(first.row_encoder_plan.default_validator_count == 1,
          "IPAR row encoder did not bind default validator count");
  Require(first.row_encoder_plan.domain_validator_count == 1,
          "IPAR row encoder did not bind domain validator count");
  Require(first.row_encoder_plan.check_validator_count == 2,
          "IPAR row encoder did not bind check validator count");
  Require(first.row_encoder_plan.not_null_validator_count == 1,
          "IPAR row encoder did not bind not-null validator count");
  Require(first.row_encoder_plan.unique_validator_count == 1,
          "IPAR row encoder did not bind unique validator count");
  Require(first.row_encoder_plan.runtime_policy_recheck_count == 1,
          "IPAR row encoder did not bind runtime security recheck count");
  Require(first.row_encoder_plan.unsupported_sblr_validators_fail_closed,
          "IPAR row encoder did not mark unsupported SBLR validators fail-closed");

  api::EngineRowValue row;
  row.fields.push_back({"payload", Value("payload-first")});
  row.fields.push_back({"id", Value("42")});
  const auto prepared = api::PrepareInsertRowForBatch(request,
                                                     row,
                                                     first.row_template,
                                                     first.row_encoder_plan);
  Require(prepared.values.size() == 2,
          "IPAR row encoder prepared unexpected value count");
  Require(prepared.values[0].first == "id" &&
              prepared.values[1].first == "payload",
          "IPAR row encoder did not use cached descriptor column order");

  api::EngineApiResult first_evidence;
  api::AddInsertBatchEvidenceToResult(first, &first_evidence);
  Require(HasEvidence(first_evidence.evidence,
                      "insert_row_encoder_descriptor_state",
                      "compiled"),
          "IPAR row encoder compile evidence missing");
  Require(HasEvidence(first_evidence.evidence,
                      "insert_validator_runtime_policy_recheck_count",
                      "1"),
          "IPAR row encoder security policy evidence missing");

  api::EngineApiResult second_evidence;
  api::AddInsertBatchEvidenceToResult(second, &second_evidence);
  Require(HasEvidence(second_evidence.evidence,
                      "insert_row_encoder_descriptor_state",
                      "reused"),
          "IPAR row encoder reuse evidence missing");
}

void ValidateRowEncoderInvalidatesOnShapeChange() {
  const auto table = Table();
  const auto request = InsertRequest(Context("ipar-row-encoder-shape-base"));
  const auto first = Begin(request, table);
  Require(first.accepted, "IPAR row encoder shape base refused");

  auto changed_table = table;
  changed_table.columns.push_back({"shape_extra", "canonical=int64;default=literal:7"});
  const auto changed = Begin(request, changed_table);
  Require(changed.accepted, "IPAR row encoder shape change refused");
  Require(!changed.prepared_descriptor_cache_hit,
          "IPAR row encoder shape change incorrectly hit descriptor cache");
  Require(first.row_encoder_plan.plan_id != changed.row_encoder_plan.plan_id,
          "IPAR row encoder plan did not change after table shape changed");
  Require(first.row_encoder_plan.row_shape_signature !=
              changed.row_encoder_plan.row_shape_signature,
          "IPAR row encoder shape signature did not change after table shape changed");
}

void ValidateEpochAndAuthorityRefusals() {
  const auto table = Table();
  const auto base = Begin(InsertRequest(Context("ipar-prepared-cache-base")), table);
  Require(base.accepted, "IPAR prepared descriptor base context refused");

  auto stale_security_options = ExpectedAuthorityOptions(base);
  auto stale_security = InsertRequest(
      Context("ipar-prepared-cache-stale-security",
              scratchbird::tests::FixtureUuid(1558, 1),
              scratchbird::tests::FixtureUuid(1558, 2),
              scratchbird::tests::FixtureUuid(1558, 3),
              scratchbird::tests::FixtureUuid(1558, 4),
              base.prepared_descriptor_catalog_epoch,
              base.prepared_descriptor_security_epoch + 1,
              base.prepared_descriptor_policy_epoch),
      stale_security_options);
  // The complete binary key includes epochs and authorization. Exercise both
  // specific scalar expectation refusal and the earlier full-key mismatch.
  BindExpectedAuthority(&stale_security, base, false);
  RequireRefusal(Begin(stale_security, table), "stale_security_epoch");
  BindExpectedAuthority(&stale_security, base);
  RequireRefusal(Begin(stale_security, table), "stale_descriptor_key");

  auto cross_session = InsertRequest(
      Context("ipar-prepared-cache-cross-session",
              scratchbird::tests::FixtureUuid(1558, 1),
              scratchbird::tests::FixtureUuid(1558, 12)),
      ExpectedAuthorityOptions(base));
  BindExpectedAuthority(&cross_session, base);
  RequireRefusal(Begin(cross_session, table), "cross_session");

  auto cross_user = InsertRequest(
      Context("ipar-prepared-cache-cross-user",
              scratchbird::tests::FixtureUuid(1558, 11),
              scratchbird::tests::FixtureUuid(1558, 2)),
      ExpectedAuthorityOptions(base));
  BindExpectedAuthority(&cross_user, base);
  RequireRefusal(Begin(cross_user, table), "cross_user");

  auto changed_group = InsertRequest(
      Context("ipar-prepared-cache-group-change",
              scratchbird::tests::FixtureUuid(1558, 1),
              scratchbird::tests::FixtureUuid(1558, 2),
              scratchbird::tests::FixtureUuid(1558, 3),
              scratchbird::tests::FixtureUuid(1558, 13)),
      ExpectedAuthorityOptions(base));
  BindExpectedAuthority(&changed_group, base, false);
  RequireRefusal(Begin(changed_group, table), "authorization_context_changed");
  BindExpectedAuthority(&changed_group, base);
  RequireRefusal(Begin(changed_group, table), "stale_descriptor_key");

  auto lease_options = ExpectedAuthorityOptions(base);
  lease_options.push_back("prepared_descriptor.lease_expires_at_epoch=10");
  lease_options.push_back("prepared_descriptor.current_lease_epoch=11");
  auto lease_expired =
      InsertRequest(Context("ipar-prepared-cache-lease-expired"), lease_options);
  BindExpectedAuthority(&lease_expired, base);
  RequireRefusal(Begin(lease_expired, table), "lease_expired");
}

void ValidateEvictionGenerationRefusal() {
  const auto table_a = Table(scratchbird::tests::FixtureUuid(1558, 14));
  auto request_a = InsertRequest(Context("ipar-prepared-cache-evict-a"),
                                 {"prepared_descriptor.cache_limit=1"});
  request_a.target_table.uuid = table_a.table_uuid;
  request_a.target_object.uuid = table_a.table_uuid;
  request_a.bound_object_identity.object_uuid = request_a.target_table.uuid;
  const auto first_a = Begin(request_a, table_a);
  Require(first_a.accepted, "IPAR eviction first descriptor refused");

  const auto table_b = Table(scratchbird::tests::FixtureUuid(1558, 15));
  auto request_b = InsertRequest(Context("ipar-prepared-cache-evict-b"),
                                 {"prepared_descriptor.cache_limit=1"});
  request_b.target_table.uuid = table_b.table_uuid;
  request_b.target_object.uuid = table_b.table_uuid;
  request_b.bound_object_identity.object_uuid = request_b.target_table.uuid;
  const auto first_b = Begin(request_b, table_b);
  Require(first_b.accepted, "IPAR eviction second descriptor refused");
  Require(first_b.prepared_descriptor_eviction_count >
              first_a.prepared_descriptor_eviction_count,
          "IPAR descriptor cache eviction counter did not advance");

  auto rebound_options = ExpectedAuthorityOptions(first_a);
  rebound_options.push_back("prepared_descriptor.cache_limit=1");
  auto rebound_request =
      InsertRequest(Context("ipar-prepared-cache-evict-a-rebound"), rebound_options);
  rebound_request.target_table.uuid = table_a.table_uuid;
  rebound_request.target_object.uuid = table_a.table_uuid;
  rebound_request.bound_object_identity.object_uuid = rebound_request.target_table.uuid;
  BindExpectedAuthority(&rebound_request, first_a);
  RequireRefusal(Begin(rebound_request, table_a), "evicted_or_rebound");
}

void ValidatePreparedDescriptorExecutesLiveValidators() {
  scratchbird::tests::OwnedTempDirectory directory;
  const auto path = directory.path() / "prepared.sbdb";
  const auto database_uuid = CreateLiveDatabase(path);

  auto setup = BeginLiveTransaction(path,
                                    database_uuid,
                                    "ipar-prepared-validator-setup",
                                    scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000601"));
  SeedLiveValidatorMetadata(setup);
  CommitLiveTransaction(setup);

  auto writer = BeginLiveTransaction(path,
                                     database_uuid,
                                     "ipar-prepared-validator-writer",
                                     scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000602"));
  {
  scratchbird::tests::FixtureEngineSession session(writer);
  const auto first = LiveInsert(
      session, writer,
      LiveRow(scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000701"),
              {{"id", TextValue("42")},
               {"tenant", TextValue("tenant_a")}}));
  if (!first.ok) {
    for (const auto& diagnostic : first.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(first.ok, "IPAR prepared validator first live insert failed");
  Require(EvidenceContains(first, "insert_row_encoder_descriptor_state", "compiled"),
          "IPAR live first insert did not compile prepared descriptor");
  Require(EvidenceContains(first, "constraint_default", "payload"),
          "IPAR live default validator evidence missing");
  Require(EvidenceContains(first, "domain_validation", kLiveDomainUuid),
          "IPAR live domain validator evidence missing");
  Require(EvidenceContains(first, "domain_check", kLiveDomainUuid),
          "IPAR live domain check evidence missing");
  Require(EvidenceContains(first, "constraint_not_null", "id"),
          "IPAR live not-null validator evidence missing");
  Require(EvidenceContains(first, "constraint_check", "id"),
          "IPAR live check validator evidence missing");
  Require(EvidenceContains(first, "constraint_key_support", kLiveIndexUuid),
          "IPAR live unique validator evidence missing");
  Require(EvidenceContains(first, "insert_runtime_security_recheck", "rls=filter"),
          "IPAR live RLS runtime recheck evidence missing");
  bool saw_payload_default = false;
  for (const auto& [field, typed] : first.result_shape.rows.front().fields) {
    if (field == "payload" && typed.encoded_value == "empty") {
      saw_payload_default = true;
    }
  }
  Require(saw_payload_default, "IPAR live default was not materialized");

  auto second_request = LiveInsertRequest(
      session, writer,
      LiveRow(scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000702"),
              {{"id", TextValue("43")},
               {"payload", TextValue("short")},
               {"tenant", TextValue("tenant_a")}}));
  const auto fresh_statement=api::EngineInsertRows(second_request);
  Require(fresh_statement.ok,"fresh statement insert failed");
  Require(EvidenceContains(fresh_statement,"insert_row_encoder_descriptor_state","compiled"),
          "a different statement snapshot must not reuse prior publication authority");
  // A second row-producing fragment of this SAME admitted statement may reuse
  // its template. Keep the engine-issued receipt alive; never forge/copy a
  // snapshot identity into a newly admitted statement to manufacture a hit.
  second_request.input_rows.front()=LiveRow(scratchbird::tests::FixtureUuid(1558,3104),
      {{"id",TextValue("47")},{"payload",TextValue("short")},{"tenant",TextValue("tenant_a")}});
  const auto second=api::EngineInsertRows(second_request);
  Require(second.ok, "IPAR prepared validator second live insert failed");
  Require(EvidenceContains(second, "insert_row_encoder_descriptor_state", "reused"),
          "IPAR live second insert did not reuse prepared descriptor");
  Require(EvidenceContains(second, "insert_memory_arena_reuse_claim",
                           "prepared_descriptor_cache_reuse"),
          "IPAR live second insert did not publish prepared reuse memory evidence");
  Require(EvidenceContains(second, "insert_memory_arena_reuse_physical_arena_claimed",
                           "false"),
          "IPAR descriptor cache hit falsely claimed physical arena reuse");
  Require(EvidenceContains(second, "insert_memory_arena_measurement_scope",
                           "request_local_allocation_lifecycle_probe") &&
              EvidenceContains(second, "insert_memory_arena_execution_workspace_covered", "false"),
          "IPAR allocation probe was promoted to execution-workspace authority");
  Require(EvidenceContains(second, "insert_memory_arena_grant_state", "granted"),
          "IPAR live second insert did not grant query memory arena scratch");
  Require(EvidenceContains(second, "insert_memory_arena_release_state", "released"),
          "IPAR live second insert did not release query memory arena scratch");
  Require(EvidenceContains(second, "insert_memory_arena_reset_state", "reset"),
          "IPAR live second insert did not reset query memory arena scratch");
  Require(EvidenceContains(second, "insert_memory_arena_fail_closed", "false"),
          "IPAR live second insert fail-closed the query memory arena");
  Require(EvidenceContains(second, "insert_memory_arena_leak_count", "0"),
          "IPAR live second insert leaked query memory arena scratch");
  Require(EvidenceContains(second, "constraint_check", "payload"),
          "IPAR live reused descriptor did not execute payload check");
  Require(EvidenceContains(second, "insert_runtime_security_recheck", "rls=filter"),
          "IPAR live reused descriptor skipped RLS runtime recheck");

  const auto domain_refusal = LiveInsert(
      session, writer,
      LiveRow(scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000703"),
              {{"id", TextValue("-1")},
               {"tenant", TextValue("tenant_a")}}));
  Require(!domain_refusal.ok,
          "IPAR live negative domain value was admitted");
  Require(HasDiagnostic(domain_refusal, "SBSQL_DOMAIN_CHECK_VIOLATION"),
          "IPAR live domain refusal diagnostic mismatch");

  const auto not_null_refusal = LiveInsert(
      session, writer,
      LiveRow(scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000704"),
              {{"id", TextValue("44")},
               {"payload", TextValue("short")}}));
  Require(!not_null_refusal.ok,
          "IPAR live missing tenant was admitted");
  Require(HasDiagnostic(not_null_refusal, "CLI.CONSTRAINT_NOT_NULL_VIOLATION"),
          "IPAR live not-null refusal diagnostic mismatch");

  const auto check_refusal = LiveInsert(
      session, writer,
      LiveRow(scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000705"),
              {{"id", TextValue("45")},
               {"payload", TextValue("payload-too-long-for-check")},
               {"tenant", TextValue("tenant_a")}}));
  Require(!check_refusal.ok,
          "IPAR live check violation was admitted");
  Require(HasDiagnostic(check_refusal, "CLI.CONSTRAINT_CHECK_VIOLATION"),
          "IPAR live check refusal diagnostic mismatch");

  const auto duplicate_refusal = LiveInsert(
      session, writer,
      LiveRow(scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000706"),
              {{"id", TextValue("42")},
               {"tenant", TextValue("tenant_a")}}));
  Require(!duplicate_refusal.ok,
          "IPAR live duplicate unique value was admitted");
  Require(HasDiagnostic(duplicate_refusal, "CLI.CONSTRAINT_PRIMARY_KEY_VIOLATION"),
          "IPAR live unique refusal diagnostic mismatch");

  const auto rls_refusal = LiveInsert(
      session, writer,
      LiveRow(scratchbird::tests::FixtureUuidLiteral("019f3000-0000-7000-8000-000000000707"),
              {{"id", TextValue("46")},
               {"payload", TextValue("short")},
               {"tenant", TextValue("tenant_b")}}));
  Require(!rls_refusal.ok,
          "IPAR live RLS-denied row was admitted");
  Require(HasDiagnostic(rls_refusal, "SECURITY.RLS.DENIED"),
          "IPAR live RLS refusal diagnostic mismatch");
  Require(EvidenceContains(rls_refusal, "insert_runtime_security_policy_result",
                           "column_equals:tenant:deny"),
          "IPAR live RLS refusal evidence missing");

  CommitLiveTransaction(writer);
  }
  auto reader=BeginLiveTransaction(path,database_uuid,"ipar-domain-committed-readback",
      scratchbird::tests::FixtureUuid(1558,3105));
  const auto stored=api::LoadMgaRelationStoreStateForRelationScans(reader,{kLiveTableUuid});
  Require(stored.ok,"load committed domain rows and index memberships");
  std::vector<unsigned> stored_ids;
  for(const auto& row:stored.state.row_versions) {
    if(row.table_uuid!=kLiveTableUuid)continue;
    Require(!row.deleted&&row.creator_tx==writer.local_transaction_id,"only successful domain writes persisted");
    const auto id=std::find_if(row.values.begin(),row.values.end(),[](const auto& field){return field.first=="id";});
    Require(id!=row.values.end()&&id->second.isPresent()&&id->second.bytes.size()==8&&
            id->second.bytes.substr(1)==std::string(7,'\0'),"stored domain integer remains native LE8");
    stored_ids.push_back(static_cast<unsigned char>(id->second.bytes.front()));
  }
  std::sort(stored_ids.begin(),stored_ids.end());
  Require(stored_ids==std::vector<unsigned>{42,43,47},"failed validators did not publish domain rows");
  std::size_t memberships=0;
  for(const auto& entry:stored.state.index_entries)if(entry.index_uuid==kLiveIndexUuid) {
    ++memberships;
    Require(entry.creator_tx==writer.local_transaction_id&&entry.key_value.starts_with("SBKOBIN:"),
            "committed domain index uses admitted binary ordered keys");
  }
  Require(memberships==3,"failed domain admission and flush did not publish index entries");
  const auto recovered_domain=api::ResolveDomainInheritedProfile(reader,kLiveDomainUuid,reader.local_transaction_id);
  Require(recovered_domain.ok&&recovered_domain.binary_profile_binding.starts_with("SBDPFB01"),
          "committed inherited binary profile remains resolvable");
  CommitLiveTransaction(reader);
  directory.Cleanup();
}

}  // namespace

int main() {
  try {
  namespace mem = scratchbird::core::memory;
  Require(mem::ConfigureDefaultMemoryManagerForFixture(mem::DefaultLocalEngineMemoryPolicy(),
      "ipar-prepared-cache").ok(), "prepared-cache memory fixture configuration");
  ValidateSameGroupChainReusesAuthorizationDescriptor();
  ValidateRowEncoderInvalidatesOnShapeChange();
  ValidateEpochAndAuthorityRefusals();
  ValidateEvictionGenerationRefusal();
  ValidatePreparedDescriptorExecutesLiveValidators();
  return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
