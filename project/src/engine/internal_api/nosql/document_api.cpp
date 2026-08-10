// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/document_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "crud_support/crud_store.hpp"
#include "index_route_capability.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "nosql/document_path_physical_provider.hpp"
#include "nosql/nosql_batch_point_lookup_support.hpp"
#include "nosql/nosql_provider_generation_store.hpp"
#include "nosql/nosql_surface_support.hpp"
#include "query/expression_api.hpp"
#include "security/security_model.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

namespace idx = scratchbird::core::index;

struct PhysicalDocumentRecord {
  std::string collection_uuid;
  std::string document_uuid;
  std::string row_uuid;
  std::string name;
  std::string payload;
  std::map<std::string, std::string> fragments;
  std::set<std::string> null_paths;
  std::string shape_id;
  EngineApiU64 shape_ref_count = 0;
  EngineApiU64 creator_tx = 0;
};

using DocumentMap = std::map<std::string, PhysicalDocumentRecord>;

struct DocumentProviderState {
  bool loaded = false;
  DocumentMap documents;
  std::map<std::string, std::string> shape_dictionary;
  std::map<std::string, EngineApiU64> shape_ref_counts;
  std::map<std::string, std::vector<std::string>> exact_path_value_index;
  std::map<std::string, std::vector<std::string>> path_index;
  EngineApiU64 provider_generation_id = 0;
};

std::map<std::string, DocumentProviderState>& DocumentStores() {
  static std::map<std::string, DocumentProviderState> stores;
  return stores;
}

std::mutex& DocumentStoresMutex() {
  static std::mutex mutex;
  return mutex;
}

std::string StoreKey(const EngineRequestContext& context) {
  return EngineNoSqlProviderDatabaseIdentity(context);
}

std::string DocumentProviderPath(const EngineRequestContext& context) {
  if (context.database_path.empty()) { return {}; }
  return context.database_path + ".sb.nosql_document_provider";
}

std::string DocumentCollectionUuid(const EngineRequestContext& context) {
  if (!context.current_schema_uuid.canonical.empty()) {
    return context.current_schema_uuid.canonical;
  }
  return DocumentPathProviderIdentityForContext(context, 1).relation_uuid;
}

bool IsEngineUuidText(const std::string& value) {
  const auto parsed = scratchbird::core::uuid::ParseUuid(value);
  return parsed.ok() &&
         scratchbird::core::uuid::IsEngineIdentityUuid(parsed.value);
}

std::string ProviderUuidOrGenerated(const std::string& value,
                                    const std::string& kind) {
  if (IsEngineUuidText(value)) { return value; }
  return GenerateCrudEngineUuid(kind);
}

void NormalizeProviderRecordIdentity(PhysicalDocumentRecord* record) {
  if (record == nullptr) { return; }
  record->document_uuid = ProviderUuidOrGenerated(record->document_uuid, "object");
  record->row_uuid = ProviderUuidOrGenerated(record->row_uuid, "row");
  if (record->name.empty()) { record->name = record->document_uuid; }
}

std::string RowField(const EngineApiResult& result, const std::string& field) {
  if (result.result_shape.rows.empty()) { return {}; }
  for (const auto& [name, value] : result.result_shape.rows.front().fields) {
    if (name == field) { return value.encoded_value; }
  }
  return {};
}

std::string RequestDocumentName(const EngineApiRequest& request,
                                const std::string& fallback) {
  if (!request.localized_names.empty() &&
      !request.localized_names.front().name.empty()) {
    return request.localized_names.front().name;
  }
  if (!request.target_object.uuid.canonical.empty()) {
    return request.target_object.uuid.canonical;
  }
  return fallback;
}

std::map<std::string, std::string> ParsePayloadFragments(
    const EngineApiRequest& request,
    const std::string& persisted_payload,
    std::set<std::string>* null_paths) {
  std::map<std::string, std::string> fragments;
  for (const auto& [path, value] : request.assignments) {
    if (!path.empty()) {
      fragments[path] = value.encoded_value;
      if (value.isSqlNull()) null_paths->insert(path);
    }
  }
  for (const auto& row : request.rows) {
    for (const auto& [path, value] : row.fields) {
      if (!path.empty()) {
        fragments[path] = value.encoded_value;
        if (value.isSqlNull()) null_paths->insert(path);
      }
    }
  }
  if (!fragments.empty()) { return fragments; }

  std::stringstream stream(persisted_payload);
  std::string pair;
  while (std::getline(stream, pair, ';')) {
    const auto equals = pair.find('=');
    if (equals == std::string::npos || equals == 0) { continue; }
    fragments[pair.substr(0, equals)] = pair.substr(equals + 1);
  }
  return fragments;
}

std::string ShapeKey(const std::map<std::string, std::string>& fragments) {
  std::string key;
  for (const auto& [path, value] : fragments) {
    if (!key.empty()) { key += '|'; }
    key += path;
  }
  return key;
}

std::string InternShape(DocumentProviderState* state,
                        const std::map<std::string, std::string>& fragments) {
  const auto shape_key = ShapeKey(fragments);
  auto& dictionary = state->shape_dictionary;
  auto it = dictionary.find(shape_key);
  if (it == dictionary.end()) {
    const auto shape_id = "document_shape_" + std::to_string(dictionary.size() + 1);
    it = dictionary.emplace(shape_key, shape_id).first;
  }
  ++state->shape_ref_counts[it->second];
  return it->second;
}

std::string ExactPathValueKey(const std::string& path, const std::string& value) {
  return path + "\x1f" + value;
}

std::vector<std::pair<std::string, std::string>> FragmentPairs(
    const std::map<std::string, std::string>& fragments) {
  return std::vector<std::pair<std::string, std::string>>(
      fragments.begin(), fragments.end());
}

void RebuildDocumentIndexes(DocumentProviderState* state) {
  state->shape_dictionary.clear();
  state->shape_ref_counts.clear();
  state->exact_path_value_index.clear();
  state->path_index.clear();
  for (auto& [uuid, record] : state->documents) {
    record.shape_id = InternShape(state, record.fragments);
  }
  for (auto& [uuid, record] : state->documents) {
    record.shape_ref_count = state->shape_ref_counts[record.shape_id];
    for (const auto& [path, value] : record.fragments) {
      state->exact_path_value_index[ExactPathValueKey(path, value)].push_back(uuid);
      state->path_index[path].push_back(uuid);
    }
  }
}

std::vector<std::pair<std::string, std::string>> DocumentRecordPairs(
    const PhysicalDocumentRecord& record) {
  return {
      {"collection_uuid", record.collection_uuid},
      {"document_uuid", record.document_uuid},
      {"row_uuid", record.row_uuid},
      {"name", record.name},
      {"payload", record.payload},
      {"shape_id", record.shape_id},
      {"shape_ref_count", std::to_string(record.shape_ref_count)},
      {"creator_tx", std::to_string(record.creator_tx)},
      {"fragments", EncodeCrudPairs(FragmentPairs(record.fragments))},
      {"null_paths", EncodeCrudPairs([&] {
         std::vector<std::pair<std::string, std::string>> paths;
         for (const auto& path : record.null_paths) paths.push_back({path, "1"});
         return paths;
       }())},
  };
}

PhysicalDocumentRecord DocumentRecordFromPairs(
    const std::vector<std::pair<std::string, std::string>>& pairs) {
  std::map<std::string, std::string> values;
  for (const auto& [key, value] : pairs) {
    values[key] = value;
  }
  PhysicalDocumentRecord record;
  record.collection_uuid = values["collection_uuid"];
  record.document_uuid = values["document_uuid"];
  record.row_uuid = values["row_uuid"];
  record.name = values["name"];
  record.payload = values["payload"];
  record.shape_id = values["shape_id"];
  try {
    record.shape_ref_count =
        static_cast<EngineApiU64>(std::stoull(values["shape_ref_count"]));
  } catch (...) {
    record.shape_ref_count = 0;
  }
  try {
    record.creator_tx = static_cast<EngineApiU64>(std::stoull(values["creator_tx"]));
  } catch (...) {
    record.creator_tx = 0;
  }
  for (const auto& [path, value] : DecodeCrudPairs(values["fragments"])) {
    record.fragments[path] = value;
  }
  for (const auto& [path, marker] : DecodeCrudPairs(values["null_paths"])) {
    if (marker == "1") record.null_paths.insert(path);
  }
  return record;
}

void PersistDocumentProviderEvent(const EngineRequestContext& context,
                                  const std::string& verb,
                                  const PhysicalDocumentRecord& record) {
  const auto path = DocumentProviderPath(context);
  if (path.empty()) { return; }
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out) { return; }
  out << "SBNOSQLDOC1\t" << verb << '\t'
      << EncodeCrudPairs(DocumentRecordPairs(record)) << '\n';
}

void LoadDocumentProviderLocked(const EngineRequestContext& context,
                                DocumentProviderState* state) {
  if (state->loaded) { return; }
  const auto path = DocumentProviderPath(context);
  if (!path.empty()) {
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      if (line.rfind("SBNOSQLDOC1\t", 0) != 0) { continue; }
      const auto first = line.find('\t');
      const auto second = first == std::string::npos
                              ? std::string::npos
                              : line.find('\t', first + 1);
      if (first == std::string::npos || second == std::string::npos) {
        continue;
      }
      const auto verb = line.substr(first + 1, second - first - 1);
      auto record = DocumentRecordFromPairs(
          DecodeCrudPairs(line.substr(second + 1)));
      if (record.collection_uuid.empty()) {
        record.collection_uuid = DocumentCollectionUuid(context);
      }
      if (record.document_uuid.empty() && record.name.empty()) { continue; }
      NormalizeProviderRecordIdentity(&record);
      if (record.document_uuid.empty()) { continue; }
      const auto erase_by_name = [&]() {
        if (record.name.empty()) { return; }
        for (auto it = state->documents.begin(); it != state->documents.end();) {
          if (it->second.collection_uuid == record.collection_uuid &&
              it->second.name == record.name) {
            it = state->documents.erase(it);
          } else {
            ++it;
          }
        }
      };
      if (verb == "DELETE") {
        state->documents.erase(record.document_uuid);
        erase_by_name();
      } else {
        erase_by_name();
        state->documents[record.document_uuid] = record;
      }
    }
  }
  RebuildDocumentIndexes(state);
  for (const auto& generation : ListNoSqlProviderGenerations(context)) {
    if (generation.family == EngineNoSqlProviderFamily::kDocument &&
        generation.collection_uuid == DocumentCollectionUuid(context)) {
      state->provider_generation_id =
          std::max(state->provider_generation_id, generation.generation_id);
    }
  }
  state->loaded = true;
}

std::vector<DocumentPathRowEvidence> ProviderRowsFromState(
    const DocumentProviderState& state,
    const std::string_view collection_uuid) {
  std::vector<DocumentPathRowEvidence> rows;
  for (const auto& [uuid, record] : state.documents) {
    (void)uuid;
    if (record.collection_uuid != collection_uuid ||
        record.fragments.empty()) {
      continue;
    }
    DocumentPathRowEvidence row;
    row.document_uuid = ProviderUuidOrGenerated(record.document_uuid, "object");
    row.row_uuid = ProviderUuidOrGenerated(record.row_uuid, "row");
    row.version_uuid = GenerateCrudEngineUuid("row");
    row.row_ordinal = record.creator_tx;
    for (const auto& [path, value] : record.fragments) {
      EngineTypedValue typed;
      typed.encoded_value = value;
      if (record.null_paths.contains(path)) {
        typed.encoded_value.clear();
        typed.setState(EngineValueState::sql_null);
      }
      row.values.push_back({path, DocumentPathScalarFromTypedValue(typed)});
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

struct DocumentProviderWriteOutcome {
  bool ok = true;
  EngineApiDiagnostic diagnostic;
  std::vector<std::string> evidence;
};

void AddDocumentProviderWriteEvidence(EngineApiResult* result,
                                      const DocumentProviderWriteOutcome& outcome) {
  for (const auto& item : outcome.evidence) {
    AddApiBehaviorEvidence(result, "document_physical_provider", item);
    if (item.rfind("provider_generation_", 0) == 0) {
      AddApiBehaviorEvidence(result, "nosql_provider_generation", item);
    }
  }
}

DocumentProviderWriteOutcome PublishProviderStateLocked(
    const EngineRequestContext& context,
    DocumentProviderState* state) {
  DocumentProviderWriteOutcome outcome;
  const auto rows =
      ProviderRowsFromState(*state, DocumentCollectionUuid(context));
  DocumentPathProviderBuildRequest build;
  build.artifact_path = DocumentPathPhysicalProviderPath(context);
  build.identity =
      DocumentPathProviderIdentityForContext(context,
                                             state->provider_generation_id + 1);
  build.rows = rows;
  auto built = BuildDocumentPathPhysicalProvider(build);
  outcome.evidence.insert(outcome.evidence.end(),
                          built.evidence.begin(),
                          built.evidence.end());
  outcome.evidence.push_back("document_provider_persistent_artifact=true");
  outcome.evidence.push_back("document_provider_exact_path_index_entries=" +
                             std::to_string(built.artifact.stats.posting_count));
  outcome.evidence.push_back("document_provider_path_dictionary_entries=" +
                             std::to_string(built.artifact.stats.path_count));
  if (!built.ok) {
    outcome.ok = false;
    outcome.diagnostic = built.diagnostic;
    return outcome;
  }

  auto metadata = MakeDocumentProviderGenerationMetadata(
      context,
      kDocumentPathPhysicalProviderId,
      DocumentCollectionUuid(context),
      built.artifact.identity.provider_generation);
  const auto published = PublishNoSqlProviderGeneration(context, metadata);
  outcome.evidence.insert(outcome.evidence.end(),
                          published.evidence.begin(),
                          published.evidence.end());
  if (!published.ok) {
    outcome.ok = false;
    outcome.diagnostic = published.diagnostic;
    return outcome;
  }
  state->provider_generation_id = built.artifact.identity.provider_generation;
  return outcome;
}

DocumentProviderWriteOutcome UpsertPhysicalDocument(
    const EngineApiRequest& request,
    const EngineApiResult& result,
    const std::string& bound_collection_uuid = {},
    const std::string& bound_document_uuid = {},
    const std::string& bound_row_uuid = {}) {
  DocumentProviderWriteOutcome outcome;
  PhysicalDocumentRecord record;
  record.collection_uuid = bound_collection_uuid.empty()
                               ? DocumentCollectionUuid(request.context)
                               : bound_collection_uuid;
  record.name = RequestDocumentName(request, RowField(result, "name"));
  record.document_uuid =
      bound_document_uuid.empty()
          ? ProviderUuidOrGenerated(result.primary_object.uuid.canonical,
                                    "object")
          : bound_document_uuid;
  record.row_uuid =
      bound_row_uuid.empty()
          ? ProviderUuidOrGenerated(result.catalog_row_uuid.canonical, "row")
          : bound_row_uuid;
  record.payload = RowField(result, "payload");
  record.fragments =
      ParsePayloadFragments(request, record.payload, &record.null_paths);
  record.creator_tx = request.context.local_transaction_id;
  if (record.document_uuid.empty()) { return outcome; }
  {
    std::lock_guard<std::mutex> guard(DocumentStoresMutex());
    auto& state = DocumentStores()[StoreKey(request.context)];
    LoadDocumentProviderLocked(request.context, &state);
    for (auto it = state.documents.begin(); it != state.documents.end();) {
      if (it->second.collection_uuid == record.collection_uuid &&
          it->second.name == record.name &&
          it->first != record.document_uuid) {
        it = state.documents.erase(it);
      } else {
        ++it;
      }
    }
    state.documents[record.document_uuid] = record;
    RebuildDocumentIndexes(&state);
    record = state.documents[record.document_uuid];
    outcome = PublishProviderStateLocked(request.context, &state);
    if (outcome.ok) { PersistDocumentProviderEvent(request.context, "UPSERT", record); }
    outcome.evidence.push_back("document_provider_concurrency_guard=mutex");
  }
  return outcome;
}

DocumentProviderWriteOutcome DeletePhysicalDocument(
    const EngineApiRequest& request) {
  DocumentProviderWriteOutcome outcome;
  const auto requested = RequestDocumentName(request, request.target_object.uuid.canonical);
  std::optional<PhysicalDocumentRecord> deleted;
  {
    std::lock_guard<std::mutex> guard(DocumentStoresMutex());
    auto& state = DocumentStores()[StoreKey(request.context)];
    LoadDocumentProviderLocked(request.context, &state);
    for (auto it = state.documents.begin(); it != state.documents.end(); ++it) {
      if (it->second.collection_uuid == DocumentCollectionUuid(request.context) &&
          (it->second.document_uuid == request.target_object.uuid.canonical ||
           it->second.name == requested)) {
        deleted = it->second;
        state.documents.erase(it);
        break;
      }
    }
    RebuildDocumentIndexes(&state);
    outcome = PublishProviderStateLocked(request.context, &state);
    if (outcome.ok && deleted.has_value()) {
      PersistDocumentProviderEvent(request.context, "DELETE", *deleted);
    }
    outcome.evidence.push_back("document_provider_concurrency_guard=mutex");
    outcome.evidence.push_back(std::string("document_provider_delete_matched=") +
                               (deleted.has_value() ? "true" : "false"));
  }
  return outcome;
}

template <typename TResult>
TResult DiagnosticResult(const EngineRequestContext& context,
                         const std::string& operation_id,
                         const char* diagnostic_code) {
  auto diagnostic = MakeInvalidRequestDiagnostic(operation_id, diagnostic_code);
  diagnostic.code = diagnostic_code;
  return MakeApiBehaviorDiagnostic<TResult>(
      context, operation_id, std::move(diagnostic));
}

void AddSelectionEvidence(const EngineNoSqlPhysicalProviderSelection& selection,
                          EngineApiResult* result) {
  for (const auto& item : selection.evidence) {
    AddApiBehaviorEvidence(result, "document_physical_provider", item);
  }
}

template <typename TResult>
std::optional<TResult> ValidatePhysicalProof(
    const EngineDocumentFindRequest& request,
    const std::string& operation_id,
    const EngineDocumentPhysicalProof& proof) {
  const auto selection = SelectLocalNoSqlPhysicalProvider(proof.provider_contract);
  if (!proof.proof_supplied) {
    return DiagnosticResult<TResult>(
        request.context,
        operation_id,
        request.wildcard_path ? kDocumentWildcardShapeProofMissing
                              : kDocumentExactPathProofMissing);
  }
  if (!selection.selected) {
    auto failure = MakeApiBehaviorDiagnostic<TResult>(
        request.context,
        operation_id,
        MakeInvalidRequestDiagnostic(operation_id,
                                     selection.missing_diagnostics.empty()
                                         ? selection.refusal_diagnostics.front()
                                         : selection.missing_diagnostics.front()));
    AddSelectionEvidence(selection, &failure);
    return failure;
  }
  const auto provider_generation =
      ValidateNoSqlProviderGeneration(request.context, proof.provider_contract);
  if (!provider_generation.ok) {
    auto failure = MakeApiBehaviorDiagnostic<TResult>(
        request.context, operation_id, provider_generation.diagnostic);
    AddSelectionEvidence(selection, &failure);
    AddNoSqlProviderGenerationEvidence(&failure, provider_generation);
    return failure;
  }
  if (!proof.shape_dictionary_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kDocumentShapeDictionaryProofMissing);
  }
  if (!proof.structural_sharing_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kDocumentStructuralSharingProofMissing);
  }
  if (request.wildcard_path && !proof.wildcard_shape_index_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kDocumentWildcardShapeProofMissing);
  }
  if (!request.wildcard_path && !proof.exact_path_index_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kDocumentExactPathProofMissing);
  }
  if (!request.projected_paths.empty() && !proof.partial_materialization_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kDocumentPartialMaterializationProofMissing);
  }
  if (proof.document_path_index_runtime_proven &&
      proof.provider_contract.provider_generation.required) {
    const auto expected = DocumentPathProviderIdentityForContext(
        request.context,
        proof.provider_contract.provider_generation.required_generation);
    if (proof.provider_contract.index_generation.index_uuid !=
        expected.index_uuid) {
      return DiagnosticResult<TResult>(
          request.context, operation_id, kDocumentPathIndexRuntimeUnproven);
    }
  }
  return std::nullopt;
}

std::vector<std::string> SplitPath(const std::string& path) {
  std::vector<std::string> segments;
  std::stringstream stream(path);
  std::string segment;
  while (std::getline(stream, segment, '.')) {
    segments.push_back(segment);
  }
  return segments;
}

bool WildcardPathMatches(const std::string& pattern, const std::string& path) {
  const auto pattern_segments = SplitPath(pattern);
  const auto path_segments = SplitPath(path);
  if (pattern_segments.size() != path_segments.size()) { return false; }
  for (std::size_t i = 0; i < pattern_segments.size(); ++i) {
    if (pattern_segments[i] == "*") { continue; }
    if (pattern_segments[i] != path_segments[i]) { return false; }
  }
  return true;
}

struct DocumentMatchResult {
  bool valid{true};
  bool matches{false};
  std::string detail;
};

DocumentMatchResult CompareDocumentValue(
    const EngineDocumentFindRequest& request,
    const std::string& encoded_value,
    const bool stored_null) {
  if (!request.comparison_value_present) {
    return {true,
            request.equals_value.empty() || encoded_value == request.equals_value,
            {}};
  }
  EngineTypedValue left;
  left.descriptor = request.comparison_value.descriptor;
  if (stored_null) {
    left.setState(EngineValueState::sql_null);
  } else {
    left.encoded_value = encoded_value;
    left.setState(EngineValueState::value);
  }
  EngineComparisonPredicateOperator operation =
      EngineComparisonPredicateOperator::unspecified;
  if (request.comparison_operator == "=") {
    operation = EngineComparisonPredicateOperator::equal;
  } else if (request.comparison_operator == "<>" ||
             request.comparison_operator == "!=") {
    operation = EngineComparisonPredicateOperator::not_equal;
  } else if (request.comparison_operator == "<") {
    operation = EngineComparisonPredicateOperator::less_than;
  } else if (request.comparison_operator == "<=") {
    operation = EngineComparisonPredicateOperator::less_than_or_equal;
  } else if (request.comparison_operator == ">") {
    operation = EngineComparisonPredicateOperator::greater_than;
  } else if (request.comparison_operator == ">=") {
    operation = EngineComparisonPredicateOperator::greater_than_or_equal;
  }
  int ordering = 0;
  std::string refusal_detail;
  if (!left.isSqlNull() && !request.comparison_value.isSqlNull()) {
    if (scratchbird::core::datatypes::CanonicalTypeIdFromStableName(
            request.comparison_value.descriptor.canonical_type_name) ==
        scratchbird::core::datatypes::CanonicalTypeId::character) {
      EngineCompareScalarValuesRequest compare_request;
      compare_request.context = request.context;
      compare_request.left_value = left;
      compare_request.right_value = request.comparison_value;
      const auto compared = EngineCompareScalarValues(compare_request);
      if (!compared.ok) {
        return {false, false,
                compared.diagnostics.empty()
                    ? "catalog-bound character comparison refused"
                    : compared.diagnostics.front().detail};
      }
      ordering = compared.comparison;
    } else if (!QowCompareCanonicalNonCollatedScalarsV1(
                   left, request.comparison_value, &ordering,
                   &refusal_detail)) {
      return {false, false, refusal_detail};
    }
  }
  EngineSqlTruthValue truth = EngineSqlTruthValue::unknown;
  if (operation == EngineComparisonPredicateOperator::unspecified) {
    return {false, false, "document comparison operator is not bound"};
  }
  if (!QowEvaluateCanonicalComparisonTruthV1(
          left, request.comparison_value, ordering, operation, &truth,
          &refusal_detail)) {
    return {false, false, refusal_detail};
  }
  return {true, truth == EngineSqlTruthValue::true_value, {}};
}

DocumentMatchResult PathMatches(const EngineDocumentFindRequest& request,
                                const PhysicalDocumentRecord& record) {
  if (request.path.empty()) return {true, true, {}};
  for (const auto& [path, value] : record.fragments) {
    const bool path_match = request.wildcard_path
                                ? WildcardPathMatches(request.path, path)
                                : request.path == path;
    if (path_match) {
      auto compared = CompareDocumentValue(
          request, value, record.null_paths.contains(path));
      if (!compared.valid || compared.matches) return compared;
    }
  }
  return {true, false, {}};
}

void AddDocumentPathEvidence(EngineApiResult* result,
                             const EngineNoSqlPhysicalProviderSelection& selection,
                             const EngineDocumentFindRequest& request,
                             bool runtime_proven) {
  const auto access = request.wildcard_path ? "wildcard_shape_index_probe"
                                            : "exact_path_index_probe";
  AddEngineNoSqlSurfaceEvidence(result, "document", access);
  AddSelectionEvidence(selection, result);
  AddApiBehaviorEvidence(result, "document_physical_access", access);
  AddApiBehaviorEvidence(result, "document_shape_fallback",
                         request.wildcard_path ? "shape_dictionary_proved" : "not_required");
  AddApiBehaviorEvidence(result, "document_partial_materialization",
                         request.projected_paths.empty() ? "matched_path_fragment_only"
                                                         : "projected_paths_only");
  AddApiBehaviorEvidence(result, "behavior_store_scan_selected", "false");
  AddApiBehaviorEvidence(result, "descriptor_scan_selected", "false");
  AddApiBehaviorEvidence(result, "row_mga_recheck_evidence", "required");
  AddApiBehaviorEvidence(result, "row_security_recheck_evidence", "required");
  AddApiBehaviorEvidence(result, "mga_finality_authority", "engine_transaction_inventory");
  AddApiBehaviorEvidence(result, "parser_transaction_finality_authority", "false");
  AddApiBehaviorEvidence(result, "client_autocommit_authority", "false");
  AddApiBehaviorEvidence(result, "document_path_provider_candidate_evidence_only", "true");
  AddApiBehaviorEvidence(result, "reference_finality_authority", "false");
  AddApiBehaviorEvidence(result, "provider_finality_authority", "false");
  AddApiBehaviorEvidence(result, "write_ahead_log_finality_authority", "false");  // wal-not-authority
  AddApiBehaviorEvidence(result,
                         "document_provider_index_consumed",
                         runtime_proven ? "true" : "false");
  AddApiBehaviorEvidence(result,
                         "document_index_runtime_correctness",
                         runtime_proven ? "proven" : "pending_provider_probe");
  AddApiBehaviorEvidence(
      result,
      "benchmark_clean_index_runtime_closure",
      runtime_proven ? "true" : "false");
}

const idx::IndexRouteCapabilityState* DocumentPathRouteCapability() {
  return idx::FindBuiltinIndexRouteCapabilityState(
      idx::IndexRouteKind::nosql_document,
      idx::IndexFamily::document_path);
}

bool DocumentPathRouteCapabilityAdmitted(
    const idx::IndexRouteCapabilityState* state) {
  return state != nullptr && state->route_complete() &&
         state->supports_read && state->supports_equality_lookup &&
         state->produces_candidate_set && state->requires_exact_recheck &&
         state->requires_mga_recheck && state->requires_security_recheck &&
         !state->supports_ordered_range && !state->supports_negative_prune;
}

void AddDocumentPathRouteCapabilityEvidence(
    EngineApiResult* result,
    const idx::IndexRouteCapabilityState* state) {
  AddApiBehaviorEvidence(result,
                         "document_index_route_kind",
                         idx::IndexRouteKindName(idx::IndexRouteKind::nosql_document));
  AddApiBehaviorEvidence(result, "document_index_family", "document_path");
  if (state == nullptr) {
    AddApiBehaviorEvidence(result, "document_index_route_capability", "missing");
    return;
  }
  AddApiBehaviorEvidence(result,
                         "document_index_route_capability",
                         DocumentPathRouteCapabilityAdmitted(state)
                             ? "complete"
                             : "refused");
  AddApiBehaviorEvidence(result,
                         "document_index_route_benchmark_clean",
                         state->benchmark_clean ? "true" : "false");
  AddApiBehaviorEvidence(result,
                         "document_index_route_supports_read",
                         state->supports_read ? "true" : "false");
  AddApiBehaviorEvidence(result,
                         "document_index_route_supports_equality_lookup",
                         state->supports_equality_lookup ? "true" : "false");
  AddApiBehaviorEvidence(result,
                         "document_index_route_supports_ordered_range",
                         state->supports_ordered_range ? "true" : "false");
  AddApiBehaviorEvidence(result,
                         "document_index_route_requires_exact_recheck",
                         state->requires_exact_recheck ? "true" : "false");
  AddApiBehaviorEvidence(result,
                         "document_index_route_requires_mga_recheck",
                         state->requires_mga_recheck ? "true" : "false");
  AddApiBehaviorEvidence(result,
                         "document_index_route_requires_security_recheck",
                         state->requires_security_recheck ? "true" : "false");
}

EngineApiDiagnostic DocumentPathRouteCapabilityDiagnostic(
    const idx::IndexRouteCapabilityState* state) {
  if (state == nullptr) {
    return MakeInvalidRequestDiagnostic(
        "nosql.document_find",
        "INDEX.ROUTE_CAPABILITY.MISSING;route=nosql_document;family=document_path");
  }
  return MakeEngineApiDiagnostic(
      state->route_diagnostic_code,
      state->route_message_key,
      state->route_detail,
      true);
}

EngineDescriptor DocumentProjectionDescriptor(
    const EngineDocumentFindRequest& request,
    const std::size_t ordinal) {
  if (ordinal < request.descriptors.size()) return request.descriptors[ordinal];
  if (!request.comparison_value.descriptor.descriptor_uuid.canonical.empty()) {
    return request.comparison_value.descriptor;
  }
  EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7200-8000-00000000d073";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "text";
  descriptor.encoded_descriptor = "type=text;nullable=true";
  return descriptor;
}

bool AddProjectedDocumentRow(EngineDocumentFindResult* result,
                             const DocumentPathProviderCandidate& candidate,
                             const EngineDocumentFindRequest& request,
                             std::size_t* retained_cells,
                             std::uint64_t* retained_memory_bytes,
                             bool* resource_refused) {
  if (result == nullptr || retained_cells == nullptr ||
      retained_memory_bytes == nullptr || resource_refused == nullptr) {
    return false;
  }
  *resource_refused = false;
  std::vector<std::pair<std::string, std::string>> fields = {
      {"surface", "document"},
      {"document_uuid", candidate.document_uuid},
      {"row_uuid", candidate.row_uuid},
      {"version_uuid", candidate.version_uuid},
      {"row_mga_recheck_required", "true"},
      {"row_security_recheck_required", "true"},
  };

  for (const auto& value : candidate.projected_values) {
    fields.push_back({"path:" + value.path, value.value.encoded_value});
  }

  EngineDocumentTypedRow typed_row;
  typed_row.document_uuid = candidate.document_uuid;
  typed_row.row_uuid = candidate.row_uuid;
  const auto append_value = [&](const std::string& path,
                                const DocumentPathScalar* scalar,
                                const std::size_t ordinal) {
    EngineTypedValue value;
    value.descriptor = DocumentProjectionDescriptor(request, ordinal);
    if (scalar == nullptr) {
      const bool nullable =
          ordinal < request.projected_path_nullable.size()
              ? request.projected_path_nullable[ordinal]
              : request.explicit_nullable_missing_projection;
      if (!nullable) return false;
      // Preserve absence through the provider boundary.  The canonical model
      // exchange (or expression projection immediately above it) owns the
      // missing-to-SQL-NULL conversion for a nullable bound descriptor.
      value.setState(EngineValueState::missing);
    } else if (scalar->is_null) {
      value.setState(EngineValueState::sql_null);
    } else {
      value.encoded_value = scalar->encoded_value;
      value.setState(EngineValueState::value);
    }
    typed_row.values.push_back({path, std::move(value)});
    return true;
  };
  if (!request.projected_paths.empty()) {
    for (std::size_t ordinal = 0; ordinal < request.projected_paths.size();
         ++ordinal) {
      const auto& path = request.projected_paths[ordinal];
      const auto found = std::ranges::find_if(
          candidate.projected_values,
          [&](const auto& value) { return value.path == path; });
      const auto* scalar = found == candidate.projected_values.end()
                               ? nullptr
                               : &found->value;
      if (!append_value(path, scalar, ordinal)) return false;
    }
  } else {
    for (std::size_t ordinal = 0;
         ordinal < candidate.projected_values.size(); ++ordinal) {
      const auto& projected = candidate.projected_values[ordinal];
      if (!append_value(projected.path, &projected.value, ordinal)) return false;
    }
  }
  if (typed_row.values.size() >
          std::numeric_limits<std::size_t>::max() - *retained_cells ||
      *retained_cells + typed_row.values.size() > request.maximum_cells) {
    *resource_refused = true;
    return false;
  }
  std::uint64_t row_bytes = sizeof(EngineDocumentTypedRow);
  const auto add_bytes = [&](const std::size_t amount) {
    if (amount > std::numeric_limits<std::uint64_t>::max() - row_bytes) {
      return false;
    }
    row_bytes += static_cast<std::uint64_t>(amount);
    return true;
  };
  if (!add_bytes(typed_row.document_uuid.size()) ||
      !add_bytes(typed_row.row_uuid.size())) {
    *resource_refused = true;
    return false;
  }
  for (const auto& path_value : typed_row.values) {
    const auto& value = path_value.value;
    if (!add_bytes(sizeof(EngineDocumentTypedPathValue)) ||
        !add_bytes(path_value.path.size()) ||
        !add_bytes(value.descriptor.descriptor_uuid.canonical.size()) ||
        !add_bytes(value.descriptor.descriptor_kind.size()) ||
        !add_bytes(value.descriptor.canonical_type_name.size()) ||
        !add_bytes(value.descriptor.encoded_descriptor.size()) ||
        !add_bytes(value.encoded_value.size()) ||
        !add_bytes(value.binary_value.size())) {
      *resource_refused = true;
      return false;
    }
  }
  if (row_bytes >
          std::numeric_limits<std::uint64_t>::max() -
              *retained_memory_bytes ||
      *retained_memory_bytes + row_bytes > request.maximum_memory_bytes) {
    *resource_refused = true;
    return false;
  }
  *retained_cells += typed_row.values.size();
  *retained_memory_bytes += row_bytes;
  result->typed_rows.push_back(std::move(typed_row));
  AddApiBehaviorRow(result, std::move(fields));
  if (!candidate.shape_id.empty()) {
    AddApiBehaviorEvidence(result, "document_shape_dictionary", candidate.shape_id);
    AddApiBehaviorEvidence(result,
                           "document_structural_sharing",
                           "shape_ref_count=" +
                               std::to_string(candidate.shape_ref_count));
  }
  AddApiBehaviorEvidence(result,
                         "document_structural_sharing",
                         "shape_dictionary_membership_reopened");
  return true;
}

std::vector<DocumentPathProviderProjectedValue> ProjectValuesFromSource(
    const PhysicalDocumentRecord& record,
    const EngineDocumentFindRequest& request) {
  std::vector<DocumentPathProviderProjectedValue> projected;
  std::set<std::string> wanted(request.projected_paths.begin(),
                               request.projected_paths.end());
  for (const auto& [path, value] : record.fragments) {
    const bool requested = !wanted.empty() && wanted.count(path) != 0;
    const bool matched_probe =
        wanted.empty() &&
        ((request.wildcard_path && WildcardPathMatches(request.path, path)) ||
         (!request.wildcard_path && request.path == path));
    if (!requested && !matched_probe) { continue; }
    DocumentPathProviderProjectedValue projected_value;
    projected_value.path = path;
    projected_value.value.scalar_type = "string";
    projected_value.value.encoded_value = value;
    projected_value.value.is_null = record.null_paths.contains(path);
    projected.push_back(std::move(projected_value));
  }
  return projected;
}

bool SourceRecordVisibleToRequest(const EngineRequestContext& context,
                                  const PhysicalDocumentRecord& record) {
  if (!context.security_context_present) { return false; }
  for (const auto& visible :
       VisibleApiBehaviorRecords(context, "document", context.local_transaction_id)) {
    if (visible.object_uuid == record.document_uuid ||
        visible.object_uuid == record.name ||
        visible.default_name == record.document_uuid ||
        visible.default_name == record.name) {
      return true;
    }
  }
  return false;
}

bool RecheckDocumentPathCandidate(
    const EngineRequestContext& context,
    const EngineDocumentFindRequest& request,
    const DocumentPathProviderCandidate& provider_candidate,
    DocumentPathProviderCandidate* rechecked_candidate,
    std::string* refusal_detail) {
  std::lock_guard<std::mutex> guard(DocumentStoresMutex());
  auto& state = DocumentStores()[StoreKey(context)];
  LoadDocumentProviderLocked(context, &state);
  const auto record = state.documents.find(provider_candidate.document_uuid);
  if (record == state.documents.end()) { return false; }
  const auto requested_collection =
      request.target_object.uuid.canonical.empty()
          ? DocumentCollectionUuid(context)
          : request.target_object.uuid.canonical;
  if (record->second.collection_uuid != requested_collection) { return false; }
  if (record->second.row_uuid != provider_candidate.row_uuid) { return false; }
  const auto matched = PathMatches(request, record->second);
  if (!matched.valid) {
    *refusal_detail = matched.detail;
    return false;
  }
  if (!matched.matches) return false;
  if (!SourceRecordVisibleToRequest(context, record->second)) { return false; }

  *rechecked_candidate = provider_candidate;
  rechecked_candidate->projected_values =
      ProjectValuesFromSource(record->second, request);
  return true;
}

EngineDocumentFindResult ExactCollectionDocumentFind(
    const EngineDocumentFindRequest& request,
    const std::string& operation_id) {
  if (!request.exact_collection_fallback) {
    return DiagnosticResult<EngineDocumentFindResult>(
        request.context, operation_id,
        kModelDocumentExactFallbackUnavailable);
  }
  const bool comparison_supported =
      request.comparison_operator == "=" ||
      request.comparison_operator == "<>" ||
      request.comparison_operator == "!=" ||
      request.comparison_operator == "<" ||
      request.comparison_operator == "<=" ||
      request.comparison_operator == ">" ||
      request.comparison_operator == ">=";
  if (!comparison_supported || request.maximum_rows == 0 ||
      request.maximum_cells == 0 || request.maximum_memory_bytes == 0) {
    return DiagnosticResult<EngineDocumentFindResult>(
        request.context, operation_id,
        "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1");
  }

  auto result =
      MakeApiBehaviorSuccess<EngineDocumentFindResult>(request.context,
                                                       operation_id);
  std::lock_guard<std::mutex> guard(DocumentStoresMutex());
  auto& state = DocumentStores()[StoreKey(request.context)];
  LoadDocumentProviderLocked(request.context, &state);
  const auto requested_collection =
      request.target_object.uuid.canonical.empty()
          ? DocumentCollectionUuid(request.context)
          : request.target_object.uuid.canonical;
  std::size_t collection_rows_scanned = 0;
  std::size_t retained_cells = 0;
  std::uint64_t retained_memory_bytes = 0;
  const auto preflight_row = [&](const PhysicalDocumentRecord& record,
                                 std::size_t* cells,
                                 std::uint64_t* retained_row_bytes,
                                 std::uint64_t* transient_bytes) {
    if (cells == nullptr || retained_row_bytes == nullptr ||
        transient_bytes == nullptr) {
      return false;
    }
    *cells = 0;
    *retained_row_bytes = sizeof(EngineDocumentTypedRow);
    *transient_bytes = sizeof(DocumentPathProviderCandidate);
    const auto checked_add = [](std::uint64_t* total,
                                const std::size_t amount) {
      if (total == nullptr ||
          amount > std::numeric_limits<std::uint64_t>::max() - *total) {
        return false;
      }
      *total += static_cast<std::uint64_t>(amount);
      return true;
    };
    if (!checked_add(retained_row_bytes, record.document_uuid.size()) ||
        !checked_add(retained_row_bytes, record.row_uuid.size()) ||
        !checked_add(transient_bytes, record.document_uuid.size()) ||
        !checked_add(transient_bytes, record.row_uuid.size()) ||
        !checked_add(transient_bytes, record.shape_id.size()) ||
        !checked_add(transient_bytes, 36) ||
        !checked_add(transient_bytes,
                     sizeof(std::pair<std::string, std::string>) * 6)) {
      return false;
    }
    const auto account_path = [&](const std::string& path,
                                  const std::string* encoded_value,
                                  const std::size_t ordinal) {
      const auto descriptor = DocumentProjectionDescriptor(request, ordinal);
      const auto value_size =
          encoded_value == nullptr ? 0 : encoded_value->size();
      if (*cells == std::numeric_limits<std::size_t>::max()) return false;
      ++*cells;
      return checked_add(retained_row_bytes,
                         sizeof(EngineDocumentTypedPathValue)) &&
             checked_add(retained_row_bytes, path.size()) &&
             checked_add(retained_row_bytes,
                         descriptor.descriptor_uuid.canonical.size()) &&
             checked_add(retained_row_bytes,
                         descriptor.descriptor_kind.size()) &&
             checked_add(retained_row_bytes,
                         descriptor.canonical_type_name.size()) &&
             checked_add(retained_row_bytes,
                         descriptor.encoded_descriptor.size()) &&
             checked_add(retained_row_bytes, value_size) &&
             checked_add(transient_bytes,
                         sizeof(DocumentPathProviderProjectedValue)) &&
             checked_add(transient_bytes, path.size()) &&
             checked_add(transient_bytes, sizeof("string") - 1) &&
             checked_add(transient_bytes, value_size) &&
             checked_add(transient_bytes,
                         sizeof(std::pair<std::string, std::string>)) &&
             checked_add(transient_bytes,
                         (sizeof("path:") - 1) + path.size()) &&
             checked_add(transient_bytes, value_size);
    };
    if (!request.projected_paths.empty()) {
      for (std::size_t ordinal = 0; ordinal < request.projected_paths.size();
           ++ordinal) {
        const auto& path = request.projected_paths[ordinal];
        const auto found = record.fragments.find(path);
        const auto* value =
            found == record.fragments.end() ? nullptr : &found->second;
        if (!account_path(path, value, ordinal)) return false;
      }
    } else {
      std::size_t ordinal = 0;
      for (const auto& [path, value] : record.fragments) {
        const bool matched =
            (request.wildcard_path &&
             WildcardPathMatches(request.path, path)) ||
            (!request.wildcard_path && request.path == path);
        if (!matched) continue;
        if (!account_path(path, &value, ordinal++)) return false;
      }
    }
    return true;
  };
  for (const auto& [uuid, record] : state.documents) {
    (void)uuid;
    if (record.collection_uuid != requested_collection) { continue; }
    ++collection_rows_scanned;
    try {
      if (request.context.query_cancellation_requested &&
          request.context.query_cancellation_requested()) {
        return DiagnosticResult<EngineDocumentFindResult>(
            request.context, operation_id,
            "SB_MODEL_EXECUTION_CANCELLED_V1");
      }
    } catch (...) {
      return DiagnosticResult<EngineDocumentFindResult>(
          request.context, operation_id,
          "SB_MODEL_COORDINATOR_LEG_FAILED_V1");
    }
    const auto matched = PathMatches(request, record);
    if (!matched.valid) {
      auto failure = DiagnosticResult<EngineDocumentFindResult>(
          request.context, operation_id,
          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1");
      AddApiBehaviorEvidence(&failure, "document_comparison_refusal",
                             matched.detail);
      return failure;
    }
    if (!matched.matches ||
        !SourceRecordVisibleToRequest(request.context, record)) {
      continue;
    }
    if (result.typed_rows.size() >= request.maximum_rows) {
      return DiagnosticResult<EngineDocumentFindResult>(
          request.context, operation_id,
          "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1");
    }
    std::size_t row_cells = 0;
    std::uint64_t retained_row_bytes = 0;
    std::uint64_t transient_bytes = 0;
    std::uint64_t peak_bytes = 0;
    if (!preflight_row(record, &row_cells, &retained_row_bytes,
                       &transient_bytes) ||
        row_cells > std::numeric_limits<std::size_t>::max() -
                        retained_cells ||
        retained_cells + row_cells > request.maximum_cells ||
        retained_row_bytes >
            std::numeric_limits<std::uint64_t>::max() -
                retained_memory_bytes ||
        !([&] {
          peak_bytes = retained_memory_bytes + retained_row_bytes;
          return transient_bytes <=
                 std::numeric_limits<std::uint64_t>::max() - peak_bytes;
        }()) ||
        (peak_bytes += transient_bytes,
         peak_bytes > request.maximum_memory_bytes)) {
      return DiagnosticResult<EngineDocumentFindResult>(
          request.context, operation_id,
          "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1");
    }
    DocumentPathProviderCandidate candidate;
    candidate.document_uuid = record.document_uuid;
    candidate.row_uuid = record.row_uuid;
    candidate.version_uuid = GenerateCrudEngineUuid("row");
    candidate.row_ordinal = record.creator_tx;
    candidate.shape_id = record.shape_id;
    candidate.shape_ref_count = record.shape_ref_count;
    candidate.projected_values = ProjectValuesFromSource(record, request);
    bool resource_refused = false;
    if (!AddProjectedDocumentRow(&result, candidate, request,
                                 &retained_cells,
                                 &retained_memory_bytes,
                                 &resource_refused)) {
      return DiagnosticResult<EngineDocumentFindResult>(
          request.context, operation_id,
          resource_refused ? "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1"
                           : kModelDocumentMissingBindingRefusedApi);
    }
  }
  result.exact_collection_fallback_used = true;
  result.residual_recheck_complete = true;
  result.base_row_mga_recheck_complete = true;
  result.security_recheck_complete = true;
  result.dml_summary.visible_rows_scanned = collection_rows_scanned;
  result.dml_summary.benchmark_clean = true;
  AddApiBehaviorEvidence(&result, "document_exact_fallback",
                         "DOCUMENT_COLLECTION_SCAN_EXACT_V1");
  AddApiBehaviorEvidence(&result, "document_exact_source_recheck",
                         "mga_visibility_security_and_value_passed");
  AddApiBehaviorEvidence(&result, "descriptor_scan_selected", "false");
  AddApiBehaviorEvidence(&result, "behavior_store_scan_selected", "false");
  AddApiBehaviorEvidence(&result, "mga_finality_authority",
                         "engine_transaction_inventory");
  return result;
}

EngineDocumentFindResult PhysicalDocumentFind(
    const EngineDocumentFindRequest& request,
    const std::string& operation_id,
    const EngineDocumentPhysicalProof& proof) {
  if (auto failure = ValidatePhysicalProof<EngineDocumentFindResult>(
          request, operation_id, proof)) {
    return *failure;
  }
  const auto selection = SelectLocalNoSqlPhysicalProvider(proof.provider_contract);
  const auto provider_generation =
      ValidateNoSqlProviderGeneration(request.context, proof.provider_contract);
  if (!provider_generation.ok) {
    auto failure = MakeApiBehaviorDiagnostic<EngineDocumentFindResult>(
        request.context, operation_id, provider_generation.diagnostic);
    AddDocumentPathEvidence(&failure, selection, request, false);
    AddNoSqlProviderGenerationEvidence(&failure, provider_generation);
    return failure;
  }
  const auto* route_capability = DocumentPathRouteCapability();
  if (!DocumentPathRouteCapabilityAdmitted(route_capability)) {
    auto failure = MakeApiBehaviorDiagnostic<EngineDocumentFindResult>(
        request.context,
        operation_id,
        DocumentPathRouteCapabilityDiagnostic(route_capability));
    AddDocumentPathEvidence(&failure, selection, request, false);
    AddNoSqlProviderGenerationEvidence(&failure, provider_generation);
    AddDocumentPathRouteCapabilityEvidence(&failure, route_capability);
    return failure;
  }
  auto result = MakeApiBehaviorSuccess<EngineDocumentFindResult>(
      request.context, operation_id);
  AddNoSqlProviderGenerationEvidence(&result, provider_generation);
  AddDocumentPathRouteCapabilityEvidence(&result, route_capability);

  DocumentPathProviderProbeRequest probe_request;
  probe_request.artifact_path = DocumentPathPhysicalProviderPath(request.context);
  probe_request.expected_identity = DocumentPathProviderIdentityForContext(
      request.context,
      proof.provider_contract.provider_generation.required_generation,
      proof.provider_contract.index_generation.index_uuid);
  probe_request.require_expected_identity =
      proof.provider_contract.provider_generation.required;
  probe_request.path = request.path;
  probe_request.equals_value.encoded_value = request.equals_value;
  probe_request.equals_value.scalar_type = "string";
  probe_request.wildcard_path = request.wildcard_path;
  probe_request.projected_paths = request.projected_paths;
  const auto probe = ProbeDocumentPathPhysicalProvider(probe_request);
  if (!probe.ok) {
    auto failure = MakeApiBehaviorDiagnostic<EngineDocumentFindResult>(
        request.context,
        operation_id,
        probe.diagnostic);
    AddDocumentPathEvidence(&failure, selection, request, false);
    for (const auto& item : probe.evidence) {
      AddApiBehaviorEvidence(&failure, "document_path_physical_provider", item);
    }
    return failure;
  }
  AddDocumentPathEvidence(&result, selection, request, true);
  for (const auto& item : probe.evidence) {
    AddApiBehaviorEvidence(&result, "document_path_physical_provider", item);
  }

  std::vector<DocumentPathProviderCandidate> rechecked_candidates;
  for (const auto& candidate : probe.projection_plan.candidates) {
    DocumentPathProviderCandidate rechecked;
    std::string refusal_detail;
    if (RecheckDocumentPathCandidate(
            request.context, request, candidate, &rechecked,
            &refusal_detail)) {
      rechecked_candidates.push_back(std::move(rechecked));
    } else if (!refusal_detail.empty()) {
      auto failure = DiagnosticResult<EngineDocumentFindResult>(
          request.context, operation_id,
          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1");
      AddApiBehaviorEvidence(&failure, "document_comparison_refusal",
                             refusal_detail);
      return failure;
    }
  }
  AddApiBehaviorEvidence(&result,
                         "document_source_candidates_rechecked",
                         std::to_string(probe.projection_plan.candidates.size()));
  AddApiBehaviorEvidence(&result,
                         "document_source_candidates_visible",
                         std::to_string(rechecked_candidates.size()));
  AddApiBehaviorEvidence(&result,
                         "document_exact_source_recheck",
                         "mga_visibility_security_and_value_passed");

  std::vector<EngineNoSqlBatchPointLookupItem> lookup_items;
  for (const auto& candidate : rechecked_candidates) {
    lookup_items.push_back(
        {candidate.document_uuid,
         candidate.row_uuid,
         0.0,
         probe.projection_plan.fetch_full_payload ? "full_payload_requested" : "",
         {{"surface", "document"}, {"projection", "document_path_provider"}}});
  }
  if (auto failure = AddEngineNoSqlOrderedBatchLookupEvidence<
          EngineDocumentFindResult>(
          request.context,
          operation_id,
          "document",
          scratchbird::core::index::BatchPointLookupPurpose::document_payload,
          selection,
          lookup_items,
          &result)) {
    return *failure;
  }
  std::size_t retained_cells = 0;
  std::uint64_t retained_memory_bytes = 0;
  for (const auto& candidate : rechecked_candidates) {
    bool resource_refused = false;
    if (!AddProjectedDocumentRow(&result, candidate, request,
                                 &retained_cells,
                                 &retained_memory_bytes,
                                 &resource_refused)) {
      auto failure = MakeApiBehaviorDiagnostic<EngineDocumentFindResult>(
          request.context, operation_id,
          MakeInvalidRequestDiagnostic(
              operation_id,
              resource_refused ? "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1"
                               : kModelDocumentMissingBindingRefusedApi));
      AddApiBehaviorEvidence(&failure, "document_missing_path", "refused");
      return failure;
    }
  }
  result.residual_recheck_complete = true;
  result.base_row_mga_recheck_complete = true;
  result.security_recheck_complete = true;
  result.dml_summary.index_probes = probe.index_probes;
  result.dml_summary.visible_rows_scanned = 0;
  result.dml_summary.benchmark_clean = true;
  AddApiBehaviorEvidence(&result,
                         "document_provider_path_index_keys_examined",
                         std::to_string(probe.path_keys_examined));
  AddApiBehaviorEvidence(&result,
                         "document_rows_returned",
                         std::to_string(result.result_shape.rows.size()));
  return result;
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_NOSQL_DOCUMENT_API_BEHAVIOR
EngineDocumentInsertResult EngineDocumentInsert(const EngineDocumentInsertRequest& request) {
  constexpr const char* kOperation = "nosql.document_insert";
  if (!request.context.cluster_authority_available && EngineNoSqlRequiresClusterAuthority(request)) {
    return EngineNoSqlClusterAuthorityUnavailable<EngineDocumentInsertResult>(request, kOperation);
  }
  const auto canonical_uuid = [](const std::string& value) {
    const auto parsed = scratchbird::core::uuid::ParseUuid(value);
    return parsed.ok() && !scratchbird::core::uuid::IsNilUuid(parsed.value) &&
           scratchbird::core::uuid::UuidToString(parsed.value) == value;
  };
  if ((!request.collection_uuid.empty() &&
       !canonical_uuid(request.collection_uuid)) ||
      (!request.document_uuid.empty() &&
       !canonical_uuid(request.document_uuid)) ||
      (!request.row_uuid.empty() && !canonical_uuid(request.row_uuid))) {
    return DiagnosticResult<EngineDocumentInsertResult>(
        request.context, kOperation,
        "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1");
  }
  if (!request.collection_uuid.empty()) {
    const auto collection = LoadMgaRelationStorageDescriptor(
        request.context, request.collection_uuid);
    if (!collection.ok ||
        collection.descriptor.relation_uuid.canonical !=
            request.collection_uuid ||
        collection.descriptor.database_uuid.canonical !=
            request.context.database_uuid.canonical ||
        collection.descriptor.relation_kind != "table" ||
        collection.descriptor.storage_profile != "local_mga_rowstore_v1" ||
        collection.descriptor.descriptor_generation == 0) {
      return DiagnosticResult<EngineDocumentInsertResult>(
          request.context, kOperation,
          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1");
    }
    const auto authorization = EvaluateMaterializedAuthorization(
        request.context, request.context.authorization_context, "INSERT",
        request.collection_uuid);
    if (!authorization.authorized || authorization.denied ||
        authorization.policy_recheck_required ||
        !authorization.diagnostics.empty()) {
      return DiagnosticResult<EngineDocumentInsertResult>(
          request.context, kOperation,
          "SB_MODEL_SECURITY_ADMISSION_REFUSED_V1");
    }
  }
  auto result = EngineNoSqlPayloadAwarePersistedWriteResult<EngineDocumentInsertResult>(
      request,
      kOperation,
      "document",
      true,
      "active");
  if (result.ok) {
    const auto provider_write =
        UpsertPhysicalDocument(request, result, request.collection_uuid,
                               request.document_uuid, request.row_uuid);
    AddDocumentProviderWriteEvidence(&result, provider_write);
    if (!provider_write.ok) {
      result.ok = false;
      result.diagnostics.push_back(provider_write.diagnostic);
    }
    AddEngineNoSqlSurfaceEvidence(&result, "document", "persisted_document_insert");
    AddApiBehaviorEvidence(&result, "document_physical_provider", "write_through_path_provider");
    AddApiBehaviorEvidence(&result, "mga_finality_authority", "engine_transaction_inventory");
  }
  return result;
}

EngineDocumentFindResult EngineDocumentFind(const EngineDocumentFindRequest& request) {
  constexpr const char* kOperation = "nosql.document_find";
  if (!request.context.cluster_authority_available && EngineNoSqlRequiresClusterAuthority(request)) {
    return EngineNoSqlClusterAuthorityUnavailable<EngineDocumentFindResult>(request, kOperation);
  }
  if (request.comparison_value_present &&
      (!QowCanonicalDescriptorIdentityV1(
           request.comparison_value.descriptor) ||
       request.comparison_value.descriptor.descriptor_kind != "scalar" ||
       (request.comparison_value.state != EngineValueState::value &&
        request.comparison_value.state != EngineValueState::sql_null))) {
    return DiagnosticResult<EngineDocumentFindResult>(
        request.context, kOperation,
        "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1");
  }
  if (request.exact_collection_fallback) {
    const auto exact_nullability = [](const std::string& encoded)
        -> std::optional<bool> {
      std::optional<bool> result;
      bool canonical_seen = false;
      bool storage_seen = false;
      std::size_t offset = 0;
      while (offset <= encoded.size()) {
        const auto end = encoded.find(';', offset);
        const auto field = encoded.substr(
            offset, end == std::string::npos ? std::string::npos
                                              : end - offset);
        if (field.starts_with("nullability=")) {
          if (canonical_seen) return std::nullopt;
          canonical_seen = true;
          const auto value = field.substr(12);
          std::optional<bool> parsed;
          if (value == "nullable") {
            parsed = true;
          } else if (value == "non_null") {
            parsed = false;
          } else {
            return std::nullopt;
          }
          if (result.has_value() && result != parsed) return std::nullopt;
          result = parsed;
        } else if (field.starts_with("nullable=")) {
          if (storage_seen) return std::nullopt;
          storage_seen = true;
          const auto value = field.substr(9);
          std::optional<bool> parsed;
          if (value == "true") {
            parsed = true;
          } else if (value == "false") {
            parsed = false;
          } else {
            return std::nullopt;
          }
          if (result.has_value() && result != parsed) return std::nullopt;
          result = parsed;
        }
        if (end == std::string::npos) break;
        offset = end + 1;
      }
      return result;
    };
    if (request.projected_paths.empty() ||
        request.projected_paths.size() !=
            request.projected_path_nullable.size() ||
        request.projected_paths.size() != request.descriptors.size()) {
      return DiagnosticResult<EngineDocumentFindResult>(
          request.context, kOperation, kModelDocumentMissingBindingRefusedApi);
    }
    for (std::size_t ordinal = 0; ordinal < request.descriptors.size();
         ++ordinal) {
      const auto& descriptor = request.descriptors[ordinal];
      const auto nullable =
          exact_nullability(descriptor.encoded_descriptor);
      if (request.projected_paths[ordinal].empty() ||
          !QowCanonicalDescriptorIdentityV1(descriptor) ||
          (descriptor.descriptor_kind != "scalar" &&
           descriptor.descriptor_kind != "canonical_type_descriptor") ||
          !nullable.has_value() ||
          *nullable != request.projected_path_nullable[ordinal]) {
        return DiagnosticResult<EngineDocumentFindResult>(
            request.context, kOperation,
            "SB_MODEL_TYPED_EXCHANGE_INVALID_V1");
      }
    }
    return ExactCollectionDocumentFind(request, kOperation);
  }
  if (request.require_benchmark_clean_index_runtime &&
      !request.physical_proof.provider_contract.provider_generation.proof_present) {
    auto result = MakeApiBehaviorDiagnostic<EngineDocumentFindResult>(
        request.context,
        kOperation,
        MakeInvalidRequestDiagnostic(kOperation,
                                     kNoSqlProviderGenerationProofMissing));
    AddApiBehaviorEvidence(&result, "descriptor_scan_selected", "false");
    AddApiBehaviorEvidence(&result, "behavior_store_scan_selected", "false");
    AddApiBehaviorEvidence(&result,
                           "document_provider_generation_required",
                           "true");
    AddApiBehaviorEvidence(&result,
                           "document_provider_fail_closed_before_fallback",
                           "true");
    return result;
  }
  if (request.require_benchmark_clean_index_runtime && request.path.empty() &&
      !request.wildcard_path && request.projected_paths.empty()) {
    const auto provider_generation = ValidateNoSqlProviderGeneration(
        request.context, request.physical_proof.provider_contract);
    if (!provider_generation.ok) {
      auto result = MakeApiBehaviorDiagnostic<EngineDocumentFindResult>(
          request.context, kOperation, provider_generation.diagnostic);
      AddNoSqlProviderGenerationEvidence(&result, provider_generation);
      AddApiBehaviorEvidence(&result, "descriptor_scan_selected", "false");
      AddApiBehaviorEvidence(&result, "behavior_store_scan_selected", "false");
      AddApiBehaviorEvidence(&result,
                             "document_provider_fail_closed_before_fallback",
                             "true");
      return result;
    }
    auto result = MakeApiBehaviorDiagnostic<EngineDocumentFindResult>(
        request.context,
        kOperation,
        MakeInvalidRequestDiagnostic(kOperation, kDocumentPathIndexUnavailable));
    AddApiBehaviorEvidence(&result, "descriptor_scan_selected", "false");
    AddApiBehaviorEvidence(&result, "behavior_store_scan_selected", "false");
    AddApiBehaviorEvidence(&result,
                           "document_provider_fail_closed_before_fallback",
                           "true");
    return result;
  }
  if (!request.path.empty() || request.wildcard_path ||
      !request.projected_paths.empty()) {
    return PhysicalDocumentFind(request, kOperation, request.physical_proof);
  }
  auto result = MakeApiBehaviorSuccess<EngineDocumentFindResult>(request.context, kOperation);
  for (const auto& record : VisibleApiBehaviorRecords(request.context, "document", request.context.local_transaction_id)) {
    AddApiBehaviorRow(&result, {{"surface", "document"},
                                {"document_uuid", record.object_uuid},
                                {"name", record.default_name},
                                {"state", record.state},
                                {"payload", record.payload}});
  }
  AddApiBehaviorEvidence(&result, "document_find", std::to_string(result.result_shape.rows.size()));
  AddEngineNoSqlSurfaceEvidence(&result, "document", "local_descriptor_scan");
  return result;
}

EngineDocumentUpdateResult EngineDocumentUpdate(const EngineDocumentUpdateRequest& request) {
  constexpr const char* kOperation = "nosql.document_update";
  if (!request.context.cluster_authority_available && EngineNoSqlRequiresClusterAuthority(request)) {
    return EngineNoSqlClusterAuthorityUnavailable<EngineDocumentUpdateResult>(request, kOperation);
  }
  auto result = EngineNoSqlPayloadAwarePersistedWriteResult<EngineDocumentUpdateResult>(
      request,
      kOperation,
      "document",
      true,
      "updated");
  if (result.ok) {
    const auto provider_write = UpsertPhysicalDocument(request, result);
    AddDocumentProviderWriteEvidence(&result, provider_write);
    if (!provider_write.ok) {
      result.ok = false;
      result.diagnostics.push_back(provider_write.diagnostic);
    }
    AddEngineNoSqlSurfaceEvidence(&result, "document", "persisted_document_update");
    AddApiBehaviorEvidence(&result,
                           "document_physical_provider",
                           "authoritative_rebuild_from_document_rows=true");
    AddApiBehaviorEvidence(&result, "mga_finality_authority", "engine_transaction_inventory");
  }
  return result;
}

EngineDocumentDeleteResult EngineDocumentDelete(const EngineDocumentDeleteRequest& request) {
  constexpr const char* kOperation = "nosql.document_delete";
  if (!request.context.cluster_authority_available && EngineNoSqlRequiresClusterAuthority(request)) {
    return EngineNoSqlClusterAuthorityUnavailable<EngineDocumentDeleteResult>(request, kOperation);
  }
  auto result = EngineNoSqlPersistedWriteResult<EngineDocumentDeleteResult>(
      request,
      kOperation,
      "document",
      true,
      "deleted",
      true);
  if (result.ok) {
    const auto provider_write = DeletePhysicalDocument(request);
    AddDocumentProviderWriteEvidence(&result, provider_write);
    if (!provider_write.ok) {
      result.ok = false;
      result.diagnostics.push_back(provider_write.diagnostic);
    }
    AddEngineNoSqlSurfaceEvidence(&result, "document", "persisted_document_delete");
    AddApiBehaviorEvidence(&result,
                           "document_physical_provider",
                           "authoritative_rebuild_from_document_rows=true");
    AddApiBehaviorEvidence(&result, "mga_finality_authority", "engine_transaction_inventory");
  }
  return result;
}

void EngineDocumentProviderCleanup(const EngineRequestContext& context,
                                   bool drop_persistent_state) {
  {
    std::lock_guard<std::mutex> guard(DocumentStoresMutex());
    DocumentStores().erase(StoreKey(context));
  }
  if (drop_persistent_state) {
    const auto path = DocumentProviderPath(context);
    if (!path.empty()) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
    const auto provider_path = DocumentPathPhysicalProviderPath(context);
    if (!provider_path.empty()) {
      std::error_code ignored;
      std::filesystem::remove(provider_path, ignored);
    }
  }
  (void)CleanupNoSqlProviderGenerations(context, drop_persistent_state);
}

}  // namespace scratchbird::engine::internal_api
