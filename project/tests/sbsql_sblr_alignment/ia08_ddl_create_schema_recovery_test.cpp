// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/internal_api/sblr_ddl_create_schema_execution_journal.hpp"
#include "catalog/name_registry.hpp"
#include "catalog/schema_tree_api.hpp"
#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace api = scratchbird::engine::internal_api;
namespace database = scratchbird::storage::database;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(std::string_view detail) {
  throw std::runtime_error(std::string(detail));
}

void Require(bool condition, std::string_view detail) {
  if (!condition) Fail(detail);
}

template <std::size_t N>
void Fill(std::array<std::uint8_t, N>* value, std::uint8_t seed) {
  for (std::size_t index = 0; index != N; ++index) {
    (*value)[index] = static_cast<std::uint8_t>(seed + index);
  }
}

std::string UuidText(const std::array<std::uint8_t, 16>& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(36);
  for (std::size_t index = 0; index != value.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      text.push_back('-');
    }
    text.push_back(kHex[value[index] >> 4U]);
    text.push_back(kHex[value[index] & 0x0fU]);
  }
  return text;
}

std::uint64_t NowMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::string NewUuid(UuidKind kind, std::uint64_t salt) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "engine UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

std::array<std::uint8_t, 16> UuidBytes(std::string_view text) {
  const auto parsed = uuid::ParseUuid(std::string(text));
  Require(parsed.ok(), "canonical UUID parsing failed");
  return parsed.value.bytes;
}

class TemporaryRoot {
 public:
  TemporaryRoot() {
#if defined(_WIN32)
    path_ = std::filesystem::temp_directory_path() /
            "sb_ddl_create_schema_recovery_ia08";
    std::error_code error;
    std::filesystem::create_directory(path_, error);
    Require(!error, "temporary recovery directory creation failed");
#else
    std::vector<char> pattern{
        '/', 't', 'm', 'p', '/', 's', 'b', '_', 'd', 'd', 'l', '_',
        'c', 'r', 'e', 'a', 't', 'e', '_', 's', 'c', 'h', 'e', 'm',
        'a', '_', 'r', 'e', 'c', 'o', 'v', 'e', 'r', 'y', '_', 'i',
        'a', '0', '8', '_', 'X', 'X', 'X', 'X', 'X', 'X', '\0'};
    const auto* created = ::mkdtemp(pattern.data());
    Require(created != nullptr, "mkdtemp failed");
    path_ = created;
#endif
  }

  TemporaryRoot(const TemporaryRoot&) = delete;
  TemporaryRoot& operator=(const TemporaryRoot&) = delete;

  ~TemporaryRoot() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

sblr::SblrDdlCreateSchemaDescriptorV1 Descriptor(std::uint8_t recovery_seed) {
  sblr::SblrDdlCreateSchemaDescriptorV1 value;
  Fill(&value.receipt, 0x10);
  value.occurrence = 1;
  value.schema_occurrence = 1;
  Fill(&value.schema_uuid, static_cast<std::uint8_t>(recovery_seed + 1));
  value.schema_generation = 1;
  Fill(&value.database_uuid, 0x30);
  Fill(&value.owning_transaction_uuid, 0x40);
  value.owning_local_transaction_id = 7;
  Fill(&value.statement_snapshot_uuid, 0x50);
  Fill(&value.catalog_epoch_uuid, 0x60);
  value.catalog_generation = 11;
  Fill(&value.security_context_uuid, 0x70);
  value.security_epoch = 13;
  Fill(&value.policy_snapshot_uuid, 0x80);
  value.policy_generation = 17;
  Fill(&value.resource_grant_uuid, 0x90);
  value.resource_generation = 19;
  Fill(&value.owner_principal_uuid, 0xa0);
  Fill(&value.binding_uuid, static_cast<std::uint8_t>(recovery_seed + 2));
  Fill(&value.recovery_uuid, recovery_seed);
  value.binding_generation = 1;
  value.recovery_generation = 1;
  Fill(&value.normalized_path_sha256, 0x11);
  Fill(&value.syntax_demand_sha256, 0x21);
  Fill(&value.authorization_evidence_sha256, 0x31);
  value.availability = 23;
  return value;
}

api::EngineRequestContext Context(
    const std::filesystem::path& database_path,
    const sblr::SblrDdlCreateSchemaDescriptorV1& descriptor) {
  api::EngineRequestContext context;
  context.database_path = database_path.string();
  context.database_uuid.canonical = UuidText(descriptor.database_uuid);
  context.statement_receipt_uuid.canonical = UuidText(descriptor.receipt);
  context.transaction_uuid.canonical =
      UuidText(descriptor.owning_transaction_uuid);
  context.local_transaction_id = descriptor.owning_local_transaction_id;
  context.statement_snapshot_uuid.canonical =
      UuidText(descriptor.statement_snapshot_uuid);
  context.catalog_epoch_uuid.canonical =
      UuidText(descriptor.catalog_epoch_uuid);
  context.catalog_generation_id = descriptor.catalog_generation;
  context.security_context_present = true;
  context.security_epoch = descriptor.security_epoch;
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      UuidText(descriptor.security_context_uuid);
  context.authorization_context.security_epoch = descriptor.security_epoch;
  context.transaction_policy_snapshot_uuid.canonical =
      UuidText(descriptor.policy_snapshot_uuid);
  context.transaction_policy_snapshot_generation =
      descriptor.policy_generation;
  context.resource_admission_uuid.canonical =
      UuidText(descriptor.resource_grant_uuid);
  context.resource_epoch = descriptor.resource_generation;
  context.principal_uuid.canonical =
      UuidText(descriptor.owner_principal_uuid);
  context.statement_metadata_snapshot_engine_owned = true;
  context.trace_tags.push_back(
      "private_ddl_create_schema_execution_journal");
  return context;
}

api::SblrDdlCreateSchemaJournalKeyV1 Key(
    const sblr::SblrDdlCreateSchemaDescriptorV1& descriptor) {
  api::SblrDdlCreateSchemaJournalKeyV1 key;
  key.database_uuid = descriptor.database_uuid;
  key.recovery_uuid = descriptor.recovery_uuid;
  key.canonical_descriptor_bytes =
      sblr::EncodeSblrDdlCreateSchemaDescriptorV1(descriptor, true);
  Require(key.canonical_descriptor_bytes.size() == 488,
          "canonical CSDO construction failed");
  return key;
}

api::EngineApiDiagnostic Ok() {
  api::EngineApiDiagnostic diagnostic;
  diagnostic.code = "OK";
  diagnostic.message_key = "ok";
  diagnostic.error = false;
  return diagnostic;
}

api::EngineApiDiagnostic OutcomeUnknown() {
  api::EngineApiDiagnostic diagnostic;
  diagnostic.code = "PARSER_SERVER_IPC.OUTCOME_UNKNOWN";
  diagnostic.message_key =
      "sblr.ddl_create_schema.test_mutation_outcome_unknown";
  diagnostic.error = true;
  return diagnostic;
}

enum class MarkerResult { created, existed, failed };

MarkerResult PublishMarker(const std::filesystem::path& path) {
#if defined(_WIN32)
  if (std::filesystem::exists(path)) return MarkerResult::existed;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return MarkerResult::failed;
  output << "published\n";
  output.flush();
  return output ? MarkerResult::created : MarkerResult::failed;
#else
  const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL |
                                                  O_CLOEXEC | O_NOFOLLOW,
                                0600);
  if (descriptor < 0) {
    return errno == EEXIST ? MarkerResult::existed : MarkerResult::failed;
  }
  constexpr char kPayload[] = "published\n";
  const auto count = ::write(descriptor, kPayload, sizeof(kPayload) - 1);
  const bool ok = count == static_cast<ssize_t>(sizeof(kPayload) - 1) &&
                  ::fsync(descriptor) == 0 && ::close(descriptor) == 0;
  if (!ok) {
    (void)::unlink(path.c_str());
    return MarkerResult::failed;
  }
  return MarkerResult::created;
#endif
}

std::filesystem::path JournalPath(
    const api::EngineRequestContext& context,
    const api::SblrDdlCreateSchemaJournalKeyV1& key) {
  return context.database_path +
         ".sb.sblr_ddl_create_schema_execution_journal.v1." +
         UuidText(key.recovery_uuid);
}

void RequireExactResult(
    const api::SblrDdlCreateSchemaJournalResultV1& result,
    const api::SblrDdlCreateSchemaJournalKeyV1& key) {
  Require(result.ok && result.found,
          "durable CREATE SCHEMA journal result was not available");
  Require(result.snapshot.key.canonical_descriptor_bytes ==
              key.canonical_descriptor_bytes,
          "durable CREATE SCHEMA journal changed the CSDO authority");
  Require(result.snapshot.canonical_result_bytes.size() == 320,
          "durable CREATE SCHEMA journal did not retain exact CSRS bytes");
  sblr::SblrDdlCreateSchemaResultV1 decoded;
  std::string detail;
  Require(sblr::DecodeSblrDdlCreateSchemaResultV1(
              result.snapshot.canonical_result_bytes.data(),
              result.snapshot.canonical_result_bytes.size(), &decoded,
              &detail),
          "durable CREATE SCHEMA journal retained invalid CSRS bytes");
}

api::EngineRequestContext BeginTransaction(
    const std::filesystem::path& path, const std::string& database_uuid,
    const std::string& principal_uuid, std::uint64_t salt) {
  api::EngineBeginTransactionRequest request;
  request.context.trust_mode = api::EngineTrustMode::server_isolated;
  request.context.request_id = "ia08-ddl-create-schema-recovery";
  request.context.database_path = path.string();
  request.context.database_uuid.canonical = database_uuid;
  request.context.principal_uuid.canonical = principal_uuid;
  request.context.session_uuid.canonical = NewUuid(UuidKind::object, salt);
  request.context.security_context_present = true;
  request.context.identifier_profile_uuid = "sbsql_v3";
  request.context.language_context.language_tag = "en";
  request.context.language_context.default_language_tag = "en";
  request.context.catalog_generation_id = 1;
  request.context.security_epoch = 1;
  request.context.resource_epoch = 1;
  request.context.datatype_catalog_snapshot_uuid.canonical =
      "019d0000-0000-7000-8000-00000000d701";
  request.context.datatype_catalog_generation = 1;
  request.context.datatype_registry_generation = 1;
  request.context.name_resolution_epoch = 1;
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  Require(begun.ok, "CREATE SCHEMA recovery transaction begin failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

sblr::SblrDdlCreateSchemaDescriptorV1 DescriptorForTransaction(
    api::EngineRequestContext* context, const std::string& schema_uuid,
    std::uint64_t salt) {
  auto descriptor = Descriptor(0xf0);
  descriptor.receipt = UuidBytes(NewUuid(UuidKind::object, salt + 1));
  descriptor.schema_uuid = UuidBytes(schema_uuid);
  descriptor.database_uuid = UuidBytes(context->database_uuid.canonical);
  descriptor.owning_transaction_uuid =
      UuidBytes(context->transaction_uuid.canonical);
  descriptor.owning_local_transaction_id = context->local_transaction_id;
  descriptor.statement_snapshot_uuid =
      UuidBytes(NewUuid(UuidKind::object, salt + 2));
  descriptor.catalog_epoch_uuid =
      UuidBytes(NewUuid(UuidKind::object, salt + 3));
  descriptor.catalog_generation = context->catalog_generation_id;
  descriptor.security_context_uuid =
      UuidBytes(NewUuid(UuidKind::object, salt + 4));
  descriptor.security_epoch = context->security_epoch;
  descriptor.policy_snapshot_uuid =
      UuidBytes(NewUuid(UuidKind::object, salt + 5));
  descriptor.policy_generation = 1;
  descriptor.resource_grant_uuid =
      UuidBytes(NewUuid(UuidKind::object, salt + 6));
  descriptor.resource_generation = context->resource_epoch;
  descriptor.owner_principal_uuid =
      UuidBytes(context->principal_uuid.canonical);
  descriptor.binding_uuid = UuidBytes(NewUuid(UuidKind::object, salt + 7));
  descriptor.recovery_uuid = UuidBytes(NewUuid(UuidKind::object, salt + 8));
  descriptor.evidence = {};

  context->statement_receipt_uuid.canonical = UuidText(descriptor.receipt);
  context->statement_snapshot_uuid.canonical =
      UuidText(descriptor.statement_snapshot_uuid);
  context->statement_metadata_snapshot_engine_owned = true;
  context->statement_metadata_snapshot_uuid = context->statement_snapshot_uuid;
  context->catalog_epoch_uuid.canonical =
      UuidText(descriptor.catalog_epoch_uuid);
  context->authorization_context.present = true;
  context->authorization_context.authority_uuid.canonical =
      UuidText(descriptor.security_context_uuid);
  context->authorization_context.security_epoch = descriptor.security_epoch;
  context->transaction_policy_snapshot_uuid.canonical =
      UuidText(descriptor.policy_snapshot_uuid);
  context->transaction_policy_snapshot_generation =
      descriptor.policy_generation;
  context->resource_admission_uuid.canonical =
      UuidText(descriptor.resource_grant_uuid);
  context->trace_tags.push_back(
      "private_ddl_create_schema_execution_journal");
  return descriptor;
}

api::EngineApiDiagnostic CreateSchemaMutation(
    const api::EngineRequestContext& context,
    const sblr::SblrDdlCreateSchemaDescriptorV1& descriptor,
    const sblr::SblrDdlCreateSchemaResultV1& planned_result) {
  api::EngineCreateSchemaRequest request;
  request.context = context;
  request.operation_id = "ddl.create_schema";
  request.target_database.uuid = context.database_uuid;
  request.target_database.object_kind = "database";
  request.target_object.uuid.canonical = UuidText(descriptor.schema_uuid);
  request.target_object.object_kind = "schema";
  request.localized_names.push_back(
      {"en", "primary", "recovered_schema", "recovered_schema", true});
  request.recovery_operation_uuid.canonical =
      UuidText(descriptor.recovery_uuid);
  request.requested_catalog_row_uuid.canonical =
      UuidText(planned_result.catalog_row_uuid);
  request.mutation_uuid.canonical = UuidText(planned_result.mutation_uuid);
  request.statement_publication_barrier_uuid.canonical =
      UuidText(planned_result.publication_barrier);
  request.option_envelopes.push_back(
      "catalog_ddl_mutation_audit:" +
      request.recovery_operation_uuid.canonical);
  const auto created = api::EngineCreateSchema(request);
  if (!created.ok) {
    if (!created.diagnostics.empty()) return created.diagnostics.front();
    api::EngineApiDiagnostic diagnostic;
    diagnostic.code = "DDL.CREATE_SCHEMA_FAILED";
    diagnostic.message_key =
        "sblr.ddl_create_schema.test_catalog_mutation_failed";
    return diagnostic;
  }
  Require(created.primary_object.uuid.canonical ==
                  request.target_object.uuid.canonical &&
              created.catalog_row_uuid.canonical ==
                  request.requested_catalog_row_uuid.canonical,
          "CREATE SCHEMA recovery returned changed catalog identities");
  return Ok();
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  Require(api::EngineCommitTransaction(request).ok,
          "CREATE SCHEMA recovery transaction commit failed");
}

}  // namespace

int main() {
  try {
    TemporaryRoot temporary_root;
    const auto database_path = temporary_root.path() / "database";

    const auto descriptor = Descriptor(0xc0);
    const auto context = Context(database_path, descriptor);
    const auto key = Key(descriptor);

    const auto begun = api::EnsureSblrDdlCreateSchemaExecutionJournalV1(
        context, key);
    RequireExactResult(begun, key);
    Require(begun.snapshot.state ==
                api::SblrDdlCreateSchemaJournalStateV1::begun &&
                begun.snapshot.journal_generation == 1,
            "CREATE SCHEMA intent was not durably begun before mutation");

    const auto replayed_intent =
        api::EnsureSblrDdlCreateSchemaExecutionJournalV1(context, key);
    RequireExactResult(replayed_intent, key);
    Require(replayed_intent.snapshot.canonical_result_bytes ==
                begun.snapshot.canonical_result_bytes &&
                replayed_intent.snapshot.catalog_row_uuid ==
                    begun.snapshot.catalog_row_uuid &&
                replayed_intent.snapshot.mutation_uuid ==
                    begun.snapshot.mutation_uuid &&
                replayed_intent.snapshot.publication_barrier_uuid ==
                    begun.snapshot.publication_barrier_uuid,
            "exact CREATE SCHEMA intent replay changed planned identities");

    const auto marker = temporary_root.path() / "first_publication.marker";
    std::uint32_t callback_count = 0;
    std::uint32_t logical_publication_count = 0;
    const auto unknown = api::ExecuteSblrDdlCreateSchemaExecutionJournalV1(
        context, key, [&](const sblr::SblrDdlCreateSchemaResultV1&) {
          ++callback_count;
          const auto published = PublishMarker(marker);
          Require(published != MarkerResult::failed,
                  "test publication marker could not be made durable");
          if (published == MarkerResult::created) ++logical_publication_count;
          return OutcomeUnknown();
        });
    Require(!unknown.ok && unknown.found && unknown.mutation_invoked &&
                unknown.snapshot.state ==
                    api::SblrDdlCreateSchemaJournalStateV1::begun,
            "unknown CREATE SCHEMA outcome did not retain begun recovery state");

    const auto repaired = api::ExecuteSblrDdlCreateSchemaExecutionJournalV1(
        context, key, [&](const sblr::SblrDdlCreateSchemaResultV1&) {
          ++callback_count;
          const auto published = PublishMarker(marker);
          Require(published != MarkerResult::failed,
                  "test recovery marker could not be read");
          if (published == MarkerResult::created) ++logical_publication_count;
          return Ok();
        });
    RequireExactResult(repaired, key);
    Require(repaired.snapshot.state ==
                api::SblrDdlCreateSchemaJournalStateV1::published &&
                repaired.snapshot.journal_generation == 2 &&
                repaired.mutation_invoked && callback_count == 2 &&
                logical_publication_count == 1,
            "CREATE SCHEMA outcome recovery repeated logical publication");

    const auto exact_csrs = repaired.snapshot.canonical_result_bytes;
    const auto replayed = api::ExecuteSblrDdlCreateSchemaExecutionJournalV1(
        context, key, [](const sblr::SblrDdlCreateSchemaResultV1&) {
          return OutcomeUnknown();
        });
    RequireExactResult(replayed, key);
    Require(replayed.replayed_published_result && !replayed.mutation_invoked &&
                replayed.snapshot.canonical_result_bytes == exact_csrs,
            "published CREATE SCHEMA retry did not return byte-identical CSRS");

    auto changed_descriptor = descriptor;
    ++changed_descriptor.catalog_generation;
    changed_descriptor.evidence = {};
    const auto changed_key = Key(changed_descriptor);
    auto changed_context = Context(database_path, changed_descriptor);
    const auto changed = api::LookupSblrDdlCreateSchemaExecutionJournalV1(
        changed_context, changed_key);
    Require(!changed.ok &&
                changed.diagnostic.code == "MGA.AUTHORITY_MISMATCH",
            "changed CREATE SCHEMA authority reused a recovery identity");

    auto foreign_context = context;
    foreign_context.statement_receipt_uuid.canonical =
        "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
    const auto hidden = api::LookupSblrDdlCreateSchemaExecutionJournalV1(
        foreign_context, key);
    Require(!hidden.ok && hidden.diagnostic.code == "SECURITY.ACCESS_DENIED",
            "foreign receipt observed CREATE SCHEMA recovery state");

#if !defined(_WIN32)
    const auto crash_descriptor = Descriptor(0xd0);
    const auto crash_context = Context(database_path, crash_descriptor);
    const auto crash_key = Key(crash_descriptor);
    const auto crash_marker = temporary_root.path() / "crash_publication.marker";
    const auto child = ::fork();
    Require(child >= 0, "CREATE SCHEMA recovery crash worker fork failed");
    if (child == 0) {
      (void)api::ExecuteSblrDdlCreateSchemaExecutionJournalV1(
          crash_context, crash_key,
          [&](const sblr::SblrDdlCreateSchemaResultV1&)
              -> api::EngineApiDiagnostic {
            const auto published = PublishMarker(crash_marker);
            ::_exit(published == MarkerResult::created ? 77 : 78);
            return OutcomeUnknown();
          });
      ::_exit(79);
    }
    int child_status = 0;
    Require(::waitpid(child, &child_status, 0) == child &&
                WIFEXITED(child_status) && WEXITSTATUS(child_status) == 77,
            "CREATE SCHEMA worker did not crash after durable publication");

    const auto after_crash =
        api::LookupSblrDdlCreateSchemaExecutionJournalV1(crash_context,
                                                         crash_key);
    RequireExactResult(after_crash, crash_key);
    Require(after_crash.snapshot.state ==
                api::SblrDdlCreateSchemaJournalStateV1::begun,
            "crash before CSRS publication did not retain durable intent");

    std::uint32_t recovery_callbacks = 0;
    const auto crash_repaired =
        api::ExecuteSblrDdlCreateSchemaExecutionJournalV1(
            crash_context, crash_key,
            [&](const sblr::SblrDdlCreateSchemaResultV1&) {
              ++recovery_callbacks;
              Require(PublishMarker(crash_marker) == MarkerResult::existed,
                      "crash recovery did not observe durable publication");
              return Ok();
            });
    RequireExactResult(crash_repaired, crash_key);
    Require(crash_repaired.snapshot.state ==
                api::SblrDdlCreateSchemaJournalStateV1::published &&
                crash_repaired.mutation_invoked && recovery_callbacks == 1,
            "CREATE SCHEMA crash recovery did not publish terminal CSRS");

    const auto crash_replayed =
        api::ExecuteSblrDdlCreateSchemaExecutionJournalV1(
            crash_context, crash_key,
            [&](const sblr::SblrDdlCreateSchemaResultV1&) {
              ++recovery_callbacks;
              return OutcomeUnknown();
            });
    Require(crash_replayed.ok && crash_replayed.replayed_published_result &&
                !crash_replayed.mutation_invoked && recovery_callbacks == 1 &&
                crash_replayed.snapshot.canonical_result_bytes ==
                    crash_repaired.snapshot.canonical_result_bytes,
            "CREATE SCHEMA crash replay invoked mutation or changed CSRS");

    const auto catalog_database_path =
        temporary_root.path() / "catalog_recovery.sbdb";
    database::DatabaseCreateConfig create_database;
    create_database.path = catalog_database_path.string();
    const auto database_identity =
        uuid::GenerateEngineIdentityV7(UuidKind::database, NowMillis() + 200);
    const auto filespace_identity =
        uuid::GenerateEngineIdentityV7(UuidKind::filespace, NowMillis() + 201);
    Require(database_identity.ok() && filespace_identity.ok(),
            "CREATE SCHEMA recovery database identities were unavailable");
    create_database.database_uuid = database_identity.value;
    create_database.filespace_uuid = filespace_identity.value;
    create_database.page_size = 16384;
    create_database.creation_unix_epoch_millis = NowMillis();
    create_database.allow_minimal_resource_bootstrap = true;
    create_database.require_resource_seed_pack = false;
    Require(database::CreateDatabaseFile(create_database).ok(),
            "CREATE SCHEMA recovery database creation failed");

    const auto catalog_database_uuid =
        uuid::UuidToString(database_identity.value.value);
    const auto catalog_principal_uuid = NewUuid(UuidKind::object, 202);
    const auto catalog_schema_uuid = NewUuid(UuidKind::schema, 203);
    auto catalog_context = BeginTransaction(
        catalog_database_path, catalog_database_uuid,
        catalog_principal_uuid, 204);
    const auto catalog_descriptor = DescriptorForTransaction(
        &catalog_context, catalog_schema_uuid, 210);
    const auto catalog_key = Key(catalog_descriptor);

    const auto catalog_child = ::fork();
    Require(catalog_child >= 0,
            "CREATE SCHEMA catalog recovery worker fork failed");
    if (catalog_child == 0) {
      (void)api::ExecuteSblrDdlCreateSchemaExecutionJournalV1(
          catalog_context, catalog_key,
          [&](const sblr::SblrDdlCreateSchemaResultV1& planned_result)
              -> api::EngineApiDiagnostic {
            const auto mutation = CreateSchemaMutation(
                catalog_context, catalog_descriptor, planned_result);
            ::_exit(mutation.error ? 82 : 81);
            return mutation;
          });
      ::_exit(83);
    }
    int catalog_child_status = 0;
    Require(::waitpid(catalog_child, &catalog_child_status, 0) ==
                    catalog_child &&
                WIFEXITED(catalog_child_status) &&
                WEXITSTATUS(catalog_child_status) == 81,
            "CREATE SCHEMA worker did not crash after real catalog mutation");

    const auto catalog_after_crash =
        api::LookupSblrDdlCreateSchemaExecutionJournalV1(catalog_context,
                                                         catalog_key);
    RequireExactResult(catalog_after_crash, catalog_key);
    Require(catalog_after_crash.snapshot.state ==
                api::SblrDdlCreateSchemaJournalStateV1::begun,
            "real catalog mutation published CSRS before the crash boundary");

    std::uint32_t catalog_recovery_callbacks = 0;
    const auto catalog_repaired =
        api::ExecuteSblrDdlCreateSchemaExecutionJournalV1(
            catalog_context, catalog_key,
            [&](const sblr::SblrDdlCreateSchemaResultV1& planned_result) {
              ++catalog_recovery_callbacks;
              return CreateSchemaMutation(catalog_context, catalog_descriptor,
                                          planned_result);
            });
    RequireExactResult(catalog_repaired, catalog_key);
    Require(catalog_repaired.ok && catalog_repaired.mutation_invoked &&
                catalog_recovery_callbacks == 1 &&
                catalog_repaired.snapshot.state ==
                    api::SblrDdlCreateSchemaJournalStateV1::published,
            "real CREATE SCHEMA catalog recovery did not publish CSRS");

    const auto own_schemas = api::VisibleSchemaTreeRecords(
        catalog_context, catalog_context.local_transaction_id);
    Require(std::count_if(
                own_schemas.begin(), own_schemas.end(),
                [&](const api::EngineSchemaTreeRecord& record) {
                  return record.schema_uuid == catalog_schema_uuid;
                }) == 1,
            "real CREATE SCHEMA recovery duplicated the schema record");
    const auto own_names = api::LoadNameRegistryState(
        catalog_context, catalog_context.local_transaction_id);
    Require(own_names.ok &&
                std::count_if(
                    own_names.state.entries.begin(), own_names.state.entries.end(),
                    [&](const api::NameRegistryEntry& entry) {
                      return !entry.deleted &&
                             entry.object_uuid == catalog_schema_uuid &&
                             entry.object_class == "schema";
                    }) == 1,
            "real CREATE SCHEMA recovery duplicated the name record");

    Commit(catalog_context);
    auto observer = BeginTransaction(catalog_database_path,
                                     catalog_database_uuid,
                                     catalog_principal_uuid, 230);
    const auto observed_schemas =
        api::VisibleSchemaTreeRecords(observer, observer.local_transaction_id);
    Require(std::count_if(
                observed_schemas.begin(), observed_schemas.end(),
                [&](const api::EngineSchemaTreeRecord& record) {
                  return record.schema_uuid == catalog_schema_uuid;
                }) == 1,
            "independent transaction did not observe exactly one recovered schema");
    const auto observed_names =
        api::LoadNameRegistryState(observer, observer.local_transaction_id);
    Require(observed_names.ok &&
                std::count_if(
                    observed_names.state.entries.begin(),
                    observed_names.state.entries.end(),
                    [&](const api::NameRegistryEntry& entry) {
                      return !entry.deleted &&
                             entry.object_uuid == catalog_schema_uuid &&
                             entry.object_class == "schema";
                    }) == 1,
            "independent transaction did not observe one recovered schema name");
    Commit(observer);
#endif

    const auto corrupt_descriptor = Descriptor(0xe0);
    const auto corrupt_context = Context(database_path, corrupt_descriptor);
    const auto corrupt_key = Key(corrupt_descriptor);
    const auto corrupt_intent =
        api::EnsureSblrDdlCreateSchemaExecutionJournalV1(corrupt_context,
                                                         corrupt_key);
    RequireExactResult(corrupt_intent, corrupt_key);
    const auto corrupt_path = JournalPath(corrupt_context, corrupt_key);
    {
      std::fstream file(corrupt_path,
                        std::ios::binary | std::ios::in | std::ios::out);
      Require(static_cast<bool>(file),
              "CREATE SCHEMA journal could not be opened for fault injection");
      file.seekg(176);
      char byte = 0;
      file.read(&byte, 1);
      Require(static_cast<bool>(file),
              "CREATE SCHEMA journal fault byte could not be read");
      byte ^= 0x01;
      file.seekp(176);
      file.write(&byte, 1);
      file.flush();
      Require(static_cast<bool>(file),
              "CREATE SCHEMA journal fault byte could not be written");
    }
    const auto corrupt = api::LookupSblrDdlCreateSchemaExecutionJournalV1(
        corrupt_context, corrupt_key);
    Require(!corrupt.ok &&
                corrupt.diagnostic.code == "MGA.AUTHORITY_MISMATCH",
            "corrupt CREATE SCHEMA recovery record did not fail closed");
  } catch (const std::exception& error) {
    std::cerr << "ddl_create_schema_recovery: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
