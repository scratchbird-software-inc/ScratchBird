// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "extensibility/executable_object_lifecycle.hpp"
#include "extensibility/executable_object_record_codec.hpp"
#include "catalog/binary_view_options.hpp"
#include <filesystem>

#include "crud_support/crud_store.hpp"
#include "dml/delete_api.hpp"
#include "engine/sblr/sblr_procedural_body_runtime.hpp"
#include "engine/sblr/sblr_procedure_abi_runtime.hpp"
#include "hash_digest.hpp"
#include "local_transaction_store.hpp"
#include "dml/insert_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "security/security_model.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

constexpr const char* kOperationCreate = "runtime.executable_object.create";
constexpr const char* kOperationAlter = "runtime.executable_object.alter";
constexpr const char* kOperationDrop = "runtime.executable_object.drop";
constexpr const char* kOperationQuiesce = "runtime.executable_object.quiesce";
constexpr const char* kOperationUnload = "runtime.executable_object.unload";
constexpr const char* kOperationBeginInvocation = "runtime.executable_object.begin_invocation";
constexpr const char* kOperationFinishInvocation = "runtime.executable_object.finish_invocation";
constexpr const char* kOperationInvoke = "runtime.executable_object.invoke";
constexpr const char* kOperationFireEventTrigger = "runtime.executable_object.fire_event_trigger";
constexpr const char* kOperationInspect = "runtime.executable_object.inspect";

std::string EventPath(const EngineRequestContext& context) {
  return context.database_path + ".sb.executable_object_events";
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream in(value);
  while (std::getline(in, current, delimiter)) { parts.push_back(current); }
  return parts;
}

std::uint64_t ParseU64(const std::string& value) {
  try {
    return static_cast<std::uint64_t>(std::stoull(value));
  } catch (...) {
    return 0;
  }
}

bool ParseBool(const std::string& value) {
  return value == "1" || value == "true" || value == "TRUE";
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool Contains(const std::string& value, const std::string& token) {
  return value.find(token) != std::string::npos;
}

std::string LowerAscii(std::string value) {
  for (char& c : value) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  return value;
}

std::string OptionValue(const EngineApiRequest& request, const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, prefix)) { return option.substr(prefix.size()); }
  }
  return {};
}

bool HasOptionToken(const EngineApiRequest& request, const std::string& token) {
  for (const auto& option : request.option_envelopes) {
    if (option == token || Contains(option, token)) { return true; }
  }
  return false;
}

std::string Join(const std::vector<std::string>& values, char delimiter) {
  std::string out;
  for (const auto& value : values) {
    if (!out.empty()) { out.push_back(delimiter); }
    out += value;
  }
  return out;
}

EngineApiDiagnostic ExecDiagnostic(const char* code, std::string detail);
EngineApiDiagnostic ExecDiagnostic(const char* code,const EngineUuid&) {
  return ExecDiagnostic(code,std::string("object_uuid"));
}
EngineApiDiagnostic OkDiagnostic();

struct RoutineDeleteColumnRangeCountDescriptor {
  EngineUuid table_uuid;
  EngineUuid column_uuid;
  std::uint32_t lower_input_slot = 0;
  std::uint32_t upper_input_slot = 0;
  std::uint32_t affected_rows_output_slot = 0;
  std::uint32_t yield_output_slot = 0;
};

bool IsRoutineDeleteColumnRangeCountDescriptor(std::string_view descriptor) {
  return descriptor == kRoutineDeleteColumnRangeCountDescriptorV1 ||
         descriptor.starts_with(
             std::string(kRoutineDeleteColumnRangeCountDescriptorV1) + "|");
}

std::optional<std::uint32_t> ParseRoutineSlot(std::string_view text) {
  if (text.empty()) { return std::nullopt; }
  std::uint32_t slot = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), slot);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      slot > 63) {
    return std::nullopt;
  }
  return slot;
}

std::optional<std::int64_t> ParseRoutineInteger(std::string_view text) {
  if (text.empty()) { return std::nullopt; }
  std::int64_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

bool IsRoutineIntegerType(std::string type_name) {
  type_name = LowerAscii(std::move(type_name));
  return type_name == "integer" || type_name == "int" ||
         type_name == "int32";
}

EngineApiDiagnostic ParseRoutineDeleteColumnRangeCountDescriptor(
    const std::string& encoded,
    RoutineDeleteColumnRangeCountDescriptor* descriptor) {
  if (descriptor == nullptr) {
    return ExecDiagnostic(kExecutableObjectDiagnosticRoutineDescriptorInvalid,
                          "descriptor_output_required");
  }
  const std::string prefix=std::string(kRoutineDeleteColumnRangeCountDescriptorV1)+"|";
  if(!encoded.starts_with(prefix)||encoded.size()!=prefix.size()+48)
    return ExecDiagnostic(kExecutableObjectDiagnosticRoutineDescriptorInvalid,"binary_delete_range_descriptor_required");
  const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(encoded.data()),encoded.size());
  std::size_t cursor=prefix.size();RoutineDeleteColumnRangeCountDescriptor parsed;
  if(!catalog_record_codec::Get(bytes,cursor,parsed.table_uuid)||parsed.table_uuid.is_nil()||
      !catalog_record_codec::Get(bytes,cursor,parsed.column_uuid)||parsed.column_uuid.is_nil()||
      !catalog_record_codec::Get(bytes,cursor,parsed.lower_input_slot)||
      !catalog_record_codec::Get(bytes,cursor,parsed.upper_input_slot)||
      !catalog_record_codec::Get(bytes,cursor,parsed.affected_rows_output_slot)||
      !catalog_record_codec::Get(bytes,cursor,parsed.yield_output_slot)||
      parsed.lower_input_slot!=0||parsed.upper_input_slot!=1||parsed.affected_rows_output_slot!=2||parsed.yield_output_slot!=2)
    return ExecDiagnostic(kExecutableObjectDiagnosticRoutineDescriptorInvalid,"binary_delete_range_descriptor_invalid");
  *descriptor=parsed;
  return OkDiagnostic();
}

bool CreateOrAlterProcedureRequested(const EngineApiRequest& request) {
  return HasOptionToken(request, "create_or_alter:true") ||
         HasOptionToken(request, "procedure_create_mode:create_or_alter") ||
         HasOptionToken(request,
                        "executable_descriptor_kind:create_or_alter_procedure");
}

EngineApiDiagnostic ExecDiagnostic(const char* code, std::string detail = {}) {
  std::string message_key = "engine.executable_object.lifecycle";
  if (code == std::string(kExecutableObjectDiagnosticDatabasePathRequired)) {
    message_key = "engine.executable_object.database_path_required";
  } else if (code == std::string(kExecutableObjectDiagnosticDatabaseWriteFailed)) {
    message_key = "engine.executable_object.database_write_failed";
  } else if (code == std::string(kExecutableObjectDiagnosticMgaTransactionRequired)) {
    message_key = "engine.executable_object.mga_transaction_required";
  } else if (code == std::string(kExecutableObjectDiagnosticSecurityContextRequired)) {
    message_key = "engine.executable_object.security_context_required";
  } else if (code == std::string(kExecutableObjectDiagnosticPermissionDenied)) {
    message_key = "engine.executable_object.permission_denied";
  } else if (code == std::string(kExecutableObjectDiagnosticUuidRequired)) {
    message_key = "engine.executable_object.uuid_required";
  } else if (code == std::string(kExecutableObjectDiagnosticKindRequired)) {
    message_key = "engine.executable_object.kind_required";
  } else if (code == std::string(kExecutableObjectDiagnosticUnsupportedKind)) {
    message_key = "engine.executable_object.unsupported_kind";
  } else if (code == std::string(kExecutableObjectDiagnosticSchemaUuidRequired)) {
    message_key = "engine.executable_object.schema_uuid_required";
  } else if (code == std::string(kExecutableObjectDiagnosticDuplicate)) {
    message_key = "engine.executable_object.duplicate";
  } else if (code == std::string(kExecutableObjectDiagnosticNotFound)) {
    message_key = "engine.executable_object.not_found";
  } else if (code == std::string(kExecutableObjectDiagnosticMgaVisibilityRefused)) {
    message_key = "engine.executable_object.mga_visibility_refused";
  } else if (code == std::string(kExecutableObjectDiagnosticStoredSblrRequired)) {
    message_key = "engine.executable_object.stored_sblr_required";
  } else if (code == std::string(kExecutableObjectDiagnosticStoredSblrProvenanceRequired)) {
    message_key = "engine.executable_object.stored_sblr_provenance_required";
  } else if (code == std::string(kExecutableObjectDiagnosticInternalProcedureRequired)) {
    message_key = "engine.executable_object.internal_procedure_required";
  } else if (code == std::string(kExecutableObjectDiagnosticExecutionBoundaryRefused)) {
    message_key = "engine.executable_object.execution_boundary_refused";
  } else if (code == std::string(kExecutableObjectDiagnosticDependencyNotVisible)) {
    message_key = "engine.executable_object.dependency_not_visible";
  } else if (code == std::string(kExecutableObjectDiagnosticDependencyInvalidated)) {
    message_key = "engine.executable_object.dependency_invalidated";
  } else if (code == std::string(kExecutableObjectDiagnosticSideEffectPolicyDenied)) {
    message_key = "engine.executable_object.side_effect_policy_denied";
  } else if (code == std::string(kExecutableObjectDiagnosticUnloadBlockedActiveInvocation)) {
    message_key = "engine.executable_object.unload_blocked_active_invocation";
  } else if (code == std::string(kExecutableObjectDiagnosticQuiescing)) {
    message_key = "engine.executable_object.quiescing";
  } else if (code == std::string(kExecutableObjectDiagnosticUnloaded)) {
    message_key = "engine.executable_object.unloaded";
  } else if (code == std::string(kExecutableObjectDiagnosticInvocationNotFound)) {
    message_key = "engine.executable_object.invocation_not_found";
  } else if (code == std::string(kExecutableObjectDiagnosticEventTriggerAuthorityUnavailable)) {
    message_key = "sbsql.event_trigger_authority_unavailable";
  } else if (code == std::string(kExecutableObjectDiagnosticEventTriggerEventUnsupported)) {
    message_key = "engine.executable_object.event_trigger_event_unsupported";
  } else if (code == std::string(kExecutableObjectDiagnosticExactMgaSelectorRequired)) {
    message_key = "engine.executable_object.exact_mga_selector_required";
  } else if (code == std::string(kExecutableObjectDiagnosticExactMgaSelectorMismatch)) {
    message_key = "engine.executable_object.exact_mga_selector_mismatch";
  } else if (code == std::string(kExecutableObjectDiagnosticRoutineDescriptorInvalid)) {
    message_key = "engine.executable_object.routine_descriptor_invalid";
  } else if (code == std::string(kExecutableObjectDiagnosticRoutineArgumentInvalid)) {
    message_key = "engine.executable_object.routine_argument_invalid";
  } else if (code == std::string(kExecutableObjectDiagnosticRoutineBindingNotVisible)) {
    message_key = "engine.executable_object.routine_binding_not_visible";
  } else if (code ==
             std::string(
                 kExecutableObjectDiagnosticPreparedMetadataVersionMismatch)) {
    message_key =
        "engine.executable_object.prepared_metadata_version_mismatch";
  }
  return EngineApiDiagnostic{code, std::move(message_key), std::move(detail), true};
}

EngineApiDiagnostic OkDiagnostic() {
  return EngineApiDiagnostic{"SB_ENGINE_API_OK", "engine.api.ok", {}, false};
}

template <typename TResult>
TResult SuccessResult(const EngineRequestContext& context, std::string operation_id) {
  TResult result;
  result.ok = true;
  result.operation_id = std::move(operation_id);
  result.transaction_uuid = context.transaction_uuid;
  result.local_transaction_id = context.local_transaction_id;
  result.embedded_trust_mode_observed = context.trust_mode == EngineTrustMode::embedded_in_process;
  return result;
}

template <typename TResult>
TResult DiagnosticResult(const EngineRequestContext& context,
                         std::string operation_id,
                         EngineApiDiagnostic diagnostic) {
  TResult result;
  result.ok = false;
  result.operation_id = std::move(operation_id);
  result.transaction_uuid = context.transaction_uuid;
  result.local_transaction_id = context.local_transaction_id;
  result.embedded_trust_mode_observed = context.trust_mode == EngineTrustMode::embedded_in_process;
  result.diagnostics.push_back(std::move(diagnostic));
  return result;
}

void AddRow(EngineApiResult* result, ApiBehaviorFields fields) {
  auto row=ApiBehaviorRow(std::move(fields));
  row.requested_row_uuid=GenerateCrudEngineUuid("row");
  result->result_shape.result_kind="executable_object_lifecycle_rows";
  result->result_shape.rows.push_back(std::move(row));
}
void AddEvidence(EngineApiResult* result,std::string kind,EngineEvidenceValue id) {
  result->evidence.push_back({std::move(kind),std::move(id)});
}

void AddPreparedMetadataEvidence(const EngineRequestContext& context,
                                 EngineApiResult* result) {
  if (!context.statement_metadata_snapshot_engine_owned) { return; }
  AddEvidence(result, "prepared_metadata_binding", "consumed");
  AddEvidence(result,
              "prepared_metadata_snapshot_uuid",
              context.statement_metadata_snapshot_uuid);
  AddEvidence(
      result,
      "prepared_metadata_exact_version",
      std::to_string(
              context.prepared_metadata_required_executable_generation) +
          ":" +
          std::to_string(context.prepared_metadata_required_metadata_epoch));
  AddEvidence(
      result,
      "prepared_metadata_active_exclusion_count",
      std::to_string(context
                         .statement_metadata_snapshot_active_excluded_local_transaction_ids
                         .size()));
  AddEvidence(
      result,
      "prepared_metadata_in_doubt_exclusion_count",
      std::to_string(context
                         .statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids
                         .size()));
}

bool EventVisible(
    const EngineRequestContext& context,
    std::uint64_t creator_tx,
    const scratchbird::storage::database::LocalTransactionStoreResult*
        transaction_inventory) {
  if (creator_tx == 0) { return true; }
  const bool engine_metadata_snapshot =
      context.statement_metadata_snapshot_engine_owned;
  const auto metadata_excludes = [&](const auto& excluded) {
    return std::find(excluded.begin(), excluded.end(), creator_tx) !=
           excluded.end();
  };
  if (transaction_inventory != nullptr && transaction_inventory->ok()) {
    for (const auto& entry : transaction_inventory->inventory.entries) {
      if (!entry.identity.local_id.valid() ||
          entry.identity.local_id.value != creator_tx) {
        continue;
      }
      using scratchbird::transaction::mga::TransactionState;
      if (creator_tx == context.local_transaction_id &&
          entry.identity.transaction_uuid.valid() &&
          entry.identity.transaction_uuid.value ==
              context.transaction_uuid) {
        return entry.state == TransactionState::active ||
               entry.state == TransactionState::read_only_active ||
               entry.state == TransactionState::preparing ||
               entry.state == TransactionState::prepared;
      }
      if (engine_metadata_snapshot &&
          (metadata_excludes(
               context
                   .statement_metadata_snapshot_active_excluded_local_transaction_ids) ||
           metadata_excludes(
               context
                   .statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids))) {
        return false;
      }
      if (!scratchbird::transaction::mga::HasCommittedInventoryOutcome(entry)) {
        return false;
      }
      const std::uint64_t visible_through =
          engine_metadata_snapshot
              ? context
                    .statement_metadata_snapshot_visible_through_local_transaction_id
              : context.snapshot_visible_through_local_transaction_id != 0
                    ? context.snapshot_visible_through_local_transaction_id
                    : context.local_transaction_id;
      return engine_metadata_snapshot
                 ? creator_tx <= visible_through
                 : visible_through == 0 || creator_tx <= visible_through;
    }
    return false;
  }
  if (context.local_transaction_id != 0 &&
      creator_tx == context.local_transaction_id) {
    return true;
  }
  if (engine_metadata_snapshot) {
    if (metadata_excludes(
            context
                .statement_metadata_snapshot_active_excluded_local_transaction_ids) ||
        metadata_excludes(
            context
                .statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids)) {
      return false;
    }
    return creator_tx <=
           context
               .statement_metadata_snapshot_visible_through_local_transaction_id;
  }
  if (context.snapshot_visible_through_local_transaction_id != 0) {
    return creator_tx <= context.snapshot_visible_through_local_transaction_id;
  }
  if (context.local_transaction_id != 0) { return creator_tx <= context.local_transaction_id; }
  return false;
}

bool ValidObjectKind(const std::string& kind) {
  static const std::set<std::string> kKinds = {
      "routine", "procedure", "function", "trigger", "event_trigger", "package", "stored_sblr"};
  return kKinds.count(kind) != 0;
}

bool IsExecutableDependencyKind(const std::string& kind) {
  return kind.empty() || kind == "executable_object" || ValidObjectKind(kind);
}

bool ValidEventTriggerEvent(const std::string& event_name) {
  static const std::set<std::string> kEvents = {
      "DDL_COMMAND_START", "DDL_COMMAND_END", "SQL_DROP", "TABLE_REWRITE"};
  return kEvents.count(event_name) != 0;
}

std::string ObjectKind(const EngineApiRequest& request) {
  if (!request.target_object.object_kind.empty()) { return LowerAscii(request.target_object.object_kind); }
  if (!request.sql_object_reference.expected_object_type.empty()) {
    return LowerAscii(request.sql_object_reference.expected_object_type);
  }
  return LowerAscii(OptionValue(request, "object_kind:"));
}

EngineUuid ObjectUuid(const EngineApiRequest& request) {
  if (!request.target_object.uuid.is_nil()) { return request.target_object.uuid; }
  if (!request.bound_object_identity.object_uuid.is_nil()) {
    return request.bound_object_identity.object_uuid;
  }
  return BinaryViewUuid(OptionValue(request, "object_uuid:"));
}

EngineUuid PackageUuid(const EngineApiRequest& request) {
  if (!request.context.current_package_uuid.is_nil()) {
    return request.context.current_package_uuid;
  }
  return BinaryViewUuid(OptionValue(request, "package_uuid:"));
}

std::string ExecutorKind(const EngineApiRequest& request,
                         const EngineExecutableObjectRecord* existing = nullptr) {
  auto executor = LowerAscii(OptionValue(request, "executor:"));
  if (executor.empty()) { executor = LowerAscii(OptionValue(request, "execution_engine:")); }
  if (executor == "internal") { executor = "internal_procedure"; }
  if (executor.empty() && existing != nullptr) { return existing->executor_kind; }
  return executor.empty() ? "metadata_only" : executor;
}

std::string StoredSblrHash(const EngineApiRequest& request,
                           const EngineExecutableObjectRecord* existing = nullptr) {
  auto hash = OptionValue(request, "sblr_hash:");
  if (hash.empty()) { hash = OptionValue(request, "stored_sblr_hash:"); }
  if (hash.empty() && existing != nullptr) { return existing->stored_sblr_hash; }
  return hash;
}

std::string StoredSblrProvenance(const EngineApiRequest& request,
                                 const EngineExecutableObjectRecord* existing = nullptr) {
  auto provenance = OptionValue(request, "sblr_provenance:");
  if (provenance.empty()) { provenance = OptionValue(request, "stored_sblr_provenance:"); }
  if (provenance.empty() && existing != nullptr) { return existing->stored_sblr_provenance; }
  return provenance;
}

std::string InternalProcedureId(const EngineApiRequest& request,
                                const EngineExecutableObjectRecord* existing = nullptr) {
  auto procedure_id = OptionValue(request, "internal_procedure_id:");
  if (procedure_id.empty()) { procedure_id = OptionValue(request, "internal_proc:"); }
  if (procedure_id.empty() && existing != nullptr) { return existing->internal_procedure_id; }
  return procedure_id;
}

std::string SideEffectClass(const EngineApiRequest& request,
                            const EngineExecutableObjectRecord* existing = nullptr) {
  auto value = LowerAscii(OptionValue(request, "side_effect_class:"));
  if (value.empty()) { value = LowerAscii(OptionValue(request, "side_effect:")); }
  if (value.empty() && existing != nullptr) { return existing->side_effect_class; }
  return value.empty() ? "none" : value;
}

std::string EventTriggerEvent(const EngineApiRequest& request,
                              const EngineExecutableObjectRecord* existing = nullptr) {
  auto event_name = OptionValue(request, "event:");
  if (event_name.empty()) { event_name = OptionValue(request, "event_trigger_event:"); }
  if (event_name.empty() && existing != nullptr) { return existing->event_trigger_event; }
  return event_name;
}

std::string PayloadFromRequest(const EngineApiRequest& request) {
  std::vector<std::string> fields;
  const auto payload = OptionValue(request, "payload:");
  if (!payload.empty()) { fields.push_back("payload_hash_or_descriptor=" + payload); }
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, "internal_procedure_id:") ||
        StartsWith(option, "executor:") ||
        StartsWith(option, "side_effect_class:") ||
        StartsWith(option, "compiled_body_provenance:") ||
        StartsWith(option, "compiled_body_descriptor:") ||
        StartsWith(option, "procedure_body_sblr_uuid:") ||
        StartsWith(option, "procedure_body_sblr_generation:") ||
        StartsWith(option, "procedure_body_bytes:") ||
        StartsWith(option, "procedure_body_sha256:") ||
        StartsWith(option, "procedure_abi_uuid:") ||
        StartsWith(option, "procedure_abi_generation:") ||
        StartsWith(option, "procedure_abi_bytes:") ||
        StartsWith(option, "procedure_abi_evidence_sha256:") ||
        StartsWith(option, "procedure_parameter_count:") ||
        StartsWith(option, "procedure_signature_sha256:") ||
        StartsWith(option, "procedure_effect_set_sha256:") ||
        StartsWith(option, "procedure_recovery_uuid:") ||
        StartsWith(option, "procedure_mutation_uuid:") ||
        StartsWith(option, "procedure_publication_barrier_uuid:") ||
        StartsWith(option, "trigger_timing:") ||
        StartsWith(option, "trigger_event:") ||
        StartsWith(option, "trigger_scope:") ||
        StartsWith(option, "trigger_target_table_uuid:") ||
        StartsWith(option, "trigger_target_table_name:") ||
        StartsWith(option, "trigger_body_sblr_uuid:") ||
        StartsWith(option, "trigger_body_sblr_generation:") ||
        StartsWith(option, "trigger_recovery_uuid:") ||
        StartsWith(option, "trigger_mutation_uuid:") ||
        StartsWith(option, "trigger_enabled_state:") ||
        StartsWith(option, "trigger_position:") ||
        StartsWith(option, "trigger_security_mode:") ||
        StartsWith(option, "trigger_image_mode:") ||
        StartsWith(option, "trigger_execution_mode:") ||
        StartsWith(option, "trigger_recursion_mode:") ||
        StartsWith(option, "trigger_failure_policy:") ||
        StartsWith(option, "trigger_origin_filter:") ||
        StartsWith(option, "trigger_definition_generation:") ||
        StartsWith(option, "related_object_") ||
        StartsWith(option, "routine_parameter_count:") ||
        StartsWith(option, "routine_parameter_") ||
        StartsWith(option, "routine_return_count:") ||
        StartsWith(option, "routine_return_")) {
      const auto colon=option.find(':');
      if(colon!=std::string::npos&&option.substr(0,colon).ends_with("uuid")&&
          BinaryViewUuid(option.substr(colon+1)).is_nil())return {};
      fields.push_back(option);
    }
  }
  if (!request.descriptors.empty()) {
    fields.push_back("descriptor=" + request.descriptors.front().canonical_type_name);
  }
  if (!request.rows.empty()) { fields.push_back("row_parameter_count=" + std::to_string(request.rows.size())); }
  return EncodeBinaryViewOptions(fields);
}

std::string PayloadFieldValue(const std::string& payload, const std::string& prefix) {
  return BinaryViewOptionValue(payload, prefix);
}

bool RequestsExecutionBoundaryBypass(const EngineApiRequest& request) {
  return HasOptionToken(request, "parser_execute") ||
         HasOptionToken(request, "parser_owned_execution") ||
         HasOptionToken(request, "sql_text:") ||
         HasOptionToken(request, "direct_storage") ||
         HasOptionToken(request, "raw_page") ||
         HasOptionToken(request, "external_engine_execution") ||
         HasOptionToken(request, "reference_execution") ||
         HasOptionToken(request, "bypass_sblr") ||
         HasOptionToken(request, "bypass_mga") ||
         HasOptionToken(request, "bypass_catalog") ||
         HasOptionToken(request, "bypass_security");
}

bool HasManagePermission(const EngineApiRequest& request) {
  return SecurityContextHasRight(request.context,
                                 "CATALOG_MUTATE",
                                 request.target_object.uuid);
}

bool HasInspectPermission(const EngineApiRequest& request) {
  return HasManagePermission(request) ||
         SecurityContextHasRight(request.context,
                                 "DISCOVER",
                                 request.target_object.uuid);
}

bool HasInvokePermission(const EngineApiRequest& request, const EngineUuid& object_uuid) {
  return HasManagePermission(request) ||
         SecurityContextHasRight(request.context, "EXECUTE", object_uuid);
}

bool HasEventTriggerManagePermission(const EngineApiRequest& request) {
  return SecurityContextHasRight(request.context,
                                 "EVENT_ADMIN",
                                 request.target_object.uuid);
}

bool HasEventTriggerDispatchAuthority(const EngineApiRequest& request) {
  return SecurityContextHasRight(request.context,
                                 "EVENT_PUBLISH",
                                 request.target_object.uuid);
}

bool SideEffectAllowed(const EngineApiRequest& request, const std::string& side_effect_class) {
  const auto sidefx = LowerAscii(side_effect_class);
  if (sidefx.empty() || sidefx == "none" || sidefx == "internal_only") { return true; }
  return HasOptionToken(request, "policy:executable.side_effect:allow") ||
         HasOptionToken(request, "side_effect_policy:allow") ||
         HasOptionToken(request, "permission:allow_side_effect_execution");
}

EngineApiDiagnostic ValidateContext(const EngineApiRequest& request,
                                    bool require_transaction,
                                    bool require_security) {
  if (request.context.database_path.empty()) {
    return ExecDiagnostic(kExecutableObjectDiagnosticDatabasePathRequired, "database_path");
  }
  if (require_transaction && request.context.local_transaction_id == 0) {
    return ExecDiagnostic(kExecutableObjectDiagnosticMgaTransactionRequired, "local_transaction_id");
  }
  if (require_security && !request.context.security_context_present) {
    return ExecDiagnostic(kExecutableObjectDiagnosticSecurityContextRequired, "security_context");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateManageAuthority(const EngineApiRequest& request,
                                            const std::string& object_kind) {
  const auto context = ValidateContext(request, true, true);
  if (context.error) { return context; }
  if (object_kind == "event_trigger" && !HasEventTriggerManagePermission(request)) {
    return ExecDiagnostic(kExecutableObjectDiagnosticPermissionDenied, "manage_event_trigger");
  }
  if (!HasManagePermission(request)) {
    return ExecDiagnostic(kExecutableObjectDiagnosticPermissionDenied, "manage_executable");
  }
  if (RequestsExecutionBoundaryBypass(request)) {
    return ExecDiagnostic(kExecutableObjectDiagnosticExecutionBoundaryRefused,
                          "engine_accepts_uuid_sblr_or_internal_procedure_operations_only");
  }
  return OkDiagnostic();
}

std::string CompiledBodyDescriptor(const EngineApiRequest& request) {
  return OptionValue(request, "compiled_body_descriptor:");
}

EngineApiDiagnostic ValidateExactMgaSelector(const EngineRequestContext& context) {
  if (context.local_transaction_id == 0 ||
      context.transaction_uuid.is_nil()) {
    return ExecDiagnostic(kExecutableObjectDiagnosticExactMgaSelectorRequired,
                          "local_transaction_id_and_transaction_uuid_required");
  }
  const auto loaded =
      scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase(
          context.database_path);
  if (!loaded.ok()) {
    return ExecDiagnostic(kExecutableObjectDiagnosticExactMgaSelectorRequired,
                          "durable_transaction_inventory_unavailable");
  }
  for (const auto& entry : loaded.inventory.entries) {
    if (!entry.identity.local_id.valid() ||
        entry.identity.local_id.value != context.local_transaction_id) {
      continue;
    }
    if (!entry.identity.transaction_uuid.valid() ||
        entry.identity.transaction_uuid.value !=
            context.transaction_uuid) {
      return ExecDiagnostic(kExecutableObjectDiagnosticExactMgaSelectorMismatch,
                            "local_transaction_id_and_transaction_uuid_do_not_match");
    }
    using scratchbird::transaction::mga::TransactionState;
    if (entry.state != TransactionState::active) {
      return ExecDiagnostic(kExecutableObjectDiagnosticExactMgaSelectorMismatch,
                            "selected_transaction_is_not_active_write_authority");
    }
    return OkDiagnostic();
  }
  return ExecDiagnostic(kExecutableObjectDiagnosticExactMgaSelectorMismatch,
                        "selected_transaction_not_found_in_durable_inventory");
}

EngineApiDiagnostic ValidateRoutineIntegerSignature(
    const EngineApiRequest& request) {
  if (OptionValue(request, "routine_parameter_count:") != "2" ||
      OptionValue(request, "routine_return_count:") != "1") {
    return ExecDiagnostic(kExecutableObjectDiagnosticRoutineDescriptorInvalid,
                          "two_integer_inputs_and_one_integer_output_required");
  }
  for (std::size_t slot = 0; slot < 2; ++slot) {
    const std::string prefix = "routine_parameter_" + std::to_string(slot);
    if (LowerAscii(OptionValue(request, prefix + "_mode:")) != "in" ||
        !IsRoutineIntegerType(OptionValue(request, prefix + "_type:"))) {
      return ExecDiagnostic(kExecutableObjectDiagnosticRoutineDescriptorInvalid,
                            prefix + "_must_be_integer_in");
    }
  }
  if (!IsRoutineIntegerType(OptionValue(request, "routine_return_0_type:"))) {
    return ExecDiagnostic(kExecutableObjectDiagnosticRoutineDescriptorInvalid,
                          "routine_return_0_must_be_integer_out");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ResolveRoutineColumnBinding(
    const EngineRequestContext& context,
    const RoutineDeleteColumnRangeCountDescriptor& routine,
    std::string* column_name) {
  // Stored routine binding is a read-only validation boundary.  CREATE TABLE
  // owns descriptor persistence; routine creation/invocation must not repair
  // or synthesize catalog/storage metadata as a side effect of validation.
  const auto loaded =
      LoadMgaRelationStorageDescriptor(context, routine.table_uuid);
  if (!loaded.ok) { return loaded.diagnostic; }
  for (const auto& column : loaded.descriptor.columns) {
    if (column.column_uuid != routine.column_uuid) { continue; }
    if (column.canonical_name_key.empty()) {
      return ExecDiagnostic(kExecutableObjectDiagnosticRoutineBindingNotVisible,
                            "column_name_key_missing");
    }
    if (column_name != nullptr) { *column_name = column.canonical_name_key; }
    return OkDiagnostic();
  }
  return ExecDiagnostic(kExecutableObjectDiagnosticRoutineBindingNotVisible,
                        "column_uuid");
}

EngineApiDiagnostic ValidateRoutineDeleteColumnRangeCountProgram(
    const EngineApiRequest& request,
    const std::string& object_kind,
    const std::string& descriptor) {
  if (!IsRoutineDeleteColumnRangeCountDescriptor(descriptor)) {
    return OkDiagnostic();
  }
  if (object_kind != "procedure") {
    return ExecDiagnostic(kExecutableObjectDiagnosticRoutineDescriptorInvalid,
                          "delete_column_range_count_requires_procedure");
  }
  const auto selector = ValidateExactMgaSelector(request.context);
  if (selector.error) { return selector; }
  const auto signature = ValidateRoutineIntegerSignature(request);
  if (signature.error) { return signature; }
  RoutineDeleteColumnRangeCountDescriptor parsed;
  const auto decoded =
      ParseRoutineDeleteColumnRangeCountDescriptor(descriptor, &parsed);
  if (decoded.error) { return decoded; }
  return ResolveRoutineColumnBinding(request.context, parsed, nullptr);
}

EngineApiDiagnostic ValidateStoredProgram(const EngineApiRequest& request,
                                          const std::string& object_kind,
                                          const EngineExecutableObjectRecord* existing,
                                          std::string* executor_kind,
                                          std::string* stored_sblr_hash,
                                          std::string* stored_sblr_provenance,
                                          std::string* internal_procedure_id,
                                          std::string* side_effect_class,
                                          std::string* event_trigger_event) {
  *executor_kind = ExecutorKind(request, existing);
  *stored_sblr_hash = StoredSblrHash(request, existing);
  *stored_sblr_provenance = StoredSblrProvenance(request, existing);
  *internal_procedure_id = InternalProcedureId(request, existing);
  *side_effect_class = SideEffectClass(request, existing);
  *event_trigger_event = EventTriggerEvent(request, existing);

  if (*executor_kind != "sblr" && *executor_kind != "internal_procedure" &&
      *executor_kind != "metadata_only") {
    return ExecDiagnostic(kExecutableObjectDiagnosticExecutionBoundaryRefused, *executor_kind);
  }
  if (*executor_kind == "sblr") {
    if (stored_sblr_hash->empty() || !StartsWith(*stored_sblr_hash, "sha256:")) {
      return ExecDiagnostic(kExecutableObjectDiagnosticStoredSblrRequired, "sha256_sblr_hash");
    }
    if (stored_sblr_provenance->empty()) {
      return ExecDiagnostic(kExecutableObjectDiagnosticStoredSblrProvenanceRequired, "sblr_provenance");
    }
  } else if (*executor_kind == "internal_procedure" && internal_procedure_id->empty()) {
    return ExecDiagnostic(kExecutableObjectDiagnosticInternalProcedureRequired, "internal_procedure_id");
  }
  if (object_kind == "event_trigger") {
    if (!ValidEventTriggerEvent(*event_trigger_event)) {
      return ExecDiagnostic(kExecutableObjectDiagnosticEventTriggerEventUnsupported, *event_trigger_event);
    }
  }
  const auto routine_program = ValidateRoutineDeleteColumnRangeCountProgram(
      request, object_kind, CompiledBodyDescriptor(request));
  if (routine_program.error) { return routine_program; }
  return OkDiagnostic();
}

template<class Record> std::string ExecutableEvent(const Record& record) {
  std::string bytes;
  if(!EncodeExecutableLifecycleRecord(record,&bytes))return {};
  return bytes;
}
std::string ObjectEvent(const EngineExecutableObjectRecord& r) { return ExecutableEvent(r); }
std::string DependencyEvent(const EngineExecutableDependencyRecord& r) { return ExecutableEvent(r); }
std::string InvocationEvent(const EngineExecutableInvocationRecord& r) { return ExecutableEvent(r); }
std::string InvalidationEvent(std::uint64_t tx,const EngineUuid& object_uuid,
    const EngineUuid& reason_uuid,std::uint64_t dependency_generation,std::uint64_t metadata_epoch) {
  EngineExecutableInvalidationRecord r;r.creator_tx=tx;r.object_uuid=object_uuid;r.reason_uuid=reason_uuid;
  r.dependency_generation=dependency_generation;r.metadata_epoch=metadata_epoch;return ExecutableEvent(r);
}
std::string CacheInvalidateEvent(const EngineRequestContext& context,const std::string& operation_id,
    const EngineUuid& object_uuid,std::uint64_t metadata_epoch) {
  EngineExecutableCacheRecord r;r.creator_tx=context.local_transaction_id;r.object_uuid=object_uuid;
  r.operation_id=operation_id;r.metadata_epoch=metadata_epoch;r.security_epoch=context.security_epoch;
  r.resource_epoch=context.resource_epoch;return ExecutableEvent(r);
}
std::string EventTriggerFireEvent(const EngineRequestContext& context,const EngineUuid& trigger_uuid,
    const std::string& event_name,const std::string& command_tag,std::uint64_t metadata_epoch) {
  EngineExecutableTriggerFireRecord r;r.creator_tx=context.local_transaction_id;r.object_uuid=trigger_uuid;
  r.event_name=event_name;r.command_tag=command_tag;r.metadata_epoch=metadata_epoch;return ExecutableEvent(r);
}

EngineApiDiagnostic AppendEvent(const EngineRequestContext& context, const std::string& event) {
  if (context.database_path.empty()) {
    return ExecDiagnostic(kExecutableObjectDiagnosticDatabasePathRequired, "database_path");
  }
  ApiBehaviorRecord frame;ExecutableLifecycleRecord decoded;
  if(!DecodeApiBehaviorRecord({reinterpret_cast<const std::uint8_t*>(event.data()),event.size()},&frame)||
      !DecodeExecutableLifecycleFrame(frame,&decoded)||frame.creator_tx!=context.local_transaction_id)
    return ExecDiagnostic(kExecutableObjectDiagnosticDatabaseWriteFailed,"binary_executable_event_invalid");
  std::ofstream out(EventPath(context), std::ios::binary | std::ios::app);
  if (!out) { return ExecDiagnostic(kExecutableObjectDiagnosticDatabaseWriteFailed, "open"); }
  out.write(event.data(),static_cast<std::streamsize>(event.size()));
  out.flush();
  if (!out) { return ExecDiagnostic(kExecutableObjectDiagnosticDatabaseWriteFailed, "flush"); }
  return OkDiagnostic();
}

EngineApiDiagnostic AppendCacheInvalidation(const EngineRequestContext& context,
                                            const std::string& operation_id,
                                            const EngineUuid& object_uuid,
                                            std::uint64_t metadata_epoch) {
  return AppendEvent(context, CacheInvalidateEvent(context, operation_id, object_uuid, metadata_epoch));
}

struct LoadOptions {
  bool enforce_visibility = true;
};

EngineLoadExecutableObjectLifecycleStateResult LoadState(const EngineRequestContext& context,
                                                         LoadOptions options) {
  EngineLoadExecutableObjectLifecycleStateResult result;
  if (context.database_path.empty()) {
    result.diagnostic = ExecDiagnostic(kExecutableObjectDiagnosticDatabasePathRequired, "database_path");
    return result;
  }
  std::ifstream in(EventPath(context), std::ios::binary);
  if (!in) {
    std::error_code ec;
    if(std::filesystem::exists(EventPath(context),ec)||ec) {
      result.diagnostic=ExecDiagnostic(kExecutableObjectDiagnosticDatabaseWriteFailed,"executable_event_read_failed");return result;
    }
    result.ok = true;
    result.diagnostic = OkDiagnostic();
    return result;
  }

  std::map<EngineUuid, EngineExecutableObjectRecord> objects;
  std::map<std::tuple<EngineUuid,EngineUuid,std::string>, EngineExecutableDependencyRecord> dependencies;
  std::map<EngineUuid, EngineExecutableInvocationRecord> active_invocations;
  const auto transaction_inventory =
      scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase(
          context.database_path);
  if (options.enforce_visibility &&
      context.statement_metadata_snapshot_engine_owned &&
      !transaction_inventory.ok()) {
    result.diagnostic = ExecDiagnostic(
        kExecutableObjectDiagnosticMgaVisibilityRefused,
        "prepared_metadata_transaction_inventory_unavailable");
    return result;
  }
  std::uint64_t event_sequence = 0;
  while (in.peek()!=std::char_traits<char>::eof()) {
    ApiBehaviorRecord frame;ExecutableLifecycleRecord event;
    if(!ReadApiBehaviorRecord(in,&frame)||!DecodeExecutableLifecycleFrame(frame,&event)) {
      result.diagnostic=ExecDiagnostic(kExecutableObjectDiagnosticDatabaseWriteFailed,"binary_executable_event_corrupt_or_legacy");return result;
    }
    ++event_sequence;
    if(options.enforce_visibility&&!EventVisible(context,frame.creator_tx,&transaction_inventory))continue;
    std::visit([&](auto& record) {
      using T=std::decay_t<decltype(record)>;
      record.event_sequence=event_sequence;
      result.state.metadata_epoch=std::max(result.state.metadata_epoch,record.metadata_epoch);
      if constexpr(std::is_same_v<T,EngineExecutableObjectRecord>) {
        if(record.deleted||record.lifecycle_state=="dropped")objects.erase(record.object_uuid);
        else objects[record.object_uuid]=std::move(record);
      } else if constexpr(std::is_same_v<T,EngineExecutableDependencyRecord>) {
        result.state.dependency_generation=std::max(result.state.dependency_generation,record.dependency_generation);
        const auto key=std::make_tuple(record.source_uuid,record.dependency_uuid,record.dependency_kind);
        if(record.deleted)dependencies.erase(key);else dependencies[key]=std::move(record);
      } else if constexpr(std::is_same_v<T,EngineExecutableInvalidationRecord>) {
        result.state.dependency_generation=std::max(result.state.dependency_generation,record.dependency_generation);
        auto found=objects.find(record.object_uuid);
        if(found!=objects.end()) {
          found->second.invalidated=true;found->second.invalidated_generation=record.dependency_generation;
          found->second.invalidation_reason_uuid=record.reason_uuid;
        }
      } else if constexpr(std::is_same_v<T,EngineExecutableInvocationRecord>) {
        if(record.lifecycle_state=="active")active_invocations[record.invocation_lease_uuid]=std::move(record);
        else active_invocations.erase(record.invocation_lease_uuid);
      }
    },event);
  }
  if(in.bad()) {result.diagnostic=ExecDiagnostic(kExecutableObjectDiagnosticDatabaseWriteFailed,"executable_event_read_failed");return result;}

  std::set<EngineUuid> active_objects;
  for (auto& [_, object] : objects) {
    active_objects.insert(object.object_uuid);
    result.state.objects.push_back(std::move(object));
  }
  for (auto& [_, dependency] : dependencies) {
    if (active_objects.count(dependency.source_uuid) != 0 &&
        (!IsExecutableDependencyKind(dependency.dependency_kind) ||
         active_objects.count(dependency.dependency_uuid) != 0)) {
      result.state.dependencies.push_back(std::move(dependency));
    }
  }
  for (auto& [_, invocation] : active_invocations) {
    if (active_objects.count(invocation.object_uuid) != 0) {
      result.state.active_invocations.push_back(std::move(invocation));
    }
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

const EngineExecutableObjectRecord* FindObject(const EngineExecutableObjectLifecycleState& state,
                                               const EngineUuid& object_uuid) {
  for (const auto& object : state.objects) {
    if (object.object_uuid == object_uuid) { return &object; }
  }
  return nullptr;
}

std::vector<EngineExecutableDependencyRecord> DependenciesForSource(
    const EngineExecutableObjectLifecycleState& state,
    const EngineUuid& source_uuid) {
  std::vector<EngineExecutableDependencyRecord> dependencies;
  for (const auto& dependency : state.dependencies) {
    if (dependency.source_uuid == source_uuid) { dependencies.push_back(dependency); }
  }
  return dependencies;
}

std::vector<EngineExecutableDependencyRecord> DependentsOf(
    const EngineExecutableObjectLifecycleState& state,
    const EngineUuid& dependency_uuid) {
  std::vector<EngineExecutableDependencyRecord> dependents;
  for (const auto& dependency : state.dependencies) {
    if (dependency.dependency_uuid == dependency_uuid) { dependents.push_back(dependency); }
  }
  return dependents;
}

std::uint64_t ActiveInvocationCount(const EngineExecutableObjectLifecycleState& state,
                                    const EngineUuid& object_uuid) {
  std::uint64_t count = 0;
  for (const auto& invocation : state.active_invocations) {
    if (invocation.object_uuid == object_uuid) { ++count; }
  }
  return count;
}

EngineApiDiagnostic FindVisibleObject(const EngineApiRequest& request,
                                      const EngineUuid& object_uuid,
                                      EngineExecutableObjectRecord* record,
                                      EngineExecutableObjectLifecycleState* visible_state) {
  const auto visible = LoadState(request.context, {.enforce_visibility = true});
  if (!visible.ok) { return visible.diagnostic; }
  const auto* found = FindObject(visible.state, object_uuid);
  if (found == nullptr) {
    const auto all = LoadState(request.context, {.enforce_visibility = false});
    if (all.ok && FindObject(all.state, object_uuid) != nullptr) {
      return ExecDiagnostic(kExecutableObjectDiagnosticMgaVisibilityRefused, object_uuid);
    }
    return ExecDiagnostic(kExecutableObjectDiagnosticNotFound, object_uuid);
  }
  if (record != nullptr) { *record = *found; }
  if (visible_state != nullptr) { *visible_state = visible.state; }
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateRelatedObjects(const EngineExecutableObjectLifecycleState& state,
                                           const EngineApiRequest& request) {
  for (const auto& related : request.related_objects) {
    if (related.uuid.is_nil()) { continue; }
    const auto related_kind = LowerAscii(related.object_kind);
    if (!IsExecutableDependencyKind(related_kind)) { continue; }
    const auto* dependency = FindObject(state, related.uuid);
    if (dependency == nullptr) {
      return ExecDiagnostic(kExecutableObjectDiagnosticDependencyNotVisible, related.uuid);
    }
    if (dependency->invalidated) {
      return ExecDiagnostic(kExecutableObjectDiagnosticDependencyInvalidated, related.uuid);
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateExecutableDependencies(const EngineExecutableObjectLifecycleState& state,
                                                   const EngineExecutableObjectRecord& object) {
  for (const auto& dependency : DependenciesForSource(state, object.object_uuid)) {
    if (!IsExecutableDependencyKind(dependency.dependency_kind)) { continue; }
    const auto* dependency_object = FindObject(state, dependency.dependency_uuid);
    if (dependency_object == nullptr) {
      return ExecDiagnostic(kExecutableObjectDiagnosticDependencyNotVisible, dependency.dependency_uuid);
    }
    if (dependency_object->invalidated || dependency_object->lifecycle_state == "unloaded" ||
        dependency_object->lifecycle_state == "quiescing") {
      return ExecDiagnostic(kExecutableObjectDiagnosticDependencyInvalidated, dependency.dependency_uuid);
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic AppendDependencyRecords(const EngineApiRequest& request,
                                            const EngineUuid& source_uuid,
                                            const std::string& source_kind,
                                            std::uint64_t dependency_generation,
                                            std::uint64_t metadata_epoch) {
  for (const auto& related : request.related_objects) {
    if (related.uuid.is_nil()) { continue; }
    EngineExecutableDependencyRecord dependency;
    dependency.creator_tx = request.context.local_transaction_id;
    dependency.source_uuid = source_uuid;
    dependency.source_kind = source_kind;
    dependency.dependency_uuid = related.uuid;
    dependency.dependency_kind = related.object_kind.empty() ? "executable_object" : LowerAscii(related.object_kind);
    dependency.dependency_generation = dependency_generation;
    dependency.metadata_epoch = metadata_epoch;
    const auto appended = AppendEvent(request.context, DependencyEvent(dependency));
    if (appended.error) { return appended; }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic RetireDependencyRecords(const EngineRequestContext& context,
                                            const EngineExecutableObjectLifecycleState& state,
                                            const EngineUuid& source_uuid,
                                            std::uint64_t dependency_generation,
                                            std::uint64_t metadata_epoch) {
  for (auto dependency : DependenciesForSource(state, source_uuid)) {
    dependency.creator_tx = context.local_transaction_id;
    dependency.dependency_generation = dependency_generation;
    dependency.metadata_epoch = metadata_epoch;
    dependency.deleted = true;
    const auto appended = AppendEvent(context, DependencyEvent(dependency));
    if (appended.error) { return appended; }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic InvalidateDependents(const EngineRequestContext& context,
                                         const EngineExecutableObjectLifecycleState& state,
                                         const EngineUuid& changed_uuid,
                                         std::uint64_t dependency_generation,
                                         std::uint64_t metadata_epoch) {
  for (const auto& dependent : DependentsOf(state, changed_uuid)) {
    const auto appended = AppendEvent(context,
                                      InvalidationEvent(context.local_transaction_id,
                                                        dependent.source_uuid,
                                                        changed_uuid,
                                                        dependency_generation,
                                                        metadata_epoch));
    if (appended.error) { return appended; }
  }
  return OkDiagnostic();
}

void FillObjectResult(EngineExecutableObjectLifecycleResult* result,
                      const EngineRequestContext& context,
                      const EngineExecutableObjectRecord& object,
                      std::uint64_t active_invocation_count) {
  result->primary_object.uuid = object.object_uuid;
  result->primary_object.object_kind = object.object_kind;
  result->bound_object_identity.object_uuid = object.object_uuid;
  result->bound_object_identity.resolved_object_type = object.object_kind;
  result->bound_object_identity.resolved_schema_uuid = object.schema_uuid;
  result->bound_object_identity.parent_object_uuid = object.package_uuid;
  result->bound_object_identity.catalog_generation_id = object.metadata_epoch;
  result->bound_object_identity.security_epoch = context.security_epoch;
  result->bound_object_identity.resource_epoch = context.resource_epoch;
  result->catalog_row_uuid = object.catalog_row_uuid;
  result->metadata_cache_epoch = object.metadata_epoch;
  result->executable_generation = object.executable_generation;
  result->active_invocation_count = active_invocation_count;
  AddEvidence(result, "executable_generation", std::to_string(object.executable_generation));
  AddEvidence(result,"metadata_cache_invalidation_object",object.object_uuid);
  AddEvidence(result,"metadata_cache_invalidation_epoch",std::to_string(object.metadata_epoch));
  if (object.executor_kind == "sblr") {
    AddEvidence(result, "stored_sblr", "hash_and_provenance_recorded");
  } else if (object.executor_kind == "internal_procedure") {
    AddEvidence(result, "internal_procedure_authority", object.internal_procedure_id);
  } else {
    AddEvidence(result, "metadata_only", "executable_descriptor_created_without_runtime_body");
  }
  AddEvidence(result, "execution_boundary", "engine_sblr_or_internal_procedure_only");
  AddRow(result, {{"object_uuid", object.object_uuid},
                  {"object_kind", object.object_kind},
                  {"schema_uuid", object.schema_uuid},
                  {"package_uuid", object.package_uuid},
                  {"lifecycle_state", object.lifecycle_state},
                  {"executable_generation", std::to_string(object.executable_generation)},
                  {"metadata_epoch", std::to_string(object.metadata_epoch)},
                  {"executor_kind", object.executor_kind},
                  {"stored_sblr_hash", object.stored_sblr_hash},
                  {"stored_sblr_provenance", object.stored_sblr_provenance},
                  {"internal_procedure_id", object.internal_procedure_id},
                  {"side_effect_class", object.side_effect_class},
                  {"event_trigger_event", object.event_trigger_event},
                  {"invalidated", object.invalidated ? "true" : "false"},
                  {"active_invocation_count", std::to_string(active_invocation_count)}});
}

EngineUuid RequestedLeaseUuid(const EngineApiRequest& request,
                               const EngineUuid& object_uuid,
                               std::uint64_t active_count) {
  const auto lease=OptionValue(request,"invocation_lease_uuid:");
  if(!lease.empty())return BinaryViewUuid(lease);
  return GenerateCrudEngineUuid("object");
}

EngineApiDiagnostic ValidatePreparedMetadataRequiredVersion(
    const EngineRequestContext& context,
    const EngineExecutableObjectRecord& object) {
  if (!context.statement_metadata_snapshot_engine_owned) {
    return OkDiagnostic();
  }
  if (context.statement_metadata_snapshot_uuid.is_nil() ||
      context.prepared_metadata_required_object_uuid.is_nil() ||
      context.prepared_metadata_required_executable_generation == 0 ||
      context.prepared_metadata_required_metadata_epoch == 0) {
    return ExecDiagnostic(
        kExecutableObjectDiagnosticPreparedMetadataVersionMismatch,
        "engine_owned_binding_missing_exact_metadata_identity");
  }
  if (object.object_uuid !=
      context.prepared_metadata_required_object_uuid) {
    return ExecDiagnostic(
        kExecutableObjectDiagnosticPreparedMetadataVersionMismatch,
        "prepared_metadata_object_uuid_mismatch");
  }
  if (object.executable_generation !=
      context.prepared_metadata_required_executable_generation) {
    return ExecDiagnostic(
        kExecutableObjectDiagnosticPreparedMetadataVersionMismatch,
        "executable_generation:" +
            std::to_string(object.executable_generation) + ":required:" +
            std::to_string(
                context.prepared_metadata_required_executable_generation));
  }
  if (object.metadata_epoch !=
      context.prepared_metadata_required_metadata_epoch) {
    return ExecDiagnostic(
        kExecutableObjectDiagnosticPreparedMetadataVersionMismatch,
        "metadata_epoch:" + std::to_string(object.metadata_epoch) +
            ":required:" +
            std::to_string(context.prepared_metadata_required_metadata_epoch));
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateInvocationReadiness(const EngineApiRequest& request,
                                                const EngineExecutableObjectLifecycleState& state,
                                                const EngineExecutableObjectRecord& object) {
  if (!HasInvokePermission(request, object.object_uuid)) {
    return ExecDiagnostic(kExecutableObjectDiagnosticPermissionDenied, "invoke_executable");
  }
  if (RequestsExecutionBoundaryBypass(request)) {
    return ExecDiagnostic(kExecutableObjectDiagnosticExecutionBoundaryRefused,
                          "engine_accepts_stored_sblr_or_internal_procedure_invocation_only");
  }
  const auto prepared_metadata =
      ValidatePreparedMetadataRequiredVersion(request.context, object);
  if (prepared_metadata.error) { return prepared_metadata; }
  if (object.lifecycle_state == "quiescing") {
    return ExecDiagnostic(kExecutableObjectDiagnosticQuiescing, object.object_uuid);
  }
  if (object.lifecycle_state == "unloaded") {
    return ExecDiagnostic(kExecutableObjectDiagnosticUnloaded, object.object_uuid);
  }
  if (object.invalidated) {
    return ExecDiagnostic(kExecutableObjectDiagnosticDependencyInvalidated,
                          object.invalidation_reason_uuid.is_nil() ? object.object_uuid
                                                                  : object.invalidation_reason_uuid);
  }
  if (object.executor_kind == "metadata_only") {
    return ExecDiagnostic(kExecutableObjectDiagnosticStoredSblrRequired,
                          "metadata_only_executable_descriptor");
  }
  const std::string compiled_descriptor =
      PayloadFieldValue(object.payload, "compiled_body_descriptor:");
  if (IsRoutineDeleteColumnRangeCountDescriptor(compiled_descriptor)) {
    const auto selector = ValidateExactMgaSelector(request.context);
    if (selector.error) { return selector; }
  }
  const auto dependencies = ValidateExecutableDependencies(state, object);
  if (dependencies.error) { return dependencies; }
  if (!SideEffectAllowed(request, object.side_effect_class)) {
    return ExecDiagnostic(kExecutableObjectDiagnosticSideEffectPolicyDenied, object.side_effect_class);
  }
  return OkDiagnostic();
}

EngineTypedValue ScalarValue(std::string type_name, std::string value) {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(type_name);
  return EngineTypedValue(std::move(descriptor), std::move(value));
}

EngineUuid FindVisibleTableUuidByName(const RelationReadSnapshot& state,
                                       const EngineRequestContext& context,
                                       const std::string& table_name) {
  const auto target = LowerAscii(table_name);
  for (const auto& table : state.tables) {
    if (!CrudCreatorVisible(state,
                            table.creator_tx,
                            table.event_sequence,
                            context.local_transaction_id)) {
      continue;
    }
    if (LowerAscii(table.default_name) == target) { return table.table_uuid; }
  }
  return {};
}

std::int64_t ParseI64(const std::string& value, std::int64_t fallback = 0) {
  try {
    return std::stoll(value);
  } catch (...) {
    return fallback;
  }
}

struct ProcessTaskRow {
  std::int64_t task_id = 0;
  std::string label;
  std::int64_t priority = 0;
};

EngineApiDiagnostic ExecuteProcessTasksProcedure(const EngineInvokeExecutableObjectRequest& request,
                                                 EngineApiResult* evidence_result) {
  const auto loaded = LoadMgaRelationStoreState(request.context);
  if (!loaded.ok) { return loaded.diagnostic; }
  RelationReadSnapshot state = BuildCrudCompatibilityStateFromMga(loaded.state);
  const EngineUuid task_table_uuid =
      FindVisibleTableUuidByName(state, request.context, "proc_tasks");
  const EngineUuid result_table_uuid =
      FindVisibleTableUuidByName(state, request.context, "proc_results");
  if (task_table_uuid.is_nil() || result_table_uuid.is_nil()) {
    return ExecDiagnostic(kExecutableObjectDiagnosticDependencyNotVisible,
                          task_table_uuid.is_nil() ? "proc_tasks" : "proc_results");
  }
  const auto threshold = ParseI64(OptionValue(request, "routine_argument_0_value:"), 0);
  if (threshold < 0) {
    return ExecDiagnostic(kExecutableObjectDiagnosticExecutionBoundaryRefused,
                          "proc_process_tasks_negative_threshold_signalled");
  }
  std::vector<ProcessTaskRow> candidates;
  for (const auto& row : VisibleCrudRowsForContext(state, task_table_uuid, request.context)) {
    const auto priority = ParseI64(CrudFieldValue(row.values, "priority"), 0);
    if (priority < threshold) { continue; }
    ProcessTaskRow task;
    task.task_id = ParseI64(CrudFieldValue(row.values, "task_id"), 0);
    task.label = CrudFieldValue(row.values, "task_label");
    task.priority = priority;
    candidates.push_back(std::move(task));
  }
  std::stable_sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
    if (left.priority != right.priority) { return left.priority > right.priority; }
    return left.task_id < right.task_id;
  });
  std::size_t next_result_id =
      VisibleCrudRowsForContext(state, result_table_uuid, request.context).size() + 1;
  std::vector<EngineRowValue> result_rows;
  result_rows.reserve(candidates.size());
  for (const auto& task : candidates) {
    EngineRowValue row;
    row.requested_row_uuid =
        GenerateCrudEngineUuid("row");
    row.fields.push_back({"result_id", ScalarValue("integer", std::to_string(next_result_id++))});
    row.fields.push_back({"task_id", ScalarValue("integer", std::to_string(task.task_id))});
    row.fields.push_back({"result_text",
                          ScalarValue("varchar",
                                      std::string(task.priority >= 2 ? "high:" : "normal:") +
                                          task.label)});
    result_rows.push_back(std::move(row));
  }
  if (result_rows.empty()) {
    AddEvidence(evidence_result, "internal_procedure_rows_inserted", "0");
    return OkDiagnostic();
  }
  EngineInsertRowsRequest insert;
  insert.context = request.context;
  insert.target_table.uuid = result_table_uuid;
  insert.target_table.object_kind = "table";
  insert.input_rows = std::move(result_rows);
  insert.require_generated_row_uuid = true;
  insert.option_envelopes.push_back("trigger_runtime_internal:true");
  insert.option_envelopes.push_back("policy:executable.side_effect:allow");
  const auto inserted = EngineInsertRows(insert);
  if (!inserted.ok) {
    if (!inserted.diagnostics.empty()) { return inserted.diagnostics.front(); }
    return ExecDiagnostic(kExecutableObjectDiagnosticExecutionBoundaryRefused,
                          "proc_process_tasks_insert_failed");
  }
  AddEvidence(evidence_result,
              "internal_procedure_rows_inserted",
              std::to_string(inserted.inserted_count));
  return OkDiagnostic();
}

EngineApiDiagnostic ExecuteDeleteColumnRangeCountProcedure(
    const EngineInvokeExecutableObjectRequest& request,
    const std::string& encoded_descriptor,
    EngineApiResult* body_result) {
  if (body_result == nullptr) {
    return ExecDiagnostic(kExecutableObjectDiagnosticRoutineDescriptorInvalid,
                          "routine_body_result_required");
  }
  RoutineDeleteColumnRangeCountDescriptor descriptor;
  const auto decoded = ParseRoutineDeleteColumnRangeCountDescriptor(
      encoded_descriptor, &descriptor);
  if (decoded.error) { return decoded; }
  if (OptionValue(request, "routine_argument_count:") != "2") {
    return ExecDiagnostic(kExecutableObjectDiagnosticRoutineArgumentInvalid,
                          "two_integer_argument_slots_required");
  }
  std::array<std::int64_t, 2> arguments{};
  for (std::size_t slot = 0; slot < arguments.size(); ++slot) {
    const std::string prefix = "routine_argument_" + std::to_string(slot);
    const std::string type = OptionValue(request, prefix + "_type:");
    if (!type.empty() && !IsRoutineIntegerType(type)) {
      return ExecDiagnostic(kExecutableObjectDiagnosticRoutineArgumentInvalid,
                            prefix + "_type_must_be_integer");
    }
    const auto value =
        ParseRoutineInteger(OptionValue(request, prefix + "_value:"));
    if (!value) {
      return ExecDiagnostic(kExecutableObjectDiagnosticRoutineArgumentInvalid,
                            prefix + "_value_must_be_integer");
    }
    arguments[slot] = *value;
  }

  std::string column_name;
  const auto binding =
      ResolveRoutineColumnBinding(request.context, descriptor, &column_name);
  if (binding.error) { return binding; }

  EngineDeleteRowsRequest delete_request;
  delete_request.context = request.context;
  delete_request.operation_id = "dml.delete_rows";
  delete_request.target_table.uuid = descriptor.table_uuid;
  delete_request.target_table.object_kind = "table";
  // The routine body invokes the canonical DELETE API; its compiled-routine
  // origin is carried as evidence below, not as new compatibility-specific DML
  // surface variant.
  delete_request.delete_surface_variant = "delete";
  delete_request.delete_predicate.predicate_kind = "column_range";
  delete_request.delete_predicate.canonical_predicate_envelope = column_name;
  delete_request.delete_predicate.bound_values.push_back(
      ScalarValue("integer", std::to_string(arguments[descriptor.lower_input_slot])));
  delete_request.delete_predicate.bound_values.push_back(
      ScalarValue("integer", std::to_string(arguments[descriptor.upper_input_slot])));
  delete_request.option_envelopes.push_back(
      "policy:executable.side_effect:allow");
  const auto deleted = EngineDeleteRows(delete_request);
  if (!deleted.ok) {
    if (!deleted.diagnostics.empty()) { return deleted.diagnostics.front(); }
    return ExecDiagnostic(kExecutableObjectDiagnosticExecutionBoundaryRefused,
                          "compiled_routine_delete_failed");
  }

  EngineDescriptor output_descriptor;
  output_descriptor.descriptor_kind = "routine_output";
  output_descriptor.canonical_type_name = "integer";
  output_descriptor.encoded_descriptor =
      "mode=out;slot=" +
      std::to_string(descriptor.affected_rows_output_slot) +
      ";source=authoritative_deleted_count";
  body_result->result_shape.result_kind = "routine.procedure.result.v1";
  body_result->result_shape.columns.push_back(output_descriptor);
  EngineRowValue yielded;
  yielded.fields.push_back(
      {"routine_output_slot_" + std::to_string(descriptor.yield_output_slot),
       ScalarValue("integer", std::to_string(deleted.deleted_count))});
  body_result->result_shape.rows.push_back(std::move(yielded));
  body_result->dml_summary = deleted.dml_summary;
  body_result->evidence.insert(body_result->evidence.end(),
                               deleted.evidence.begin(),
                               deleted.evidence.end());
  AddEvidence(body_result,
              "routine_instruction",
              "delete.uuid_bound.column_range");
  AddEvidence(body_result, "routine_target_table_uuid", descriptor.table_uuid);
  AddEvidence(body_result, "routine_target_column_uuid", descriptor.column_uuid);
  AddEvidence(body_result,
              "routine_affected_rows_output_slot",
              std::to_string(descriptor.affected_rows_output_slot) + ":" +
                  std::to_string(deleted.deleted_count));
  AddEvidence(body_result,
              "routine_yielded_output_rows",
              std::to_string(body_result->result_shape.rows.size()));
  return OkDiagnostic();
}

EngineApiDiagnostic ExecuteInternalProcedureDescriptor(
    const EngineInvokeExecutableObjectRequest& request,
    const EngineExecutableObjectRecord& object,
    EngineApiResult* evidence_result) {
  const std::string descriptor =
      PayloadFieldValue(object.payload, "compiled_body_descriptor:");
  if (descriptor == "sblr.psql.body.null.v1") {
    const auto body_uuid =
        BinaryViewUuid(PayloadFieldValue(object.payload, "procedure_body_sblr_uuid:"));
    const auto body_generation = ParseU64(PayloadFieldValue(
        object.payload, "procedure_body_sblr_generation:"));
    const auto body_hex =
        PayloadFieldValue(object.payload, "procedure_body_bytes:");
    const auto body_sha =
        PayloadFieldValue(object.payload, "procedure_body_sha256:");
    const auto abi_uuid =
        BinaryViewUuid(PayloadFieldValue(object.payload, "procedure_abi_uuid:"));
    const auto abi_generation = ParseU64(PayloadFieldValue(
        object.payload, "procedure_abi_generation:"));
    const auto abi_bytes_hex =
        PayloadFieldValue(object.payload, "procedure_abi_bytes:");
    const auto abi_evidence =
        PayloadFieldValue(object.payload, "procedure_abi_evidence_sha256:");
    const auto parameter_count = ParseU64(PayloadFieldValue(
        object.payload, "procedure_parameter_count:"));
    const auto signature_sha = PayloadFieldValue(
        object.payload, "procedure_signature_sha256:");
    const auto effect_sha = PayloadFieldValue(
        object.payload, "procedure_effect_set_sha256:");
    const auto recovery_uuid =
        BinaryViewUuid(PayloadFieldValue(object.payload, "procedure_recovery_uuid:"));
    const auto mutation_uuid =
        BinaryViewUuid(PayloadFieldValue(object.payload, "procedure_mutation_uuid:"));
    const auto publication_barrier_uuid = BinaryViewUuid(PayloadFieldValue(
        object.payload, "procedure_publication_barrier_uuid:"));
    const auto& decoded_bytes = body_hex;
    scratchbird::engine::sblr::SblrProceduralBodyV1 body;
    std::string body_detail;
    const auto parsed_body_uuid = scratchbird::core::uuid::MakeTypedUuid(core::platform::UuidKind::object,body_uuid);
    const auto parsed_procedure_uuid =
        scratchbird::core::uuid::MakeTypedUuid(core::platform::UuidKind::object,object.object_uuid);
    if (!parsed_body_uuid.ok() || !parsed_procedure_uuid.ok() ||
        body_generation == 0 || body_hex.empty() || decoded_bytes.empty() ||
        body_sha.empty() || abi_uuid.is_nil() ||
        abi_generation == 0 || abi_bytes_hex.empty() || abi_evidence.empty() ||
        parameter_count > 1 || signature_sha.empty() || effect_sha.empty() ||
        recovery_uuid.is_nil() ||
        mutation_uuid.is_nil() ||
        publication_barrier_uuid.is_nil() ||
        !scratchbird::engine::sblr::DecodeSblrProceduralBodyV1(
            reinterpret_cast<const std::uint8_t*>(decoded_bytes.data()),
            decoded_bytes.size(), &body, &body_detail) ||
        !std::equal(body.body_uuid.begin(), body.body_uuid.end(),
                    parsed_body_uuid.value.value.bytes.begin()) ||
        !std::equal(body.procedure_uuid.begin(), body.procedure_uuid.end(),
                    parsed_procedure_uuid.value.value.bytes.begin()) ||
        body.body_generation != body_generation ||
        body.procedure_generation != object.executable_generation ||
        scratchbird::core::hash::HexLower(body.effect_set_sha256) !=
            effect_sha) {
      return ExecDiagnostic(kExecutableObjectDiagnosticRoutineDescriptorInvalid,
                            body_detail.empty()
                                ? "procedural_null_body_authority_invalid"
                                : body_detail);
    }
    const std::vector<std::uint8_t> canonical_bytes(decoded_bytes.begin(),
                                                     decoded_bytes.end());
    const auto actual_sha = scratchbird::core::hash::HexLower(
        scratchbird::core::hash::ComputeSha256Digest(canonical_bytes).digest);
    if (actual_sha != body_sha ||
        object.stored_sblr_hash != "sha256:" + actual_sha ||
        object.stored_sblr_provenance !=
            "engine.bound.procedural_body.v1" ||
        object.side_effect_class != "none") {
      return ExecDiagnostic(kExecutableObjectDiagnosticRoutineDescriptorInvalid,
                            "procedural_null_body_persistence_mismatch");
    }
    const auto& persisted_abi_text = abi_bytes_hex;
    const std::vector<std::uint8_t> persisted_abi_bytes(
        persisted_abi_text.begin(), persisted_abi_text.end());
    scratchbird::engine::sblr::SblrProcedureAbiV1 persisted_abi;
    scratchbird::engine::sblr::SblrProcedureAbiV1 invocation_abi;
    scratchbird::engine::sblr::SblrProcedureArgumentVectorV1 arguments;
    std::string abi_detail;
    std::string argument_detail;
    if (persisted_abi_bytes.size() !=
            scratchbird::engine::sblr::kSblrProcedureAbiV1Bytes ||
        request.canonical_procedure_abi_bytes != persisted_abi_bytes ||
        !scratchbird::engine::sblr::DecodeSblrProcedureAbiV1(
            persisted_abi_bytes.data(), persisted_abi_bytes.size(),
            &persisted_abi, &abi_detail) ||
        !scratchbird::engine::sblr::DecodeSblrProcedureAbiV1(
            request.canonical_procedure_abi_bytes.data(),
            request.canonical_procedure_abi_bytes.size(), &invocation_abi,
            &abi_detail) ||
        persisted_abi.evidence != invocation_abi.evidence ||
        scratchbird::core::hash::HexLower(persisted_abi.evidence) !=
            abi_evidence ||
        EngineUuid{persisted_abi.abi_uuid} !=
            abi_uuid ||
        persisted_abi.abi_generation != abi_generation ||
        persisted_abi.procedure_generation != object.executable_generation ||
        persisted_abi.parameters.size() != parameter_count ||
        !std::equal(persisted_abi.procedure_uuid.begin(),
                    persisted_abi.procedure_uuid.end(),
                    parsed_procedure_uuid.value.value.bytes.begin()) ||
        request.canonical_argument_vector_bytes.size() !=
            scratchbird::engine::sblr::kSblrProcedureArgumentVectorV1Bytes ||
        !scratchbird::engine::sblr::DecodeSblrProcedureArgumentVectorV1(
            request.canonical_argument_vector_bytes.data(),
            request.canonical_argument_vector_bytes.size(), &arguments,
            &argument_detail) ||
        arguments.procedure_abi_uuid != persisted_abi.abi_uuid ||
        arguments.procedure_abi_generation != persisted_abi.abi_generation ||
        arguments.argument_vector_generation == 0 ||
        arguments.invocation_generation != 1 ||
        arguments.arguments.size() != persisted_abi.parameters.size() ||
        EngineUuid{arguments.invocation_uuid} !=
            BinaryViewUuid(OptionValue(request, "invocation_lease_uuid:"))) {
      return ExecDiagnostic(
          kExecutableObjectDiagnosticRoutineDescriptorInvalid,
          !argument_detail.empty() ? argument_detail
                                   : (!abi_detail.empty()
                                          ? abi_detail
                                          : "procedure_abi_or_argument_authority_mismatch"));
    }
    std::optional<std::int64_t> canonical_argument;
    if (parameter_count == 1) {
      const auto& parameter = persisted_abi.parameters.front();
      const auto& argument = arguments.arguments.front();
      std::int64_t value = 0;
      if (argument.ordinal != parameter.ordinal ||
          argument.datatype_descriptor_uuid !=
              parameter.datatype_descriptor_uuid ||
          argument.datatype_descriptor_generation !=
              parameter.datatype_descriptor_generation ||
          argument.type_uuid != parameter.type_uuid ||
          !scratchbird::engine::sblr::DecodeSblrProcedureInt64Value(
              argument.canonical_value_bytes, &value)) {
        return ExecDiagnostic(
            kExecutableObjectDiagnosticRoutineDescriptorInvalid,
            "procedure_argument_descriptor_or_value_mismatch");
      }
      canonical_argument = value;
    }
    if (request.context.query_cancellation_requested &&
        request.context.query_cancellation_requested()) {
      return MakeEngineApiDiagnostic("PROCESS.CANCELLED",
                                     "procedure.invoke.cancelled_before_node",
                                     {}, true);
    }
    AddEvidence(evidence_result, "procedural_body_profile", "null_body_v1");
    AddEvidence(evidence_result, "procedural_node",
                "sblr.psql.node.psql_null_stmt.v1");
    AddEvidence(evidence_result, "procedural_node_result",
                "psql_ir_result.v1");
    AddEvidence(evidence_result, "procedural_body_uuid", body_uuid);
    AddEvidence(evidence_result, "procedure_abi_uuid", abi_uuid);
    AddEvidence(evidence_result, "procedure_parameter_count",
                std::to_string(parameter_count));
    if (canonical_argument.has_value()) {
      AddEvidence(evidence_result, "procedure_argument_0_int64",
                  std::to_string(*canonical_argument));
    }
    AddEvidence(evidence_result, "procedure_signature_sha256", signature_sha);
    AddEvidence(evidence_result, "procedure_effect_set_sha256", effect_sha);
    AddEvidence(evidence_result, "procedure_recovery_uuid", recovery_uuid);
    AddEvidence(evidence_result, "procedure_mutation_uuid", mutation_uuid);
    AddEvidence(evidence_result, "procedure_publication_barrier_uuid",
                publication_barrier_uuid);
    evidence_result->result_shape.result_kind = "procedure_invocation_result";
    return OkDiagnostic();
  }
  if (descriptor == "sbsql.compiled.procedural.process_tasks.v1") {
    return ExecuteProcessTasksProcedure(request, evidence_result);
  }
  if (IsRoutineDeleteColumnRangeCountDescriptor(descriptor)) {
    return ExecuteDeleteColumnRangeCountProcedure(
        request, descriptor, evidence_result);
  }
  if (descriptor.empty()) { return OkDiagnostic(); }
  AddEvidence(evidence_result, "internal_procedure_descriptor_no_side_effect", descriptor);
  return OkDiagnostic();
}

EngineBeginExecutableObjectInvocationResult BeginInvocationWithOperation(
    const EngineBeginExecutableObjectInvocationRequest& request,
    const std::string& operation_id) {
  const auto context = ValidateContext(request, true, true);
  if (context.error) {
    return DiagnosticResult<EngineBeginExecutableObjectInvocationResult>(
        request.context, operation_id, context);
  }
  const EngineUuid object_uuid = ObjectUuid(request);
  if (object_uuid.is_nil()) {
    return DiagnosticResult<EngineBeginExecutableObjectInvocationResult>(
        request.context,
        operation_id,
        ExecDiagnostic(kExecutableObjectDiagnosticUuidRequired, "target_object.uuid"));
  }
  EngineExecutableObjectRecord object;
  EngineExecutableObjectLifecycleState state;
  const auto visible = FindVisibleObject(request, object_uuid, &object, &state);
  if (visible.error) {
    return DiagnosticResult<EngineBeginExecutableObjectInvocationResult>(
        request.context, operation_id, visible);
  }
  const auto readiness = ValidateInvocationReadiness(request, state, object);
  if (readiness.error) {
    return DiagnosticResult<EngineBeginExecutableObjectInvocationResult>(
        request.context, operation_id, readiness);
  }
  const std::uint64_t active_count = ActiveInvocationCount(state, object_uuid);
  EngineExecutableInvocationRecord invocation;
  invocation.creator_tx = request.context.local_transaction_id;
  invocation.invocation_lease_uuid = RequestedLeaseUuid(request, object_uuid, active_count);
  invocation.object_uuid = object_uuid;
  invocation.executable_generation = object.executable_generation;
  invocation.lifecycle_state = "active";
  invocation.metadata_epoch = state.metadata_epoch + 1;
  const auto appended = AppendEvent(request.context, InvocationEvent(invocation));
  if (appended.error) {
    return DiagnosticResult<EngineBeginExecutableObjectInvocationResult>(
        request.context, operation_id, appended);
  }

  object.metadata_epoch = invocation.metadata_epoch;
  auto result = SuccessResult<EngineBeginExecutableObjectInvocationResult>(request.context, operation_id);
  result.invocation_lease_uuid = invocation.invocation_lease_uuid;
  FillObjectResult(&result, request.context, object, active_count + 1);
  AddPreparedMetadataEvidence(request.context, &result);
  AddEvidence(&result, "invocation_lifecycle", "active_invocation_acquired");
  if (object.executor_kind == "sblr") {
    AddEvidence(&result, "sblr_authority", "stored_sblr_hash_verified");
  } else {
    AddEvidence(&result, "internal_procedure_authority", object.internal_procedure_id);
  }
  return result;
}

EngineFinishExecutableObjectInvocationResult FinishInvocationWithOperation(
    const EngineFinishExecutableObjectInvocationRequest& request,
    const std::string& operation_id) {
  const auto context = ValidateContext(request, true, true);
  if (context.error) {
    return DiagnosticResult<EngineFinishExecutableObjectInvocationResult>(
        request.context, operation_id, context);
  }
  const EngineUuid object_uuid = ObjectUuid(request);
  const EngineUuid lease_uuid = BinaryViewUuid(OptionValue(request, "invocation_lease_uuid:"));
  if (object_uuid.is_nil()) {
    return DiagnosticResult<EngineFinishExecutableObjectInvocationResult>(
        request.context,
        operation_id,
        ExecDiagnostic(kExecutableObjectDiagnosticUuidRequired, "target_object.uuid"));
  }
  if (lease_uuid.is_nil()) {
    return DiagnosticResult<EngineFinishExecutableObjectInvocationResult>(
        request.context,
        operation_id,
        ExecDiagnostic(kExecutableObjectDiagnosticInvocationNotFound, "invocation_lease_uuid"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineFinishExecutableObjectInvocationResult>(
        request.context, operation_id, loaded.diagnostic);
  }
  const auto* object = FindObject(loaded.state, object_uuid);
  if (object == nullptr) {
    return DiagnosticResult<EngineFinishExecutableObjectInvocationResult>(
        request.context,
        operation_id,
        ExecDiagnostic(kExecutableObjectDiagnosticNotFound, object_uuid));
  }
  const auto prepared_metadata =
      ValidatePreparedMetadataRequiredVersion(request.context, *object);
  if (prepared_metadata.error) {
    return DiagnosticResult<EngineFinishExecutableObjectInvocationResult>(
        request.context, operation_id, prepared_metadata);
  }
  bool found = false;
  for (const auto& invocation : loaded.state.active_invocations) {
    if (invocation.invocation_lease_uuid == lease_uuid && invocation.object_uuid == object_uuid) {
      found = true;
      break;
    }
  }
  if (!found) {
    return DiagnosticResult<EngineFinishExecutableObjectInvocationResult>(
        request.context,
        operation_id,
        ExecDiagnostic(kExecutableObjectDiagnosticInvocationNotFound, lease_uuid));
  }
  EngineExecutableInvocationRecord release;
  release.creator_tx = request.context.local_transaction_id;
  release.invocation_lease_uuid = lease_uuid;
  release.object_uuid = object_uuid;
  release.executable_generation = object->executable_generation;
  release.lifecycle_state = "released";
  release.metadata_epoch = loaded.state.metadata_epoch + 1;
  const auto appended = AppendEvent(request.context, InvocationEvent(release));
  if (appended.error) {
    return DiagnosticResult<EngineFinishExecutableObjectInvocationResult>(
        request.context, operation_id, appended);
  }
  auto result = SuccessResult<EngineFinishExecutableObjectInvocationResult>(request.context, operation_id);
  result.invocation_lease_uuid = lease_uuid;
  auto output_object = *object;
  output_object.metadata_epoch = release.metadata_epoch;
  FillObjectResult(&result,
                   request.context,
                   output_object,
                   ActiveInvocationCount(loaded.state, object_uuid) - 1);
  AddPreparedMetadataEvidence(request.context, &result);
  AddEvidence(&result, "invocation_lifecycle", "active_invocation_released");
  return result;
}

EngineApiDiagnostic PreflightCreateExecutableObjectImpl(
    const EngineCreateExecutableObjectRequest& request) {
  const EngineUuid object_uuid = ObjectUuid(request);
  const std::string object_kind = ObjectKind(request);
  if (object_kind.empty()) {
    return ExecDiagnostic(kExecutableObjectDiagnosticKindRequired,
                          "target_object.object_kind");
  }
  const auto authority = ValidateManageAuthority(request, object_kind);
  if (authority.error) { return authority; }
  if (object_uuid.is_nil()) {
    return ExecDiagnostic(kExecutableObjectDiagnosticUuidRequired,
                          "target_object.uuid");
  }
  if (!ValidObjectKind(object_kind)) {
    return ExecDiagnostic(kExecutableObjectDiagnosticUnsupportedKind,
                          object_kind);
  }
  if (object_kind != "event_trigger" &&
      request.target_schema.uuid.is_nil()) {
    return ExecDiagnostic(kExecutableObjectDiagnosticSchemaUuidRequired,
                          "target_schema.uuid");
  }
  const bool create_or_alter = CreateOrAlterProcedureRequested(request);
  if (create_or_alter) {
    if (object_kind != "procedure") {
      return ExecDiagnostic(kExecutableObjectDiagnosticRoutineDescriptorInvalid,
                            "create_or_alter_route_requires_procedure");
    }
    const auto selector = ValidateExactMgaSelector(request.context);
    if (selector.error) { return selector; }
  }

  std::string executor_kind;
  std::string stored_sblr_hash;
  std::string stored_sblr_provenance;
  std::string internal_procedure_id;
  std::string side_effect_class;
  std::string event_trigger_event;
  const auto stored = ValidateStoredProgram(request,
                                            object_kind,
                                            nullptr,
                                            &executor_kind,
                                            &stored_sblr_hash,
                                            &stored_sblr_provenance,
                                            &internal_procedure_id,
                                            &side_effect_class,
                                            &event_trigger_event);
  if (stored.error) { return stored; }

  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) { return loaded.diagnostic; }
  if (FindObject(loaded.state, object_uuid) != nullptr) {
    return create_or_alter
               ? OkDiagnostic()
               : ExecDiagnostic(kExecutableObjectDiagnosticDuplicate,
                                object_uuid);
  }
  if (create_or_alter &&
      !request.create_or_alter_identity_engine_allocated) {
    return ExecDiagnostic(kExecutableObjectDiagnosticRoutineDescriptorInvalid,
                          "create_or_alter_identity_must_be_engine_allocated");
  }
  return ValidateRelatedObjects(loaded.state, request);
}

}  // namespace

EngineLoadExecutableObjectLifecycleStateResult LoadExecutableObjectLifecycleState(
    const EngineRequestContext& context) {
  return LoadState(context, {.enforce_visibility = true});
}

EngineLoadExecutableObjectLifecycleStateResult LoadExecutableObjectLifecycleStateForRuntimeDispatch(
    const EngineRequestContext& context) {
  return LoadState(context, {.enforce_visibility = false});
}

EngineApiDiagnostic ValidateExecutableObjectExactMgaSelector(
    const EngineRequestContext& context) {
  return ValidateExactMgaSelector(context);
}

EngineApiDiagnostic PreflightCreateExecutableObject(
    const EngineCreateExecutableObjectRequest& request) {
  return PreflightCreateExecutableObjectImpl(request);
}

EngineCreateExecutableObjectResult EngineCreateExecutableObject(
    const EngineCreateExecutableObjectRequest& request) {
  const EngineUuid object_uuid = ObjectUuid(request);
  const std::string object_kind = ObjectKind(request);
  if (object_kind.empty()) {
    return DiagnosticResult<EngineCreateExecutableObjectResult>(
        request.context,
        kOperationCreate,
        ExecDiagnostic(kExecutableObjectDiagnosticKindRequired, "target_object.object_kind"));
  }
  const auto authority = ValidateManageAuthority(request, object_kind);
  if (authority.error) {
    return DiagnosticResult<EngineCreateExecutableObjectResult>(
        request.context, kOperationCreate, authority);
  }
  if (object_uuid.is_nil()) {
    return DiagnosticResult<EngineCreateExecutableObjectResult>(
        request.context,
        kOperationCreate,
        ExecDiagnostic(kExecutableObjectDiagnosticUuidRequired, "target_object.uuid"));
  }
  if (!ValidObjectKind(object_kind)) {
    return DiagnosticResult<EngineCreateExecutableObjectResult>(
        request.context,
        kOperationCreate,
        ExecDiagnostic(kExecutableObjectDiagnosticUnsupportedKind, object_kind));
  }
  if (object_kind != "event_trigger" && request.target_schema.uuid.is_nil()) {
    return DiagnosticResult<EngineCreateExecutableObjectResult>(
        request.context,
        kOperationCreate,
        ExecDiagnostic(kExecutableObjectDiagnosticSchemaUuidRequired, "target_schema.uuid"));
  }
  const bool create_or_alter = CreateOrAlterProcedureRequested(request);
  if (create_or_alter) {
    if (object_kind != "procedure") {
      return DiagnosticResult<EngineCreateExecutableObjectResult>(
          request.context,
          kOperationCreate,
          ExecDiagnostic(kExecutableObjectDiagnosticRoutineDescriptorInvalid,
                         "create_or_alter_route_requires_procedure"));
    }
    const auto selector = ValidateExactMgaSelector(request.context);
    if (selector.error) {
      return DiagnosticResult<EngineCreateExecutableObjectResult>(
          request.context, kOperationCreate, selector);
    }
  }
  std::string executor_kind;
  std::string stored_sblr_hash;
  std::string stored_sblr_provenance;
  std::string internal_procedure_id;
  std::string side_effect_class;
  std::string event_trigger_event;
  const auto stored = ValidateStoredProgram(request,
                                            object_kind,
                                            nullptr,
                                            &executor_kind,
                                            &stored_sblr_hash,
                                            &stored_sblr_provenance,
                                            &internal_procedure_id,
                                            &side_effect_class,
                                            &event_trigger_event);
  if (stored.error) {
    return DiagnosticResult<EngineCreateExecutableObjectResult>(request.context, kOperationCreate, stored);
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineCreateExecutableObjectResult>(
        request.context, kOperationCreate, loaded.diagnostic);
  }
  if (FindObject(loaded.state, object_uuid) != nullptr && create_or_alter) {
    EngineAlterExecutableObjectRequest alter;
    static_cast<EngineApiRequest&>(alter) = request;
    auto altered = EngineAlterExecutableObject(alter);
    if (altered.ok) {
      AddEvidence(&altered, "create_or_alter_resolution", "alter");
      AddEvidence(&altered, "create_or_alter_authority", "engine_exact_mga");
    }
    return altered;
  }
  if (create_or_alter &&
      !request.create_or_alter_identity_engine_allocated) {
    return DiagnosticResult<EngineCreateExecutableObjectResult>(
        request.context,
        kOperationCreate,
        ExecDiagnostic(kExecutableObjectDiagnosticRoutineDescriptorInvalid,
                       "create_or_alter_identity_must_be_engine_allocated"));
  }
  if (FindObject(loaded.state, object_uuid) != nullptr) {
    return DiagnosticResult<EngineCreateExecutableObjectResult>(
        request.context,
        kOperationCreate,
        ExecDiagnostic(kExecutableObjectDiagnosticDuplicate, object_uuid));
  }
  const auto dependencies = ValidateRelatedObjects(loaded.state, request);
  if (dependencies.error) {
    return DiagnosticResult<EngineCreateExecutableObjectResult>(
        request.context, kOperationCreate, dependencies);
  }

  const std::uint64_t epoch = loaded.state.metadata_epoch + 1;
  const std::uint64_t dependency_generation = loaded.state.dependency_generation + 1;
  EngineExecutableObjectRecord record;
  record.creator_tx = request.context.local_transaction_id;
  record.object_uuid = object_uuid;
  record.catalog_row_uuid = GenerateCrudEngineUuid("row");
  record.object_kind = object_kind;
  record.schema_uuid = request.target_schema.uuid;
  record.owner_principal_uuid = request.context.principal_uuid;
  record.package_uuid = PackageUuid(request);
  record.lifecycle_state = "active";
  record.executable_generation = 1;
  record.metadata_epoch = epoch;
  record.executor_kind = executor_kind;
  record.stored_sblr_hash = stored_sblr_hash;
  record.stored_sblr_provenance = stored_sblr_provenance;
  record.internal_procedure_id = internal_procedure_id;
  record.side_effect_class = side_effect_class;
  record.event_trigger_event = event_trigger_event;
  record.payload = PayloadFromRequest(request);

  auto appended = AppendEvent(request.context, ObjectEvent(record));
  if (appended.error) {
    return DiagnosticResult<EngineCreateExecutableObjectResult>(request.context, kOperationCreate, appended);
  }
  appended = AppendDependencyRecords(request, object_uuid, object_kind, dependency_generation, epoch);
  if (appended.error) {
    return DiagnosticResult<EngineCreateExecutableObjectResult>(request.context, kOperationCreate, appended);
  }
  appended = AppendCacheInvalidation(request.context, kOperationCreate, object_uuid, epoch);
  if (appended.error) {
    return DiagnosticResult<EngineCreateExecutableObjectResult>(request.context, kOperationCreate, appended);
  }

  auto result = SuccessResult<EngineCreateExecutableObjectResult>(request.context, kOperationCreate);
  FillObjectResult(&result, request.context, record, 0);
  AddEvidence(&result, "permission_check", "manage_executable");
  AddEvidence(&result, "dependency_generation", std::to_string(dependency_generation));
  if (create_or_alter) {
    AddEvidence(&result, "create_or_alter_resolution", "create");
    AddEvidence(&result, "create_or_alter_authority", "engine_exact_mga");
  }
  if (object_kind == "event_trigger") {
    AddEvidence(&result, "event_trigger_boundary", "ddl_event_filter_registered");
  }
  return result;
}

EngineAlterExecutableObjectResult EngineAlterExecutableObject(
    const EngineAlterExecutableObjectRequest& request) {
  const EngineUuid object_uuid = ObjectUuid(request);
  const auto authority = ValidateManageAuthority(request, ObjectKind(request));
  if (authority.error) {
    return DiagnosticResult<EngineAlterExecutableObjectResult>(
        request.context, kOperationAlter, authority);
  }
  if (object_uuid.is_nil()) {
    return DiagnosticResult<EngineAlterExecutableObjectResult>(
        request.context,
        kOperationAlter,
        ExecDiagnostic(kExecutableObjectDiagnosticUuidRequired, "target_object.uuid"));
  }
  EngineExecutableObjectRecord existing;
  EngineExecutableObjectLifecycleState state;
  const auto visible = FindVisibleObject(request, object_uuid, &existing, &state);
  if (visible.error) {
    return DiagnosticResult<EngineAlterExecutableObjectResult>(
        request.context, kOperationAlter, visible);
  }
  std::string executor_kind;
  std::string stored_sblr_hash;
  std::string stored_sblr_provenance;
  std::string internal_procedure_id;
  std::string side_effect_class;
  std::string event_trigger_event;
  const auto stored = ValidateStoredProgram(request,
                                            existing.object_kind,
                                            &existing,
                                            &executor_kind,
                                            &stored_sblr_hash,
                                            &stored_sblr_provenance,
                                            &internal_procedure_id,
                                            &side_effect_class,
                                            &event_trigger_event);
  if (stored.error) {
    return DiagnosticResult<EngineAlterExecutableObjectResult>(request.context, kOperationAlter, stored);
  }
  const auto dependencies = ValidateRelatedObjects(state, request);
  if (dependencies.error) {
    return DiagnosticResult<EngineAlterExecutableObjectResult>(
        request.context, kOperationAlter, dependencies);
  }

  const std::uint64_t epoch = state.metadata_epoch + 1;
  const std::uint64_t dependency_generation = state.dependency_generation + 1;
  EngineExecutableObjectRecord replacement = existing;
  replacement.creator_tx = request.context.local_transaction_id;
  replacement.executable_generation = existing.executable_generation + 1;
  replacement.metadata_epoch = epoch;
  replacement.executor_kind = executor_kind;
  replacement.stored_sblr_hash = stored_sblr_hash;
  replacement.stored_sblr_provenance = stored_sblr_provenance;
  replacement.internal_procedure_id = internal_procedure_id;
  replacement.side_effect_class = side_effect_class;
  replacement.event_trigger_event = event_trigger_event;
  replacement.payload = PayloadFromRequest(request);
  replacement.invalidated = false;
  replacement.invalidated_generation = 0;
  replacement.invalidation_reason_uuid = {};

  auto appended = AppendEvent(request.context, ObjectEvent(replacement));
  if (appended.error) {
    return DiagnosticResult<EngineAlterExecutableObjectResult>(request.context, kOperationAlter, appended);
  }
  if (!request.related_objects.empty()) {
    appended = RetireDependencyRecords(request.context, state, object_uuid, dependency_generation, epoch);
    if (appended.error) {
      return DiagnosticResult<EngineAlterExecutableObjectResult>(request.context, kOperationAlter, appended);
    }
    appended = AppendDependencyRecords(request, object_uuid, existing.object_kind, dependency_generation, epoch);
    if (appended.error) {
      return DiagnosticResult<EngineAlterExecutableObjectResult>(request.context, kOperationAlter, appended);
    }
  }
  appended = InvalidateDependents(request.context, state, object_uuid, dependency_generation, epoch);
  if (appended.error) {
    return DiagnosticResult<EngineAlterExecutableObjectResult>(request.context, kOperationAlter, appended);
  }
  appended = AppendCacheInvalidation(request.context, kOperationAlter, object_uuid, epoch);
  if (appended.error) {
    return DiagnosticResult<EngineAlterExecutableObjectResult>(request.context, kOperationAlter, appended);
  }

  auto result = SuccessResult<EngineAlterExecutableObjectResult>(request.context, kOperationAlter);
  FillObjectResult(&result,
                   request.context,
                   replacement,
                   ActiveInvocationCount(state, object_uuid));
  AddEvidence(&result, "dependency_invalidation", object_uuid);
  AddEvidence(&result, "permission_check", "manage_executable");
  return result;
}

EngineDropExecutableObjectResult EngineDropExecutableObject(
    const EngineDropExecutableObjectRequest& request) {
  const EngineUuid object_uuid = ObjectUuid(request);
  const auto authority = ValidateManageAuthority(request, ObjectKind(request));
  if (authority.error) {
    return DiagnosticResult<EngineDropExecutableObjectResult>(
        request.context, kOperationDrop, authority);
  }
  if (object_uuid.is_nil()) {
    return DiagnosticResult<EngineDropExecutableObjectResult>(
        request.context,
        kOperationDrop,
        ExecDiagnostic(kExecutableObjectDiagnosticUuidRequired, "target_object.uuid"));
  }
  EngineExecutableObjectRecord existing;
  EngineExecutableObjectLifecycleState state;
  const auto visible = FindVisibleObject(request, object_uuid, &existing, &state);
  if (visible.error) {
    return DiagnosticResult<EngineDropExecutableObjectResult>(
        request.context, kOperationDrop, visible);
  }
  const auto active_count = ActiveInvocationCount(state, object_uuid);
  if (active_count != 0) {
    return DiagnosticResult<EngineDropExecutableObjectResult>(
        request.context,
        kOperationDrop,
        ExecDiagnostic(kExecutableObjectDiagnosticUnloadBlockedActiveInvocation, object_uuid));
  }
  const std::uint64_t epoch = state.metadata_epoch + 1;
  const std::uint64_t dependency_generation = state.dependency_generation + 1;
  auto dropped = existing;
  dropped.creator_tx = request.context.local_transaction_id;
  dropped.lifecycle_state = "dropped";
  dropped.executable_generation = existing.executable_generation + 1;
  dropped.metadata_epoch = epoch;
  dropped.deleted = true;

  auto appended = InvalidateDependents(request.context, state, object_uuid, dependency_generation, epoch);
  if (appended.error) {
    return DiagnosticResult<EngineDropExecutableObjectResult>(request.context, kOperationDrop, appended);
  }
  appended = RetireDependencyRecords(request.context, state, object_uuid, dependency_generation, epoch);
  if (appended.error) {
    return DiagnosticResult<EngineDropExecutableObjectResult>(request.context, kOperationDrop, appended);
  }
  appended = AppendEvent(request.context, ObjectEvent(dropped));
  if (appended.error) {
    return DiagnosticResult<EngineDropExecutableObjectResult>(request.context, kOperationDrop, appended);
  }
  appended = AppendCacheInvalidation(request.context, kOperationDrop, object_uuid, epoch);
  if (appended.error) {
    return DiagnosticResult<EngineDropExecutableObjectResult>(request.context, kOperationDrop, appended);
  }

  auto result = SuccessResult<EngineDropExecutableObjectResult>(request.context, kOperationDrop);
  dropped.deleted = false;
  FillObjectResult(&result, request.context, dropped, 0);
  AddEvidence(&result, "dependency_invalidation", object_uuid);
  AddEvidence(&result, "permission_check", "manage_executable");
  return result;
}

EngineQuiesceExecutableObjectResult EngineQuiesceExecutableObject(
    const EngineQuiesceExecutableObjectRequest& request) {
  const EngineUuid object_uuid = ObjectUuid(request);
  const auto authority = ValidateManageAuthority(request, ObjectKind(request));
  if (authority.error) {
    return DiagnosticResult<EngineQuiesceExecutableObjectResult>(
        request.context, kOperationQuiesce, authority);
  }
  EngineExecutableObjectRecord existing;
  EngineExecutableObjectLifecycleState state;
  const auto visible = FindVisibleObject(request, object_uuid, &existing, &state);
  if (visible.error) {
    return DiagnosticResult<EngineQuiesceExecutableObjectResult>(
        request.context, kOperationQuiesce, visible);
  }
  const std::uint64_t epoch = state.metadata_epoch + 1;
  auto replacement = existing;
  replacement.creator_tx = request.context.local_transaction_id;
  replacement.lifecycle_state = "quiescing";
  replacement.executable_generation = existing.executable_generation + 1;
  replacement.metadata_epoch = epoch;
  auto appended = AppendEvent(request.context, ObjectEvent(replacement));
  if (appended.error) {
    return DiagnosticResult<EngineQuiesceExecutableObjectResult>(request.context, kOperationQuiesce, appended);
  }
  appended = AppendCacheInvalidation(request.context, kOperationQuiesce, object_uuid, epoch);
  if (appended.error) {
    return DiagnosticResult<EngineQuiesceExecutableObjectResult>(request.context, kOperationQuiesce, appended);
  }
  auto result = SuccessResult<EngineQuiesceExecutableObjectResult>(request.context, kOperationQuiesce);
  FillObjectResult(&result, request.context, replacement, ActiveInvocationCount(state, object_uuid));
  AddEvidence(&result, "unload_behavior", "quiescing_new_invocations_blocked");
  return result;
}

EngineUnloadExecutableObjectResult EngineUnloadExecutableObject(
    const EngineUnloadExecutableObjectRequest& request) {
  const EngineUuid object_uuid = ObjectUuid(request);
  const auto authority = ValidateManageAuthority(request, ObjectKind(request));
  if (authority.error) {
    return DiagnosticResult<EngineUnloadExecutableObjectResult>(
        request.context, kOperationUnload, authority);
  }
  EngineExecutableObjectRecord existing;
  EngineExecutableObjectLifecycleState state;
  const auto visible = FindVisibleObject(request, object_uuid, &existing, &state);
  if (visible.error) {
    return DiagnosticResult<EngineUnloadExecutableObjectResult>(
        request.context, kOperationUnload, visible);
  }
  const auto active_count = ActiveInvocationCount(state, object_uuid);
  if (active_count != 0) {
    return DiagnosticResult<EngineUnloadExecutableObjectResult>(
        request.context,
        kOperationUnload,
        ExecDiagnostic(kExecutableObjectDiagnosticUnloadBlockedActiveInvocation, object_uuid));
  }
  const std::uint64_t epoch = state.metadata_epoch + 1;
  const std::uint64_t dependency_generation = state.dependency_generation + 1;
  auto replacement = existing;
  replacement.creator_tx = request.context.local_transaction_id;
  replacement.lifecycle_state = "unloaded";
  replacement.executable_generation = existing.executable_generation + 1;
  replacement.metadata_epoch = epoch;
  auto appended = AppendEvent(request.context, ObjectEvent(replacement));
  if (appended.error) {
    return DiagnosticResult<EngineUnloadExecutableObjectResult>(request.context, kOperationUnload, appended);
  }
  appended = InvalidateDependents(request.context, state, object_uuid, dependency_generation, epoch);
  if (appended.error) {
    return DiagnosticResult<EngineUnloadExecutableObjectResult>(request.context, kOperationUnload, appended);
  }
  appended = AppendCacheInvalidation(request.context, kOperationUnload, object_uuid, epoch);
  if (appended.error) {
    return DiagnosticResult<EngineUnloadExecutableObjectResult>(request.context, kOperationUnload, appended);
  }
  auto result = SuccessResult<EngineUnloadExecutableObjectResult>(request.context, kOperationUnload);
  FillObjectResult(&result, request.context, replacement, 0);
  AddEvidence(&result, "unload_behavior", "dispatch_table_removed");
  AddEvidence(&result, "dependency_invalidation", object_uuid);
  return result;
}

EngineBeginExecutableObjectInvocationResult EngineBeginExecutableObjectInvocation(
    const EngineBeginExecutableObjectInvocationRequest& request) {
  return BeginInvocationWithOperation(request, kOperationBeginInvocation);
}

EngineFinishExecutableObjectInvocationResult EngineFinishExecutableObjectInvocation(
    const EngineFinishExecutableObjectInvocationRequest& request) {
  return FinishInvocationWithOperation(request, kOperationFinishInvocation);
}

EngineInvokeExecutableObjectResult EngineInvokeExecutableObject(
    const EngineInvokeExecutableObjectRequest& request) {
  EngineBeginExecutableObjectInvocationRequest begin;
  static_cast<EngineApiRequest&>(begin) = request;
  auto begun = BeginInvocationWithOperation(begin, kOperationInvoke);
  if (!begun.ok) {
    EngineInvokeExecutableObjectResult failed;
    static_cast<EngineExecutableObjectLifecycleResult&>(failed) = begun;
    return failed;
  }
  EngineExecutableObjectRecord object;
  EngineExecutableObjectLifecycleState state;
  const auto object_uuid = ObjectUuid(request);
  const auto visible = FindVisibleObject(request, object_uuid, &object, &state);
  EngineApiDiagnostic body_diagnostic = OkDiagnostic();
  EngineApiResult body_evidence;
  if (visible.error) {
    body_diagnostic = visible;
  } else {
    body_diagnostic =
        ValidatePreparedMetadataRequiredVersion(request.context, object);
    if (!body_diagnostic.error) {
      body_diagnostic =
          ExecuteInternalProcedureDescriptor(request, object, &body_evidence);
    }
  }
  EngineFinishExecutableObjectInvocationRequest finish;
  static_cast<EngineApiRequest&>(finish) = request;
  finish.option_envelopes.push_back(BinaryViewUuidOption("invocation_lease_uuid:",begun.invocation_lease_uuid));
  auto finished = FinishInvocationWithOperation(finish, kOperationInvoke);
  if (!finished.ok) {
    EngineInvokeExecutableObjectResult failed;
    static_cast<EngineExecutableObjectLifecycleResult&>(failed) = finished;
    return failed;
  }
  if (body_diagnostic.error) {
    return DiagnosticResult<EngineInvokeExecutableObjectResult>(
        request.context,
        kOperationInvoke,
        body_diagnostic);
  }
  EngineInvokeExecutableObjectResult result;
  static_cast<EngineExecutableObjectLifecycleResult&>(result) = finished;
  result.invocation_lease_uuid = begun.invocation_lease_uuid;
  result.evidence.insert(result.evidence.end(),
                         body_evidence.evidence.begin(),
                         body_evidence.evidence.end());
  if (!body_evidence.result_shape.result_kind.empty() ||
      !body_evidence.result_shape.columns.empty() ||
      !body_evidence.result_shape.rows.empty()) {
    result.result_shape = std::move(body_evidence.result_shape);
  }
  result.dml_summary = std::move(body_evidence.dml_summary);
  AddEvidence(&result, "invocation_lifecycle", "sblr_or_internal_procedure_completed");
  if (HasOptionToken(request, "policy:executable.side_effect:allow") ||
      HasOptionToken(request, "side_effect_policy:allow")) {
    AddEvidence(&result, "side_effect_policy", "allowed_by_engine_policy");
  }
  return result;
}

EngineFireExecutableEventTriggerResult EngineFireExecutableEventTrigger(
    const EngineFireExecutableEventTriggerRequest& request) {
  const auto context = ValidateContext(request, true, true);
  if (context.error) {
    return DiagnosticResult<EngineFireExecutableEventTriggerResult>(
        request.context, kOperationFireEventTrigger, context);
  }
  if (HasOptionToken(request, "event_trigger_authority_unavailable:true")) {
    return DiagnosticResult<EngineFireExecutableEventTriggerResult>(
        request.context,
        kOperationFireEventTrigger,
        ExecDiagnostic(kExecutableObjectDiagnosticEventTriggerAuthorityUnavailable,
                       "event_trigger_security_or_policy_authority_unavailable"));
  }
  if (!HasEventTriggerDispatchAuthority(request)) {
    return DiagnosticResult<EngineFireExecutableEventTriggerResult>(
        request.context,
        kOperationFireEventTrigger,
        ExecDiagnostic(kExecutableObjectDiagnosticPermissionDenied, "fire_event_trigger"));
  }
  if (RequestsExecutionBoundaryBypass(request)) {
    return DiagnosticResult<EngineFireExecutableEventTriggerResult>(
        request.context,
        kOperationFireEventTrigger,
        ExecDiagnostic(kExecutableObjectDiagnosticExecutionBoundaryRefused,
                       "event_trigger_dispatch_requires_engine_boundary"));
  }
  const std::string event_name = EventTriggerEvent(request, nullptr);
  if (!ValidEventTriggerEvent(event_name)) {
    return DiagnosticResult<EngineFireExecutableEventTriggerResult>(
        request.context,
        kOperationFireEventTrigger,
        ExecDiagnostic(kExecutableObjectDiagnosticEventTriggerEventUnsupported, event_name));
  }
  if (LowerAscii(OptionValue(request, "ddl_object_kind:")) == "event_trigger") {
    auto result = SuccessResult<EngineFireExecutableEventTriggerResult>(
        request.context, kOperationFireEventTrigger);
    AddEvidence(&result, "event_trigger_boundary", "event_trigger_self_event_suppressed");
    AddRow(&result, {{"event", event_name}, {"fired_count", "0"}, {"boundary", "self_event_suppressed"}});
    return result;
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineFireExecutableEventTriggerResult>(
        request.context, kOperationFireEventTrigger, loaded.diagnostic);
  }
  std::uint64_t fired_count = 0;
  EngineUuid first_trigger_uuid;
  for (const auto& object : loaded.state.objects) {
    if (object.object_kind != "event_trigger" || object.event_trigger_event != event_name ||
        object.lifecycle_state != "active") {
      continue;
    }
    const auto dependencies = ValidateExecutableDependencies(loaded.state, object);
    if (dependencies.error) {
      return DiagnosticResult<EngineFireExecutableEventTriggerResult>(
          request.context, kOperationFireEventTrigger, dependencies);
    }
    if (!SideEffectAllowed(request, object.side_effect_class)) {
      return DiagnosticResult<EngineFireExecutableEventTriggerResult>(
          request.context,
          kOperationFireEventTrigger,
          ExecDiagnostic(kExecutableObjectDiagnosticSideEffectPolicyDenied, object.side_effect_class));
    }
    const auto appended = AppendEvent(request.context,
                                     EventTriggerFireEvent(request.context,
                                                           object.object_uuid,
                                                           event_name,
                                                           OptionValue(request, "ddl_command_tag:"),
                                                           loaded.state.metadata_epoch + fired_count + 1));
    if (appended.error) {
      return DiagnosticResult<EngineFireExecutableEventTriggerResult>(
          request.context, kOperationFireEventTrigger, appended);
    }
    if (first_trigger_uuid.is_nil()) { first_trigger_uuid = object.object_uuid; }
    ++fired_count;
  }
  auto result = SuccessResult<EngineFireExecutableEventTriggerResult>(
      request.context, kOperationFireEventTrigger);
  result.metadata_cache_epoch = loaded.state.metadata_epoch + fired_count;
  AddEvidence(&result, "event_trigger_boundary", "ddl_event_dispatched_by_engine");
  AddEvidence(&result, "event_trigger_fire_count", std::to_string(fired_count));
  AddRow(&result, {{"event", event_name},
                  {"ddl_command_tag", OptionValue(request, "ddl_command_tag:")},
                  {"fired_count", std::to_string(fired_count)},
                  {"first_trigger_uuid", first_trigger_uuid}});
  return result;
}

EngineInspectExecutableObjectResult EngineInspectExecutableObjects(
    const EngineInspectExecutableObjectRequest& request) {
  const auto context = ValidateContext(request, false, true);
  if (context.error) {
    return DiagnosticResult<EngineInspectExecutableObjectResult>(
        request.context, kOperationInspect, context);
  }
  if (!HasInspectPermission(request)) {
    return DiagnosticResult<EngineInspectExecutableObjectResult>(
        request.context,
        kOperationInspect,
        ExecDiagnostic(kExecutableObjectDiagnosticPermissionDenied, "inspect_executable"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineInspectExecutableObjectResult>(
        request.context, kOperationInspect, loaded.diagnostic);
  }
  auto result = SuccessResult<EngineInspectExecutableObjectResult>(request.context, kOperationInspect);
  result.metadata_cache_epoch = loaded.state.metadata_epoch;
  for (const auto& object : loaded.state.objects) {
    if (!request.target_object.uuid.is_nil() &&
        object.object_uuid != request.target_object.uuid) {
      continue;
    }
    AddRow(&result, {{"object_uuid", object.object_uuid},
                    {"object_kind", object.object_kind},
                    {"lifecycle_state", object.lifecycle_state},
                    {"executable_generation", std::to_string(object.executable_generation)},
                    {"metadata_epoch", std::to_string(object.metadata_epoch)},
                    {"stored_sblr_hash", object.stored_sblr_hash},
                    {"stored_sblr_provenance", object.stored_sblr_provenance},
                    {"invalidated", object.invalidated ? "true" : "false"},
                    {"active_invocation_count",
                     std::to_string(ActiveInvocationCount(loaded.state, object.object_uuid))}});
  }
  AddEvidence(&result, "permission_check", "inspect_executable");
  AddEvidence(&result, "metadata_cache_epoch", std::to_string(loaded.state.metadata_epoch));
  return result;
}

}  // namespace scratchbird::engine::internal_api
