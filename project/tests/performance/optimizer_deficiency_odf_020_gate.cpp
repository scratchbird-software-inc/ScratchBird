// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"
#include "prepared_execution_template.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_prepared_template.hpp"

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace sblr = scratchbird::engine::sblr;

namespace {

static_assert(!std::is_aggregate_v<
              exec::PreparedTemplateStatementUseReceipt>);
static_assert(!std::is_default_constructible_v<
              exec::PreparedTemplateStatementUseReceipt>);

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool Has(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

template <typename Slot>
bool StableNamesAreUnique(const std::vector<Slot>& slots) {
  std::vector<std::string> names;
  names.reserve(slots.size());
  for (const auto& slot : slots) names.push_back(slot.stable_name);
  std::sort(names.begin(), names.end());
  return std::adjacent_find(names.begin(), names.end()) == names.end();
}

api::EngineUuid Uuid(const std::string& value) {
  api::EngineUuid uuid;
  uuid.canonical = value;
  return uuid;
}

api::EngineDescriptor Descriptor(const std::string& uuid,
                                 const std::string& type,
                                 const std::string& encoded) {
  auto descriptor = exec::MakeExecutorDescriptor(type, encoded);
  descriptor.descriptor_uuid = Uuid(uuid);
  descriptor.descriptor_kind = "executor.scalar";
  return descriptor;
}

api::EngineColumnDefinition Column(const std::string& uuid,
                                   const std::string& type,
                                   std::uint32_t ordinal) {
  api::EngineColumnDefinition column;
  column.requested_column_uuid = Uuid(uuid);
  column.descriptor = Descriptor("desc:" + uuid, type, "type=" + type + ";uuid=" + uuid);
  column.ordinal = ordinal;
  column.nullable = false;
  return column;
}

api::EngineRequestContext BaseContext() {
  api::EngineRequestContext context;
  context.database_uuid = Uuid("db.odf020");
  context.principal_uuid = Uuid("principal.odf020");
  context.current_role_uuid = Uuid("role.reader");
  context.session_uuid = Uuid("session.odf020");
  context.transaction_uuid =
      Uuid("019f0200-0000-7000-8000-000000000002");
  context.statement_uuid =
      Uuid("019f0200-0000-7000-8000-000000000003");
  context.statement_snapshot_uuid =
      Uuid("019f0200-0000-7000-8000-000000000004");
  context.statement_metadata_snapshot_uuid =
      Uuid("019f0200-0000-7000-8000-000000000005");
  context.catalog_epoch_uuid =
      Uuid("019f0200-0000-7000-8000-000000000001");
  context.local_transaction_id = 88;
  context.snapshot_visible_through_local_transaction_id = 0;
  context.transaction_isolation_level = "snapshot";
  context.security_context_present = true;
  context.catalog_generation_id = 20;
  context.security_epoch = 21;
  context.resource_epoch = 22;
  context.name_resolution_epoch = 23;
  return context;
}

exec::PhysicalMgaStatementContext StatementContext(
    const api::EngineRequestContext& context) {
  exec::PhysicalMgaStatementContext statement;
  statement.statement_uuid = context.statement_uuid.canonical;
  statement.owning_transaction_uuid = context.transaction_uuid.canonical;
  statement.statement_snapshot_uuid =
      context.statement_snapshot_uuid.canonical;
  statement.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  statement.owning_local_transaction_id = context.local_transaction_id;
  statement.visible_committed_high_watermark =
      context.snapshot_visible_through_local_transaction_id;
  statement.oldest_active_transaction_id = 40;
  statement.oldest_interesting_transaction_id = 40;
  statement.oldest_snapshot_transaction_id = 40;
  statement.retention_horizon_transaction_id = 40;
  statement.active_excluded_local_transaction_ids = {
      context.local_transaction_id};
  statement.in_doubt_excluded_local_transaction_ids = {77};
  statement.snapshot_kind = "statement_stable";
  statement.publication_inventory_next_local_transaction_id = 100;
  statement.inventory_authoritative = true;
  statement.complete = true;
  statement.current = true;
  return statement;
}

exec::CanonicalExecutionMgaAuthority Authority(
    const api::EngineRequestContext& context) {
  exec::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = StatementContext(context);
  const auto current = authority.statement_context;
  authority.resolve_current = [current]() {
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context = current;
    return resolution;
  };
  authority.origin = exec::CanonicalMgaAuthorityOrigin::kEngineTransactionInventory;
  return authority;
}

api::EngineApiRequest BaseRequest(const api::EngineRequestContext& context) {
  api::EngineApiRequest request;
  request.context = context;
  request.operation_id = "query.execute";
  request.target_database.uuid = Uuid("db.odf020");
  request.target_database.object_kind = "database";
  request.target_schema.uuid = Uuid("schema.public");
  request.target_schema.object_kind = "schema";
  request.target_object.uuid = Uuid("rel.customer");
  request.target_object.object_kind = "relation";
  request.related_objects = {
      {Uuid("rel.customer"), "relation"},
      {Uuid("fn.redaction_policy"), "function"},
  };
  request.columns = {
      Column("col.customer_id", "int64", 0),
      Column("col.customer_name", "text", 1),
  };
  request.descriptors = {
      request.columns[0].descriptor,
      request.columns[1].descriptor,
  };
  request.bound_object_identity.object_uuid = request.target_object.uuid;
  request.bound_object_identity.resolved_schema_uuid = request.target_schema.uuid;
  request.bound_object_identity.catalog_generation_id = context.catalog_generation_id;
  request.bound_object_identity.security_epoch = context.security_epoch;
  request.bound_object_identity.resource_epoch = context.resource_epoch;
  request.predicate.predicate_kind = "scalar_eq";
  request.predicate.canonical_predicate_envelope = "sblr.predicate.uuid_bound.v1";
  request.predicate.bound_values.push_back(exec::MakeExecutorValue(request.columns[0].descriptor, "42", false));
  api::EngineIndexDefinition index;
  index.requested_index_uuid = Uuid("idx.customer.customer_id");
  index.index_kind = "btree";
  index.key_envelopes = {"col.customer_id"};
  index.physical_profile = "mga_visible_index";
  request.indexes.push_back(index);
  request.policy_profile.names = {"tenant_visibility", "role_authorization"};
  request.policy_profile.encoded_profiles = {"policy_uuid=policy.customer.visible"};
  return request;
}

sblr::SblrOperationEnvelope BaseEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.execute",
                                         "SBLR_QUERY_EXECUTE",
                                         "trace.odf020.select.customer");
  envelope.opcode_code = 0x1207;
  envelope.parser_package_uuid =
      "019f0200-0000-7000-8000-000000000101";
  envelope.registry_snapshot_uuid =
      "019f0200-0000-7000-8000-000000000102";
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.result_shape = "engine.result.customer_projection.v1";
  return envelope;
}

exec::PreparedTemplateBindContext BuildBindContext(const sblr::SblrOperationEnvelope& envelope,
                                                   const api::EngineRequestContext& context,
                                                   const api::EngineApiRequest& request,
                                                   exec::PreparedTemplateAdmission* admission = nullptr) {
  auto build = sblr::BuildPreparedTemplateFromSblr(
      envelope, context, request, Authority(context));
  if (admission != nullptr) *admission = build.admission;
  return build.bind_context;
}

bool FirstPreparePopulatesTemplateAndSecondReusesIt() {
  exec::PreparedTemplateCache cache;
  const auto context = BaseContext();
  const auto request = BaseRequest(context);
  const auto envelope = BaseEnvelope();
  auto build = sblr::BuildPreparedTemplateFromSblr(
      envelope, context, request, Authority(context));
  if (!Require(build.ok, "SBLR prepared template build failed: " + build.diagnostic_code) ||
      !Require(Has(build.evidence, "parser_sql_text_authority=false"),
               "SBLR builder did not reject parser SQL text authority") ||
      !Require(Has(build.evidence, "uuid_bound_descriptors_authority=true"),
               "SBLR builder did not record UUID-bound descriptor authority")) {
    return false;
  }

  auto first = cache.Prepare(build.admission);
  auto second = cache.Prepare(build.admission);
  if (!Require(first.ok, "first prepare failed: " + first.diagnostic_code) ||
      !Require(!first.reused_existing_template, "first prepare incorrectly reused a template") ||
      !Require(second.ok, "second prepare failed: " + second.diagnostic_code) ||
      !Require(second.reused_existing_template, "second prepare did not reuse exact template") ||
      !Require(first.prepared_template.get() == second.prepared_template.get(),
               "second prepare reused a different template instance")) {
    return false;
  }

  const auto& prepared = *first.prepared_template;
  return Require(prepared.descriptor_slots.size() == 2, "descriptor slots were not populated") &&
         Require(prepared.field_offsets.size() == 2, "field offsets were not populated") &&
         Require(prepared.result_shape.columns.size() == 2, "result shape descriptor was not populated") &&
         Require(prepared.predicate_slots.size() == 1, "predicate slots were not de-duplicated") &&
         Require(prepared.parameter_slots.size() == 1, "parameter slots were not de-duplicated") &&
         Require(StableNamesAreUnique(prepared.predicate_slots), "predicate slot stable names were duplicated") &&
         Require(StableNamesAreUnique(prepared.parameter_slots), "parameter slot stable names were duplicated") &&
         Require(prepared.index_descriptors.size() == 1, "index descriptor was not populated") &&
         Require(prepared.key.epochs.catalog_epoch == context.catalog_generation_id,
                 "catalog epoch was not cached in the template key") &&
         Require(prepared.key.epochs.security_epoch == context.security_epoch,
                 "security epoch was not cached in the template key") &&
         Require(prepared.key.epochs.policy_resource_epoch == context.resource_epoch,
                 "policy/resource epoch was not cached in the template key") &&
         Require(prepared.key.epochs.name_resolution_epoch == context.name_resolution_epoch,
                 "name-resolution epoch was not cached in the template key") &&
         Require(prepared.key.catalog_epoch_uuid ==
                     context.catalog_epoch_uuid.canonical,
                 "catalog epoch UUID was not cached independently") &&
         Require(exec::PreparedTemplateCanonicalKey(prepared.key).find(
                     "visibility") == std::string::npos,
                 "shared template identity retained transaction visibility");
}

bool BindRefusesWithExactDiagnostics() {
  exec::PreparedTemplateCache cache;
  const auto context = BaseContext();
  const auto request = BaseRequest(context);
  const auto envelope = BaseEnvelope();
  exec::PreparedTemplateAdmission admission;
  auto bind_context = BuildBindContext(envelope, context, request, &admission);
  auto prepared = cache.Prepare(admission).prepared_template;

  const auto expect_code = [&](exec::PreparedTemplateBindContext changed,
                               const std::string& code,
                               const std::string& message) {
    const auto result = cache.Bind(*prepared, changed);
    return Require(!result.ok, message + " unexpectedly succeeded") &&
           Require(result.diagnostic_code == code,
                   message + " diagnostic mismatch: " + result.diagnostic_code) &&
           Require(!result.statement_use_receipt,
                   message + " emitted an executable statement-use receipt");
  };

  auto changed = bind_context;
  changed.engine_context.catalog_generation_id += 1;
  if (!expect_code(changed, "SB_PREPARED_TEMPLATE_STALE_CATALOG_EPOCH", "stale catalog epoch")) return false;

  changed = bind_context;
  changed.engine_context.security_epoch += 1;
  if (!expect_code(changed, "SB_PREPARED_TEMPLATE_STALE_SECURITY_EPOCH", "stale security epoch")) return false;

  changed = bind_context;
  changed.engine_context.resource_epoch += 1;
  if (!expect_code(changed, "SB_PREPARED_TEMPLATE_STALE_POLICY_RESOURCE_EPOCH", "stale policy/resource epoch")) {
    return false;
  }

  changed = bind_context;
  changed.engine_context.name_resolution_epoch += 1;
  if (!expect_code(changed, "SB_PREPARED_TEMPLATE_STALE_NAME_RESOLUTION_EPOCH", "stale name-resolution epoch")) {
    return false;
  }

  changed = bind_context;
  changed.engine_context.principal_uuid = Uuid("principal.odf020.changed");
  if (!expect_code(changed, "SB_PREPARED_TEMPLATE_POLICY_METADATA_MISMATCH", "changed authorization identity")) {
    return false;
  }

  changed = bind_context;
  changed.engine_context.snapshot_visible_through_local_transaction_id += 1;
  if (!expect_code(changed, "SB_PREPARED_TEMPLATE_MGA_REQUEST_CONTEXT_MISMATCH", "request high-water mismatch")) {
    return false;
  }

  changed = bind_context;
  changed.mga_authority = {};
  if (!expect_code(changed, "SB_PREPARED_TEMPLATE_MGA_AUTHORITY_REQUIRED", "missing MGA authority")) {
    return false;
  }

  changed = bind_context;
  changed.mga_authority.statement_context.statement_uuid = "malformed";
  if (!expect_code(changed, "SB_PREPARED_TEMPLATE_MGA_STATEMENT_CONTEXT_INVALID", "malformed MGA context")) {
    return false;
  }

  changed = bind_context;
  changed.mga_authority.resolve_current = [current = StatementContext(context)]() mutable {
    current.current = false;
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context = current;
    return resolution;
  };
  if (!expect_code(changed, "SB_PREPARED_TEMPLATE_MGA_CURRENT_CONTEXT_INVALID", "noncurrent resolver context")) {
    return false;
  }

  changed = bind_context;
  changed.mga_authority.resolve_current = [current = StatementContext(context)]() mutable {
    current.owning_transaction_uuid =
        "019f0200-0000-7000-8000-000000000012";
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context = current;
    return resolution;
  };
  if (!expect_code(changed, "SB_PREPARED_TEMPLATE_MGA_CURRENT_CONTEXT_MISMATCH", "wrong current owner")) {
    return false;
  }

  changed = bind_context;
  changed.mga_authority.resolve_current = [current = StatementContext(context)]() mutable {
    current.statement_uuid = "019f0200-0000-7000-8000-000000000013";
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context = current;
    return resolution;
  };
  if (!expect_code(changed, "SB_PREPARED_TEMPLATE_MGA_CURRENT_CONTEXT_MISMATCH", "cross-statement resolver context")) {
    return false;
  }

  changed = bind_context;
  changed.mga_authority.resolve_current = [current = StatementContext(context)]() mutable {
    current.active_excluded_local_transaction_ids.push_back(90);
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context = current;
    return resolution;
  };
  if (!expect_code(changed, "SB_PREPARED_TEMPLATE_MGA_CURRENT_CONTEXT_MISMATCH", "changed exclusion vector")) {
    return false;
  }

  changed = bind_context;
  changed.mga_authority.resolve_current = []() {
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.diagnostic.ok = false;
    resolution.diagnostic.diagnostic_code = "SB_TEST_MGA_RESOLVER_REFUSED";
    resolution.diagnostic.detail = "test resolver refusal";
    return resolution;
  };
  if (!expect_code(changed, "SB_TEST_MGA_RESOLVER_REFUSED", "resolver refusal")) {
    return false;
  }

  changed = bind_context;
  changed.descriptor_set_digest = "changed.descriptor.digest";
  if (!expect_code(changed, "SB_PREPARED_TEMPLATE_DESCRIPTOR_MISMATCH", "changed descriptor set")) return false;

  changed = bind_context;
  changed.result_shape_digest = "changed.result.shape";
  if (!expect_code(changed, "SB_PREPARED_TEMPLATE_RESULT_SHAPE_MISMATCH", "changed result shape")) return false;

  changed = bind_context;
  changed.available_predicate_slots.clear();
  if (!expect_code(changed, "SB_PREPARED_TEMPLATE_MISSING_PREDICATE_SLOT", "missing predicate slot")) return false;

  changed = bind_context;
  changed.available_parameter_slots.clear();
  if (!expect_code(changed, "SB_PREPARED_TEMPLATE_MISSING_PARAMETER_SLOT", "missing parameter slot")) return false;

  changed = bind_context;
  changed.engine_context.security_context_present = false;
  if (!expect_code(changed, "SB_PREPARED_TEMPLATE_MISSING_SECURITY_CONTEXT", "missing security context")) return false;

  changed = bind_context;
  changed.engine_context.transaction_uuid = {};
  changed.engine_context.local_transaction_id = 0;
  if (!expect_code(changed, "SB_PREPARED_TEMPLATE_MISSING_TRANSACTION_CONTEXT", "missing transaction context")) {
    return false;
  }

  return true;
}

bool SuccessfulBindPreservesMGAAndSecurityRechecks() {
  exec::PreparedTemplateCache cache;
  const auto context = BaseContext();
  const auto request = BaseRequest(context);
  const auto envelope = BaseEnvelope();
  auto build = sblr::BuildPreparedTemplateFromSblr(
      envelope, context, request, Authority(context));
  auto prepared = cache.Prepare(build.admission).prepared_template;
  const auto unissued = exec::RevalidatePreparedTemplateStatementUse(
      *prepared, {});
  const auto result = cache.LookupAndBind(build.admission.key, build.bind_context);
  return Require(!unissued.ok && !unissued.executable_receipt,
                 "unissued statement-use receipt became executable") &&
         Require(unissued.diagnostic_code ==
                     "SB_PREPARED_TEMPLATE_STATEMENT_USE_RECEIPT_REQUIRED",
                 "unissued receipt diagnostic mismatch") &&
         Require(result.ok, "bind failed: " + result.diagnostic_code) &&
         Require(Has(result.evidence, "prepared_template_cached_metadata_only=true"),
                 "bind evidence did not prove metadata-only template caching") &&
         Require(Has(result.evidence, "mga_visibility_recheck=preserved"),
                 "bind evidence did not preserve MGA visibility recheck") &&
         Require(Has(result.evidence, "mga_finality_authority=engine_transaction_inventory"),
                 "bind evidence did not preserve MGA finality authority") &&
         Require(Has(result.evidence, "security_authorization_recheck=preserved"),
                 "bind evidence did not preserve security recheck") &&
         Require(result.statement_use_receipt != nullptr,
                 "successful bind did not issue an immutable use receipt") &&
         Require(result.statement_use_receipt->statement_context()
                         .visible_committed_high_watermark == 0,
                 "zero committed high-water was not preserved as valid") &&
         Require(exec::RevalidatePreparedTemplateStatementUse(
                     *prepared, result.statement_use_receipt)
                     .ok,
                 "statement-use receipt did not revalidate before use") &&
         Require(prepared->policy_metadata.cached_metadata_only,
                 "prepared template did not mark policy metadata as cache-only") &&
         Require(prepared->policy_metadata.visibility_recheck_required,
                 "prepared template did not require visibility recheck") &&
         Require(prepared->policy_metadata.security_recheck_required,
                 "prepared template did not require security recheck") &&
         Require(!prepared->policy_metadata.finality_authority_cached,
                 "prepared template cached finality authority");
}

bool SharedMetadataReusesAcrossStatementsButReceiptsDoNot() {
  exec::PreparedTemplateCache cache;
  const auto first_context = BaseContext();
  const auto first_request = BaseRequest(first_context);
  const auto envelope = BaseEnvelope();
  const auto first_build = sblr::BuildPreparedTemplateFromSblr(
      envelope, first_context, first_request, Authority(first_context));
  const auto first_prepare = cache.Prepare(first_build.admission);
  const auto first_bind = cache.Bind(*first_prepare.prepared_template,
                                     first_build.bind_context);

  auto second_context = first_context;
  second_context.transaction_uuid =
      Uuid("019f0200-0000-7000-8000-000000000022");
  second_context.statement_uuid =
      Uuid("019f0200-0000-7000-8000-000000000023");
  second_context.statement_snapshot_uuid =
      Uuid("019f0200-0000-7000-8000-000000000024");
  second_context.statement_metadata_snapshot_uuid =
      Uuid("019f0200-0000-7000-8000-000000000025");
  second_context.local_transaction_id = 89;
  second_context.snapshot_visible_through_local_transaction_id = 56;
  auto second_request = BaseRequest(second_context);
  const auto second_build = sblr::BuildPreparedTemplateFromSblr(
      envelope, second_context, second_request, Authority(second_context));
  const auto second_prepare = cache.Prepare(second_build.admission);
  const auto second_bind = cache.Bind(*second_prepare.prepared_template,
                                      second_build.bind_context);

  return Require(first_build.ok && second_build.ok,
                 "cross-statement template builds failed") &&
         Require(second_prepare.reused_existing_template,
                 "shared metadata was not reused across statements") &&
         Require(first_prepare.prepared_template.get() ==
                     second_prepare.prepared_template.get(),
                 "cross-statement reuse selected different shared metadata") &&
         Require(first_bind.ok && second_bind.ok,
                 "cross-statement use receipt bind failed") &&
         Require(first_bind.statement_use_receipt &&
                     second_bind.statement_use_receipt,
                 "cross-statement bind omitted a use receipt") &&
         Require(first_bind.statement_use_receipt->receipt_id() !=
                     second_bind.statement_use_receipt->receipt_id(),
                 "different statements shared one use receipt") &&
         Require(first_bind.statement_use_receipt->statement_context()
                         .statement_uuid !=
                     second_bind.statement_use_receipt->statement_context()
                         .statement_uuid,
                 "cross-statement receipts lost exact statement identity");
}

}  // namespace

int main() {
  if (!FirstPreparePopulatesTemplateAndSecondReusesIt()) return 1;
  if (!BindRefusesWithExactDiagnostics()) return 1;
  if (!SuccessfulBindPreservesMGAAndSecurityRechecks()) return 1;
  if (!SharedMetadataReusesAcrossStatementsButReceiptsDoNot()) return 1;
  return 0;
}
