// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "ddl/alter_api.hpp"
#include "ddl/create_api.hpp"
#include "domain_support/domain_store.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kSchemaUuid = "019f0000-0000-7000-8000-000000420001";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_domain_policy_predicate_closure_" +
          std::to_string(CurrentUnixMillis()) + ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events",
                            ".sb.crud_events",
                            ".sb.domain_events",
                            ".sb.name_events",
                            ".sb.transaction_inventory",
                            ".dirty.manifest",
                            ".recovery.evidence",
                            ".sb.owner.lock"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

std::string CreateMinimalDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810590000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810590001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810590002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "domain policy predicate test database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

std::string DomainUuid(unsigned ordinal) {
  std::ostringstream out;
  out << "019f0000-0000-7000-8000-" << std::hex << std::setw(12)
      << std::setfill('0') << (0x420000u + ordinal);
  return out.str();
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-domain-policy-predicate-closure";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000420101";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000420102";
  context.current_schema_uuid.canonical = std::string(kSchemaUuid);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  context.trace_tags.push_back("domain_policy_predicate_closure");
  return context;
}

api::EngineRequestContext BeginEngineTransaction(const std::filesystem::path& path,
                                                 const std::string& database_uuid) {
  api::EngineBeginTransactionRequest begin;
  begin.context = EngineContext(path, database_uuid);
  begin.isolation_level = "read_committed";
  const auto result = api::EngineBeginTransaction(begin);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.ok, "transaction begin failed for domain policy predicate test");
  auto context = begin.context;
  context.local_transaction_id = result.local_transaction_id;
  context.transaction_uuid = result.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      result.snapshot_visible_through_local_transaction_id;
  return context;
}

api::EngineDescriptor ScalarDescriptor(std::string canonical_type_name) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(canonical_type_name);
  descriptor.encoded_descriptor = "type=" + descriptor.canonical_type_name;
  return descriptor;
}

api::EngineCreateDomainRequest DomainCreateRequest(const api::EngineRequestContext& context,
                                                   const std::string& domain_uuid,
                                                   const std::string& name,
                                                   const std::string& base_type) {
  api::EngineCreateDomainRequest request;
  request.context = context;
  request.target_schema.uuid.canonical = std::string(kSchemaUuid);
  request.target_schema.object_kind = "schema";
  request.target_object.uuid.canonical = domain_uuid;
  request.target_object.object_kind = "domain";
  request.localized_names.push_back({"en", "primary", "", name, true});
  request.descriptors.push_back(ScalarDescriptor(base_type));
  return request;
}

std::string FirstDetail(const std::vector<api::EngineApiDiagnostic>& diagnostics) {
  return diagnostics.empty() ? std::string{} : diagnostics.front().detail;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence_refs,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : evidence_refs) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) { return true; }
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  return HasEvidence(result.evidence, kind, id);
}

api::EngineCreateDomainResult CreateDomainResult(const api::EngineRequestContext& context,
                                                 const std::string& domain_uuid,
                                                 const std::string& name,
                                                 const std::string& base_type,
                                                 const std::string& check_envelope = {},
                                                 const std::string& visibility_policy = {}) {
  auto request = DomainCreateRequest(context, domain_uuid, name, base_type);
  if (!check_envelope.empty()) {
    request.option_envelopes.push_back("check_constraint:" + check_envelope);
  }
  if (!visibility_policy.empty()) {
    request.option_envelopes.push_back("visibility_policy:" + visibility_policy);
  }
  return api::EngineCreateDomain(request);
}

api::EngineDescriptor CreateDomain(const api::EngineRequestContext& context,
                                   const std::string& domain_uuid,
                                   const std::string& name,
                                   const std::string& base_type,
                                   const std::string& check_envelope = {},
                                   const std::string& visibility_policy = {}) {
  const auto result =
      CreateDomainResult(context, domain_uuid, name, base_type, check_envelope, visibility_policy);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.ok, "EngineCreateDomain unexpectedly failed");
  Require(result.operation_id == "ddl.create_domain",
          "EngineCreateDomain returned unexpected operation id");
  Require(HasEvidence(result, "domain_event", "domain_create"),
          "EngineCreateDomain did not persist domain event evidence");
  const auto domain = api::FindVisibleDomain(context, domain_uuid, context.local_transaction_id);
  Require(domain.has_value(), "created domain is not visible to engine transaction");
  return api::DomainDescriptor(*domain);
}

api::EngineTypedValue TypedValue(std::string type, std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor = ScalarDescriptor(std::move(type));
  typed.encoded_value = std::move(value);
  typed.is_null = false;
  return typed;
}

void ExpectDomainValidation(const api::EngineRequestContext& context,
                            const api::EngineDescriptor& descriptor,
                            const api::EngineTypedValue& value,
                            bool expected_ok,
                            std::string_view expected_detail) {
  const auto result =
      api::ValidateDomainTypedValue(context, descriptor, value, context.local_transaction_id);
  if (result.ok != expected_ok) {
    std::cerr << result.diagnostic.code << ':' << result.diagnostic.detail << '\n';
  }
  Require(result.ok == expected_ok, "domain validation returned unexpected status");
  if (expected_ok) {
    Require(HasEvidence(result.evidence, "domain_validation", descriptor.descriptor_uuid.canonical),
            "domain validation evidence missing");
  } else {
    Require(result.diagnostic.detail == expected_detail, "domain validation diagnostic drifted");
  }
}

struct PredicateCase {
  std::string envelope;
  std::string base_type;
  std::string ok_value;
  std::string fail_value;
  std::string fail_detail;
};

void RequirePredicateSurface(const api::EngineRequestContext& context) {
  Require(api::IsSupportedDomainCheckEnvelope("not_empty"),
          "not_empty check envelope should be supported");
  Require(api::IsSupportedDomainCheckEnvelope("sblr_predicate:length_gte:2"),
          "wrapped length predicate should be supported");
  Require(!api::IsSupportedDomainCheckEnvelope("sblr_predicate:starts_with:x"),
          "unsupported wrapped predicate was accepted as supported");
  Require(!api::IsSupportedDomainCheckEnvelope("gt:not-a-number"),
          "invalid numeric predicate RHS was accepted as supported");

  const std::vector<PredicateCase> cases = {
      {"not_empty", "text", "x", "", "domain.validate_value:domain_check_not_empty_failed"},
      {"eq:7", "bigint", "7", "8", "domain.validate_value:domain_check_eq_failed"},
      {"gt:7", "bigint", "8", "7", "domain.validate_value:domain_check_gt_failed"},
      {"gte:7", "bigint", "7", "6", "domain.validate_value:domain_check_gte_failed"},
      {"lt:7", "bigint", "6", "7", "domain.validate_value:domain_check_lt_failed"},
      {"lte:7", "bigint", "7", "8", "domain.validate_value:domain_check_lte_failed"},
      {"length_gt:2", "text", "abc", "ab", "domain.validate_value:domain_check_length_gt_failed"},
      {"length_gte:2", "text", "ab", "a", "domain.validate_value:domain_check_length_gte_failed"},
      {"length_lt:3", "text", "ab", "abc", "domain.validate_value:domain_check_length_lt_failed"},
      {"length_lte:3", "text", "abc", "abcd", "domain.validate_value:domain_check_length_lte_failed"},
  };

  unsigned ordinal = 1;
  for (const auto& test_case : cases) {
    const auto raw_uuid = DomainUuid(ordinal++);
    const auto raw_descriptor = CreateDomain(context,
                                             raw_uuid,
                                             "domain_predicate_raw_" + std::to_string(ordinal),
                                             test_case.base_type,
                                             test_case.envelope);
    ExpectDomainValidation(context,
                           raw_descriptor,
                           TypedValue(test_case.base_type, test_case.ok_value),
                           true,
                           {});
    ExpectDomainValidation(context,
                           raw_descriptor,
                           TypedValue(test_case.base_type, test_case.fail_value),
                           false,
                           test_case.fail_detail);

    const auto wrapped_uuid = DomainUuid(ordinal++);
    const auto wrapped_descriptor = CreateDomain(context,
                                                 wrapped_uuid,
                                                 "domain_predicate_wrapped_" + std::to_string(ordinal),
                                                 test_case.base_type,
                                                 "sblr_predicate:" + test_case.envelope);
    ExpectDomainValidation(context,
                           wrapped_descriptor,
                           TypedValue(test_case.base_type, test_case.ok_value),
                           true,
                           {});
    std::string wrapped_detail = test_case.fail_detail;
    const std::string prefix = "domain.validate_value:";
    if (wrapped_detail.rfind(prefix, 0) == 0) {
      wrapped_detail = prefix + std::string("domain_sblr_predicate_") +
                       wrapped_detail.substr(prefix.size());
    }
    if (test_case.envelope == "not_empty") {
      wrapped_detail = "domain.validate_value:domain_sblr_predicate_not_empty_failed";
    }
    ExpectDomainValidation(context,
                           wrapped_descriptor,
                           TypedValue(test_case.base_type, test_case.fail_value),
                           false,
                           wrapped_detail);
  }
}

void ExpectCreateRefusal(const api::EngineCreateDomainResult& result,
                         std::string_view expected_detail) {
  Require(!result.ok, "EngineCreateDomain unexpectedly accepted unsupported input");
  Require(FirstDetail(result.diagnostics) == expected_detail,
          "EngineCreateDomain refusal diagnostic drifted");
}

void RequireCreateAndAlterRefusals(const api::EngineRequestContext& context) {
  ExpectCreateRefusal(
      CreateDomainResult(context,
                         DomainUuid(100),
                         "unsupported_wrapped_predicate",
                         "text",
                         "sblr_predicate:starts_with:x"),
      "ddl.create_domain:unsupported_domain_check_constraint");
  ExpectCreateRefusal(
      CreateDomainResult(context,
                         DomainUuid(101),
                         "invalid_numeric_predicate",
                         "bigint",
                         "gt:not-a-number"),
      "ddl.create_domain:unsupported_domain_check_constraint");

  auto constraint_request =
      DomainCreateRequest(context, DomainUuid(102), "unsupported_constraint_check", "text");
  constraint_request.constraints.push_back(
      {api::EngineUuid{}, {}, "check", "sblr_predicate:contains:needle"});
  ExpectCreateRefusal(api::EngineCreateDomain(constraint_request),
                      "ddl.create_domain:unsupported_domain_check_constraint");

  auto unsupported_option =
      DomainCreateRequest(context, DomainUuid(103), "unsupported_option", "text");
  unsupported_option.option_envelopes.push_back("future_domain_option:true");
  ExpectCreateRefusal(api::EngineCreateDomain(unsupported_option),
                      "ddl.create_domain:unsupported_domain_option");

  auto unsupported_constraint =
      DomainCreateRequest(context, DomainUuid(104), "unsupported_constraint", "text");
  unsupported_constraint.constraints.push_back(
      {api::EngineUuid{}, {}, "foreign_key", "fk:other"});
  ExpectCreateRefusal(api::EngineCreateDomain(unsupported_constraint),
                      "ddl.create_domain:unsupported_domain_constraint");

  const auto descriptor =
      CreateDomain(context, DomainUuid(105), "alter_check_refusal_anchor", "text", "not_empty");
  api::EngineAlterObjectRequest alter;
  alter.context = context;
  alter.target_object.uuid.canonical = descriptor.descriptor_uuid.canonical;
  alter.target_object.object_kind = "domain";
  alter.option_envelopes.push_back("check_constraint:sblr_predicate:matches_regex:x");
  const auto alter_result = api::EngineAlterObject(alter);
  Require(!alter_result.ok, "EngineAlterObject unexpectedly accepted unsupported domain check");
  Require(FirstDetail(alter_result.diagnostics) ==
              "ddl.alter_object:unsupported_domain_check_constraint",
          "EngineAlterObject refusal diagnostic drifted");
}

api::DomainReadPolicyResult ReadDomainColumn(const api::EngineRequestContext& context,
                                             const std::string& domain_uuid,
                                             std::string column_name = "domain_col") {
  return api::ApplyDomainReadPoliciesToCrudValues(
      context,
      {{column_name, "domain:" + domain_uuid}},
      {{column_name, "visible"}},
      context.local_transaction_id);
}

void ExpectReadPolicy(const api::EngineRequestContext& context,
                      const std::string& domain_uuid,
                      bool expected_ok,
                      std::string_view expected_detail) {
  const auto result = ReadDomainColumn(context, domain_uuid);
  if (result.ok != expected_ok) {
    std::cerr << result.diagnostic.code << ':' << result.diagnostic.detail << '\n';
  }
  Require(result.ok == expected_ok, "domain read policy returned unexpected status");
  if (expected_ok) {
    Require(HasEvidence(result.evidence, "domain_read_policy", domain_uuid),
            "domain read policy evidence missing");
  } else {
    Require(result.diagnostic.detail == expected_detail, "domain read policy diagnostic drifted");
  }
}

void RequireVisibilityPolicies(const api::EngineRequestContext& context) {
  CreateDomain(context, DomainUuid(200), "visibility_allow_all", "text", {}, "allow_all");
  ExpectReadPolicy(context, DomainUuid(200), true, {});

  CreateDomain(context, DomainUuid(201), "visibility_deny_all", "text", {}, "deny_all");
  ExpectReadPolicy(context,
                   DomainUuid(201),
                   false,
                   "domain.validate_value:domain_visibility_denied:domain_col");

  CreateDomain(context,
               DomainUuid(202),
               "visibility_require_security_context",
               "text",
               {},
               "require_security_context");
  ExpectReadPolicy(context, DomainUuid(202), true, {});
  auto no_security_context = context;
  no_security_context.security_context_present = false;
  ExpectReadPolicy(no_security_context,
                   DomainUuid(202),
                   false,
                   "domain.validate_value:domain_visibility_requires_security_context:domain_col");

  const std::string required_principal = context.principal_uuid.canonical;
  CreateDomain(context,
               DomainUuid(203),
               "visibility_require_principal",
               "text",
               {},
               "require_principal:" + required_principal);
  ExpectReadPolicy(context, DomainUuid(203), true, {});
  auto wrong_principal = context;
  wrong_principal.principal_uuid.canonical = "019f0000-0000-7000-8000-00000042ffff";
  ExpectReadPolicy(wrong_principal,
                   DomainUuid(203),
                   false,
                   "domain.validate_value:domain_visibility_principal_denied:domain_col");

  CreateDomain(context,
               DomainUuid(204),
               "visibility_require_right",
               "text",
               {},
               "require_right:DOMAIN_READ");
  auto right_context = context;
  right_context.trace_tags.push_back("right:DOMAIN_READ");
  ExpectReadPolicy(right_context, DomainUuid(204), true, {});
  ExpectReadPolicy(context,
                   DomainUuid(204),
                   false,
                   "domain.validate_value:domain_visibility_right_denied:domain_col:DOMAIN_READ");

  CreateDomain(context,
               DomainUuid(205),
               "visibility_unsupported_policy",
               "text",
               {},
               "future_visibility_policy");
  ExpectReadPolicy(context,
                   DomainUuid(205),
                   false,
                   "domain.validate_value:domain_visibility_policy_unsupported:domain_col");
}

}  // namespace

int main() {
  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  const auto context = BeginEngineTransaction(path, database_uuid);

  RequirePredicateSurface(context);
  RequireCreateAndAlterRefusals(context);
  RequireVisibilityPolicies(context);

  RemoveDatabaseArtifacts(path);
  std::cout << "sbsql_domain_policy_predicate_closure_conformance=passed\n";
  return EXIT_SUCCESS;
}
