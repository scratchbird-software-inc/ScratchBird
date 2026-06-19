// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/insert_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr const char* kSchemaUuid = "019f2000-0000-7000-8000-000000000001";
constexpr const char* kTableUuid = "019f2000-0000-7000-8000-000000000101";
constexpr const char* kPrincipalUuid = "019f2000-0000-7000-8000-000000000201";
constexpr const char* kGroupUuid = "019f2000-0000-7000-8000-000000000202";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
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

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view needle = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind != kind) {
      continue;
    }
    if (needle.empty() ||
        evidence.evidence_id.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::filesystem::path MakeTempPath() {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  return std::filesystem::temp_directory_path() /
         ("sb_ipar_runtime_security_" + std::to_string(now) + "_" +
          std::to_string(static_cast<long long>(getpid())) + ".sbdb");
}

std::string CreateDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779800200000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779800200001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779800200002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ":"
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "IPAR runtime security database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineAuthorizationSubject Subject(std::string uuid, std::string kind) {
  api::EngineAuthorizationSubject subject;
  subject.subject_uuid.canonical = std::move(uuid);
  subject.subject_kind = std::move(kind);
  return subject;
}

api::EngineRequestContext BaseContext(const std::filesystem::path& path,
                                      const std::string& database_uuid,
                                      std::string session_suffix) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "ipar-runtime-security";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.principal_uuid.canonical = kPrincipalUuid;
  context.session_uuid.canonical =
      "019f2000-0000-7000-8000-000000000" + std::move(session_suffix);
  context.current_schema_uuid.canonical = kSchemaUuid;
  context.default_root_uuid.canonical = "019f2000-0000-7000-8000-000000000203";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      "019f2000-0000-7000-8000-000000000204";
  context.authorization_context.principal_uuid = context.principal_uuid;
  context.authorization_context.security_epoch = context.security_epoch;
  context.authorization_context.policy_epoch = context.resource_epoch;
  context.authorization_context.catalog_generation_id = context.catalog_generation_id;
  context.authorization_context.effective_subjects.push_back(
      Subject(kPrincipalUuid, "principal"));
  context.authorization_context.effective_subjects.push_back(
      Subject(kGroupUuid, "group"));

  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical = "019f2000-0000-7000-8000-000000000301";
  grant.subject_uuid.canonical = kGroupUuid;
  grant.subject_kind = "group";
  grant.target_uuid.canonical = kTableUuid;
  grant.right = "INSERT";
  grant.security_epoch = context.security_epoch;
  context.authorization_context.grants.push_back(std::move(grant));

  api::EngineMaterializedAuthorizationPolicy policy;
  policy.policy_uuid.canonical = "019f2000-0000-7000-8000-000000000302";
  policy.subject_uuid.canonical = kGroupUuid;
  policy.subject_kind = "group";
  policy.target_uuid.canonical = kTableUuid;
  policy.right = "INSERT";
  policy.policy_kind = "rls_filter";
  policy.requires_runtime_recheck = true;
  policy.policy_epoch = context.resource_epoch;
  policy.canonical_policy_envelope = "sblr_predicate:column_equals:tenant:tenant_a";
  context.authorization_context.policies.push_back(std::move(policy));
  return context;
}

api::EngineRequestContext Begin(const std::filesystem::path& path,
                                const std::string& database_uuid,
                                std::string session_suffix) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(path, database_uuid, std::move(session_suffix));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  if (!begun.ok) {
    for (const auto& diagnostic : begun.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(begun.ok, "IPAR runtime security begin failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  const auto committed = api::EngineCommitTransaction(request);
  Require(committed.ok, "IPAR runtime security commit failed");
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.descriptor.encoded_descriptor = "type=text";
  typed.encoded_value = std::move(value);
  typed.state = api::EngineValueState::value;
  return typed;
}

api::EngineRowValue Row(std::string row_uuid, std::string tenant) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  row.fields.push_back({"payload", TextValue("payload")});
  row.fields.push_back({"tenant", TextValue(std::move(tenant))});
  return row;
}

void SeedMetadata(const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.table_uuid = kTableUuid;
  table.default_name = "ipar_runtime_security";
  table.columns.push_back({"tenant", "type=text;nullable=false"});
  table.columns.push_back({"payload", "type=text"});
  Require(!api::AppendMgaTableMetadata(context, table).error,
          "IPAR runtime security table metadata append failed");
}

api::EngineInsertRowsResult Insert(const api::EngineRequestContext& context,
                                   api::EngineRowValue row) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = kTableUuid;
  request.target_table.object_kind = "table";
  request.target_object.uuid.canonical = kTableUuid;
  request.target_object.object_kind = "table";
  request.input_rows.push_back(std::move(row));
  return api::EngineInsertRows(request);
}

void VerifyRuntimeSecurityRecheck() {
  const auto path = MakeTempPath();
  const auto database_uuid = CreateDatabase(path);

  auto setup = Begin(path, database_uuid, "401");
  SeedMetadata(setup);
  Commit(setup);

  auto allowed_context = Begin(path, database_uuid, "402");
  const auto allowed = Insert(
      allowed_context,
      Row("019f2000-0000-7000-8000-000000000501", "tenant_a"));
  if (!allowed.ok) {
    for (const auto& diagnostic : allowed.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(allowed.ok, "IPAR runtime security allowed insert failed");
  Require(HasEvidence(allowed, "insert_runtime_security_policy_evaluated"),
          "IPAR runtime security policy evaluation evidence missing");
  Require(HasEvidence(allowed, "insert_runtime_security_recheck", "rls=filter"),
          "IPAR runtime security filter recheck evidence missing");
  Commit(allowed_context);

  auto denied_context = Begin(path, database_uuid, "403");
  const auto denied = Insert(
      denied_context,
      Row("019f2000-0000-7000-8000-000000000502", "tenant_b"));
  Require(!denied.ok, "IPAR runtime security denied insert was admitted");
  Require(HasDiagnostic(denied, "SECURITY.RLS.DENIED"),
          "IPAR runtime security denied insert diagnostic mismatch");
  Require(HasEvidence(denied, "insert_runtime_security_policy_result",
                      "column_equals:tenant:deny"),
          "IPAR runtime security deny evidence missing");
  Require(HasEvidence(denied, "security_deep_enforcement_refusal",
                      "rls_policy=deny"),
          "IPAR runtime security deep refusal evidence missing");

  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(path.string() + ".sb.mga", ignored);
  std::filesystem::remove(path.string() + ".sb.behavior", ignored);
}

}  // namespace

int main() {
  VerifyRuntimeSecurityRecheck();
  return EXIT_SUCCESS;
}
