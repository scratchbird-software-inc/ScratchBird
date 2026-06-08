// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/catalog_lookup_api.hpp"
#include "catalog/descriptor_api.hpp"
#include "catalog/name_resolution_api.hpp"
#include "catalog/schema_tree_api.hpp"
#include "database_lifecycle.hpp"
#include "ddl/alter_api.hpp"
#include "ddl/comment_api.hpp"
#include "ddl/create_api.hpp"
#include "ddl/drop_api.hpp"
#include "dml/delete_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/merge_api.hpp"
#include "dml/select_api.hpp"
#include "dml/update_api.hpp"
#include "extensibility/gpu_api.hpp"
#include "extensibility/llvm_api.hpp"
#include "extensibility/parser_package_api.hpp"
#include "extensibility/udr_api.hpp"
#include "management/config_api.hpp"
#include "management/management_api.hpp"
#include "management/support_bundle_api.hpp"
#include "nosql/document_api.hpp"
#include "nosql/graph_api.hpp"
#include "nosql/key_value_api.hpp"
#include "nosql/search_api.hpp"
#include "nosql/time_series_api.hpp"
#include "nosql/vector_api.hpp"
#include "observability/explain_api.hpp"
#include "observability/metrics_api.hpp"
#include "observability/show_api.hpp"
#include "query/expression_api.hpp"
#include "query/plan_api.hpp"
#include "query/predicate_api.hpp"
#include "query/projection_api.hpp"
#include "security/grant_api.hpp"
#include "security/identity_api.hpp"
#include "security/policy_api.hpp"
#include "security/visibility_api.hpp"
#include "transaction/savepoint_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

using namespace scratchbird::engine::internal_api;

namespace {

EngineRequestContext BaseContext(const std::string& path) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.request_id = "engine-api-noncluster-behavior-probe";
  context.database_path = path;
  context.trace_tags.push_back("group:ROOT");
  return context;
}

bool CreateProbeDatabase(const std::string& path) {
  constexpr std::uint64_t kCreationMillis = 1770000000000ULL;
  const auto database_uuid =
      scratchbird::core::uuid::GenerateEngineIdentityV7(scratchbird::core::platform::UuidKind::database,
                                                        kCreationMillis + 10);
  const auto filespace_uuid =
      scratchbird::core::uuid::GenerateEngineIdentityV7(scratchbird::core::platform::UuidKind::filespace,
                                                        kCreationMillis + 11);
  if (!database_uuid.ok()) {
    std::cerr << database_uuid.diagnostic.diagnostic_code << ":" << database_uuid.diagnostic.message_key << "\n";
    return false;
  }
  if (!filespace_uuid.ok()) {
    std::cerr << filespace_uuid.diagnostic.diagnostic_code << ":" << filespace_uuid.diagnostic.message_key << "\n";
    return false;
  }
  scratchbird::storage::database::DatabaseCreateConfig create;
  create.path = path;
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = kCreationMillis;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = scratchbird::storage::database::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ":" << created.diagnostic.message_key << "\n";
    return false;
  }
  return true;
}

EngineLocalizedName Name(const std::string& value) { return {"en", "default", value, value, true}; }
EngineTypedValue Value(const std::string& value) { EngineTypedValue typed; typed.encoded_value = value; return typed; }
EngineColumnDefinition Column(const std::string& name, const std::string& type, std::uint32_t ordinal) {
  EngineColumnDefinition column;
  column.names.push_back(Name(name));
  column.descriptor.canonical_type_name = type;
  column.ordinal = ordinal;
  return column;
}
EngineRowValue Row(const std::string& id, const std::string& name) {
  EngineRowValue row;
  row.fields.push_back({"id", Value(id)});
  row.fields.push_back({"name", Value(name)});
  return row;
}
EngineRowValue NumericRow(std::initializer_list<std::int64_t> values) {
  EngineRowValue row;
  std::size_t index = 0;
  for (const auto value : values) { row.fields.push_back({"c" + std::to_string(index++), Value(std::to_string(value))}); }
  return row;
}
EngineQueryRelation Relation(const std::string& name, std::initializer_list<EngineRowValue> rows) {
  EngineQueryRelation relation;
  relation.relation_name = name;
  relation.descriptor_digest = name;
  relation.rows.assign(rows.begin(), rows.end());
  return relation;
}

bool Ok(const EngineApiResult& result) {
  if (!result.ok) { return false; }
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.error) { return false; }
  }
  return true;
}
void CountLabeled(const char* label, const EngineApiResult& result, std::size_t* ok_count, std::size_t* fail_count) {
  if (Ok(result)) {
    ++(*ok_count);
    return;
  }
  ++(*fail_count);
  std::cerr << "FAIL " << label << " operation=" << result.operation_id << " ok=" << (result.ok ? "true" : "false") << "\n";
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << "  diagnostic code=" << diagnostic.code << " detail=" << diagnostic.detail
              << " error=" << (diagnostic.error ? "true" : "false") << "\n";
  }
}

#define Count(result, ok_count, fail_count) CountLabeled(#result, (result), (ok_count), (fail_count))

}  // namespace

int main(int argc, char** argv) {
  std::string path = "/tmp/sb_engine_api_noncluster_behavior_probe.db";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--path" && i + 1 < argc) { path = argv[++i]; }
  }
  std::filesystem::remove(path);
  std::filesystem::remove(path + ".sb.owner.lock");
  std::size_t ok_count = 0;
  std::size_t fail_count = 0;
  auto base = BaseContext(path);

  if (CreateProbeDatabase(path)) { ++ok_count; } else { ++fail_count; }
  EngineBeginTransactionRequest begin; begin.context = base; const auto tx = EngineBeginTransaction(begin); Count(tx, &ok_count, &fail_count);
  auto ctx = base; ctx.local_transaction_id = tx.local_transaction_id; ctx.transaction_uuid = tx.transaction_uuid;

  EngineCreateSchemaRequest schema; schema.context = ctx; schema.localized_names.push_back(Name("app")); const auto schema_result = EngineCreateSchema(schema); Count(schema_result, &ok_count, &fail_count);
  EngineCreateTableRequest table; table.context = ctx; table.table_names.push_back(Name("person")); table.table_columns.push_back(Column("id", "text", 1)); table.table_columns.push_back(Column("name", "text", 2)); const auto table_result = EngineCreateTable(table); Count(table_result, &ok_count, &fail_count);
  EngineCreateIndexRequest index; index.context = ctx; index.target_object = table_result.primary_object; EngineIndexDefinition idx; idx.names.push_back(Name("person_id_idx")); idx.index_kind = "btree_unique"; idx.key_envelopes.push_back("id"); index.indexes.push_back(idx); Count(EngineCreateIndex(index), &ok_count, &fail_count);
  EngineCreateDomainRequest domain; domain.context = ctx; domain.localized_names.push_back(Name("positive_int")); EngineDescriptor descriptor; descriptor.canonical_type_name = "int32"; domain.descriptors.push_back(descriptor); Count(EngineCreateDomain(domain), &ok_count, &fail_count);
  EngineCreateSequenceRequest seq; seq.context = ctx; seq.localized_names.push_back(Name("seq1")); Count(EngineCreateSequence(seq), &ok_count, &fail_count);
  EngineCreateViewRequest view; view.context = ctx; view.localized_names.push_back(Name("v_person")); Count(EngineCreateView(view), &ok_count, &fail_count);
  EngineCreateFunctionRequest fn; fn.context = ctx; fn.localized_names.push_back(Name("f1")); Count(EngineCreateFunction(fn), &ok_count, &fail_count);
  EngineCreateProcedureRequest proc; proc.context = ctx; proc.localized_names.push_back(Name("p1")); Count(EngineCreateProcedure(proc), &ok_count, &fail_count);
  EngineCreateTriggerRequest trig; trig.context = ctx; trig.localized_names.push_back(Name("tr1")); Count(EngineCreateTrigger(trig), &ok_count, &fail_count);

  EngineCreateSavepointRequest sp; sp.context = ctx; sp.localized_names.push_back(Name("sp1")); Count(EngineCreateSavepoint(sp), &ok_count, &fail_count);
  EngineInsertRowsRequest insert; insert.context = ctx; insert.target_table = table_result.primary_object; insert.input_rows.push_back(Row("1", "Ada")); const auto insert_result = EngineInsertRows(insert); Count(insert_result, &ok_count, &fail_count);
  EngineSelectRowsRequest select; select.context = ctx; select.source_object = table_result.primary_object; Count(EngineSelectRows(select), &ok_count, &fail_count);
  EngineUpdateRowsRequest update; update.context = ctx; update.target_table = table_result.primary_object; update.update_predicate.predicate_kind = "column_equals"; update.update_predicate.canonical_predicate_envelope = "id"; update.update_predicate.bound_values.push_back(Value("1")); update.assignments.push_back({"name", Value("Grace")}); Count(EngineUpdateRows(update), &ok_count, &fail_count);
  EngineMergeRowsRequest merge; merge.context = ctx; merge.target_object = table_result.primary_object; merge.match_predicate.predicate_kind = "column_equals"; merge.match_predicate.canonical_predicate_envelope = "id"; merge.match_predicate.bound_values.push_back(Value("2")); merge.rows.push_back(Row("2", "Lin")); Count(EngineMergeRows(merge), &ok_count, &fail_count);
  EngineDeleteRowsRequest del; del.context = ctx; del.target_table = table_result.primary_object; del.delete_predicate.predicate_kind = "column_equals"; del.delete_predicate.canonical_predicate_envelope = "id"; del.delete_predicate.bound_values.push_back(Value("2")); Count(EngineDeleteRows(del), &ok_count, &fail_count);
  EngineRollbackToSavepointRequest rbsp; rbsp.context = ctx; rbsp.localized_names.push_back(Name("sp1")); Count(EngineRollbackToSavepoint(rbsp), &ok_count, &fail_count);
  EngineReleaseSavepointRequest relsp; relsp.context = ctx; relsp.localized_names.push_back(Name("sp1")); Count(EngineReleaseSavepoint(relsp), &ok_count, &fail_count);

  EngineBindExpressionRequest expr; expr.context = ctx; Count(EngineBindExpression(expr), &ok_count, &fail_count);
  EngineBindPredicateRequest pred; pred.context = ctx; pred.predicate = update.update_predicate; Count(EngineBindPredicate(pred), &ok_count, &fail_count);
  EngineBindProjectionRequest proj; proj.context = ctx; proj.projection.canonical_projection_envelopes.push_back("name"); Count(EngineBindProjection(proj), &ok_count, &fail_count);
  EnginePlanOperationRequest plan; plan.context = ctx; plan.target_object = table_result.primary_object; plan.predicate = update.update_predicate; Count(EnginePlanOperation(plan), &ok_count, &fail_count);
  EnginePlanOperationRequest query_join; query_join.context = ctx; query_join.execute = true; query_join.query_operation = "join"; query_join.relations.push_back(Relation("left", {NumericRow({1, 100}), NumericRow({2, 200})})); query_join.relations.push_back(Relation("right", {NumericRow({1, 9}), NumericRow({3, 8})})); Count(EnginePlanOperation(query_join), &ok_count, &fail_count);
  EnginePlanOperationRequest query_agg; query_agg.context = ctx; query_agg.execute = true; query_agg.query_operation = "aggregate"; query_agg.relations.push_back(Relation("agg", {NumericRow({1, 10}), NumericRow({1, 20}), NumericRow({2, 5})})); query_agg.group_key_column = 0; query_agg.aggregate_value_column = 1; Count(EnginePlanOperation(query_agg), &ok_count, &fail_count);
  EnginePlanOperationRequest query_window; query_window.context = ctx; query_window.execute = true; query_window.query_operation = "window"; query_window.relations.push_back(Relation("window", {NumericRow({2}), NumericRow({1}), NumericRow({3})})); Count(EnginePlanOperation(query_window), &ok_count, &fail_count);
  EnginePlanOperationRequest query_set; query_set.context = ctx; query_set.execute = true; query_set.query_operation = "set_operation"; query_set.set_operation = "except"; query_set.relations.push_back(Relation("set_left", {NumericRow({1}), NumericRow({2})})); query_set.relations.push_back(Relation("set_right", {NumericRow({2})})); Count(EnginePlanOperation(query_set), &ok_count, &fail_count);
  EnginePlanOperationRequest query_cte; query_cte.context = ctx; query_cte.execute = true; query_cte.query_operation = "recursive_cte"; query_cte.relations.push_back(Relation("cte", {NumericRow({7})})); Count(EnginePlanOperation(query_cte), &ok_count, &fail_count);
  EnginePlanOperationRequest query_subquery; query_subquery.context = ctx; query_subquery.execute = true; query_subquery.query_operation = "correlated_subquery"; query_subquery.relations.push_back(Relation("subquery", {NumericRow({42})})); Count(EnginePlanOperation(query_subquery), &ok_count, &fail_count);

  EngineResolveNameRequest resolve; resolve.context = ctx; resolve.localized_names.push_back(Name("person")); Count(EngineResolveName(resolve), &ok_count, &fail_count);
  EngineLookupObjectRequest lookup; lookup.context = ctx; lookup.target_object = table_result.primary_object; Count(EngineLookupObject(lookup), &ok_count, &fail_count);
  EngineListCatalogChildrenRequest children; children.context = ctx; Count(EngineListCatalogChildren(children), &ok_count, &fail_count);
  EngineGetDescriptorRequest getdesc; getdesc.context = ctx; getdesc.target_object = table_result.primary_object; Count(EngineGetDescriptor(getdesc), &ok_count, &fail_count);
  EngineGetDependenciesRequest deps; deps.context = ctx; deps.target_object = table_result.primary_object; deps.related_objects.push_back(schema_result.primary_object); Count(EngineGetDependencies(deps), &ok_count, &fail_count);

  EngineCreateIdentityRequest ident; ident.context = ctx; ident.localized_names.push_back(Name("user1")); Count(EngineCreateIdentity(ident), &ok_count, &fail_count);
  EngineAlterIdentityRequest aident; aident.context = ctx; aident.localized_names.push_back(Name("user1")); Count(EngineAlterIdentity(aident), &ok_count, &fail_count);
  EngineGrantRightRequest grant; grant.context = ctx; grant.option_envelopes.push_back("OBS_INDEX_PROFILE_READ:DEV"); const auto grant_result = EngineGrantRight(grant); if (grant_result.ok) ++ok_count; else ++fail_count;
  EngineRevokeRightRequest revoke; revoke.context = ctx; Count(EngineRevokeRight(revoke), &ok_count, &fail_count);
  EngineEvaluateVisibilityRequest vis; vis.context = ctx; vis.target_object = table_result.primary_object; Count(EngineEvaluateVisibility(vis), &ok_count, &fail_count);
  EngineEvaluatePolicyRequest pol; pol.context = ctx; Count(EngineEvaluatePolicy(pol), &ok_count, &fail_count);

  EngineShowVersionRequest sv; sv.context = ctx; Count(EngineShowVersion(sv), &ok_count, &fail_count);
  EngineShowDatabaseRequest sd; sd.context = ctx; Count(EngineShowDatabase(sd), &ok_count, &fail_count);
  EngineShowSystemRequest ss; ss.context = ctx; Count(EngineShowSystem(ss), &ok_count, &fail_count);
  EngineShowCatalogRequest sc; sc.context = ctx; Count(EngineShowCatalog(sc), &ok_count, &fail_count);
  EngineShowSessionsRequest ses; ses.context = ctx; Count(EngineShowSessions(ses), &ok_count, &fail_count);
  EngineShowTransactionsRequest st; st.context = ctx; Count(EngineShowTransactions(st), &ok_count, &fail_count);
  EngineShowLocksRequest sl; sl.context = ctx; Count(EngineShowLocks(sl), &ok_count, &fail_count);
  EngineShowStatementsRequest sst; sst.context = ctx; Count(EngineShowStatements(sst), &ok_count, &fail_count);
  EngineShowMetricsRequest sm; sm.context = ctx; Count(EngineShowMetrics(sm), &ok_count, &fail_count);
  EngineExplainOperationRequest ex; ex.context = ctx; ex.operation_id = "dml.select_rows"; Count(EngineExplainOperation(ex), &ok_count, &fail_count);

  EngineInspectConfigRequest ic; ic.context = ctx; Count(EngineInspectConfig(ic), &ok_count, &fail_count);
  EngineSetConfigRequest setc; setc.context = ctx; setc.localized_names.push_back(Name("cache.pages")); setc.option_envelopes.push_back("value:128"); Count(EngineSetConfig(setc), &ok_count, &fail_count);
  EngineResetConfigRequest resetc; resetc.context = ctx; resetc.localized_names.push_back(Name("cache.pages")); Count(EngineResetConfig(resetc), &ok_count, &fail_count);
  EngineInspectManagementRuntimeRequest im; im.context = ctx; Count(EngineInspectManagementRuntime(im), &ok_count, &fail_count);
  EngineControlManagementRuntimeRequest cm; cm.context = ctx; cm.option_envelopes.push_back("noop"); Count(EngineControlManagementRuntime(cm), &ok_count, &fail_count);
  EnginePrepareSupportBundleRequest psb; psb.context = ctx; Count(EnginePrepareSupportBundle(psb), &ok_count, &fail_count);

  EngineRegisterUdrPackageRequest udr; udr.context = ctx; udr.localized_names.push_back(Name("udr_pkg")); udr.option_envelopes.push_back("permission:manage_udr"); udr.option_envelopes.push_back("trust:trusted_cpp"); udr.option_envelopes.push_back("abi:sb_udr_1"); Count(EngineRegisterUdrPackage(udr), &ok_count, &fail_count);
  EngineRegisterParserPackageRequest parser; parser.context = ctx; parser.localized_names.push_back(Name("parser_pkg")); Count(EngineRegisterParserPackage(parser), &ok_count, &fail_count);
  EngineCompileLlvmModuleRequest llvm; llvm.context = ctx; llvm.option_envelopes.push_back("compile:jit"); llvm.option_envelopes.push_back("module:sblr_empty_fragment"); Count(EngineCompileLlvmModule(llvm), &ok_count, &fail_count);
  EngineInspectGpuCapabilityRequest gpu; gpu.context = ctx; Count(EngineInspectGpuCapability(gpu), &ok_count, &fail_count);

  EngineDocumentInsertRequest di; di.context = ctx; di.localized_names.push_back(Name("doc1")); di.rows.push_back(Row("doc1", "body")); Count(EngineDocumentInsert(di), &ok_count, &fail_count);
  EngineDocumentFindRequest df; df.context = ctx; Count(EngineDocumentFind(df), &ok_count, &fail_count);
  EngineDocumentUpdateRequest du; du.context = ctx; du.localized_names.push_back(Name("doc1")); Count(EngineDocumentUpdate(du), &ok_count, &fail_count);
  EngineDocumentDeleteRequest dd; dd.context = ctx; dd.localized_names.push_back(Name("doc1")); Count(EngineDocumentDelete(dd), &ok_count, &fail_count);
  EngineGraphQueryRequest gq; gq.context = ctx; Count(EngineGraphQuery(gq), &ok_count, &fail_count);
  EngineKeyValuePutRequest kvp; kvp.context = ctx; kvp.localized_names.push_back(Name("k1")); kvp.option_envelopes.push_back("value:v1"); Count(EngineKeyValuePut(kvp), &ok_count, &fail_count);
  EngineKeyValueGetRequest kvg; kvg.context = ctx; kvg.localized_names.push_back(Name("k1")); Count(EngineKeyValueGet(kvg), &ok_count, &fail_count);
  EngineTimeSeriesAppendRequest tsa; tsa.context = ctx; Count(EngineTimeSeriesAppend(tsa), &ok_count, &fail_count);
  EngineVectorSearchRequest vs; vs.context = ctx; Count(EngineVectorSearch(vs), &ok_count, &fail_count);
  EngineSearchQueryRequest sq; sq.context = ctx; Count(EngineSearchQuery(sq), &ok_count, &fail_count);

  EngineAlterObjectRequest alter; alter.context = ctx; alter.target_object = table_result.primary_object; Count(EngineAlterObject(alter), &ok_count, &fail_count);
  EngineCommentOnObjectRequest comment; comment.context = ctx; comment.target_object = table_result.primary_object; comment.option_envelopes.push_back("comment:hello"); Count(EngineCommentOnObject(comment), &ok_count, &fail_count);
  EngineDropObjectRequest drop; drop.context = ctx; drop.target_object = schema_result.primary_object; Count(EngineDropObject(drop), &ok_count, &fail_count);

  EngineCommitTransactionRequest commit; commit.context = ctx; Count(EngineCommitTransaction(commit), &ok_count, &fail_count);
  EngineBeginTransactionRequest prep_begin; prep_begin.context = base; const auto prep_tx = EngineBeginTransaction(prep_begin); Count(prep_tx, &ok_count, &fail_count);
  auto prep_ctx = base; prep_ctx.local_transaction_id = prep_tx.local_transaction_id; prep_ctx.transaction_uuid = prep_tx.transaction_uuid;
  EnginePrepareTransactionRequest prep; prep.context = prep_ctx; Count(EnginePrepareTransaction(prep), &ok_count, &fail_count);
  EngineBeginTransactionRequest rollback_begin; rollback_begin.context = base; const auto rollback_tx = EngineBeginTransaction(rollback_begin); Count(rollback_tx, &ok_count, &fail_count);
  auto rollback_ctx = base; rollback_ctx.local_transaction_id = rollback_tx.local_transaction_id; rollback_ctx.transaction_uuid = rollback_tx.transaction_uuid;
  EngineRollbackTransactionRequest rollback_success; rollback_success.context = rollback_ctx; Count(EngineRollbackTransaction(rollback_success), &ok_count, &fail_count);
  EngineRollbackTransactionRequest rb; rb.context = base; const auto rollback_without_tx = EngineRollbackTransaction(rb); const bool rollback_rejected = !rollback_without_tx.ok;

  const bool ok = fail_count == 0 && ok_count >= 70 && rollback_rejected;
  std::cout << "{\n";
  std::cout << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
  std::cout << "  \"ok_count\": " << ok_count << ",\n";
  std::cout << "  \"fail_count\": " << fail_count << ",\n";
  std::cout << "  \"rollback_rejected_without_transaction\": " << (rollback_rejected ? "true" : "false") << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}
