// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "extensibility/executable_object_lifecycle.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
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

std::string HexEncode(const std::string& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2);
  for (unsigned char c : value) {
    out.push_back(kHex[(c >> 4) & 0x0f]);
    out.push_back(kHex[c & 0x0f]);
  }
  return out;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return 10 + c - 'a'; }
  if (c >= 'A' && c <= 'F') { return 10 + c - 'A'; }
  return -1;
}

std::string HexDecode(const std::string& value) {
  std::string out;
  if ((value.size() % 2) != 0) { return out; }
  out.reserve(value.size() / 2);
  for (std::size_t i = 0; i < value.size(); i += 2) {
    const int hi = HexValue(value[i]);
    const int lo = HexValue(value[i + 1]);
    if (hi < 0 || lo < 0) { return {}; }
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
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

EngineTypedValue Value(std::string value) {
  EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.encoded_value = std::move(value);
  return typed;
}

void AddRow(EngineApiResult* result, std::vector<std::pair<std::string, std::string>> fields) {
  EngineRowValue row;
  row.requested_row_uuid.canonical = "exec-row-" + std::to_string(result->result_shape.rows.size() + 1);
  for (auto& field : fields) { row.fields.push_back({std::move(field.first), Value(std::move(field.second))}); }
  result->result_shape.result_kind = "executable_object_lifecycle_rows";
  result->result_shape.rows.push_back(std::move(row));
}

void AddEvidence(EngineApiResult* result, std::string kind, std::string id) {
  result->evidence.push_back({std::move(kind), std::move(id)});
}

bool EventVisible(const EngineRequestContext& context, std::uint64_t creator_tx) {
  if (creator_tx == 0) { return true; }
  if (context.local_transaction_id != 0 && creator_tx == context.local_transaction_id) { return true; }
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

std::string ObjectUuid(const EngineApiRequest& request) {
  if (!request.target_object.uuid.canonical.empty()) { return request.target_object.uuid.canonical; }
  if (!request.bound_object_identity.object_uuid.canonical.empty()) {
    return request.bound_object_identity.object_uuid.canonical;
  }
  return OptionValue(request, "object_uuid:");
}

std::string PackageUuid(const EngineApiRequest& request) {
  if (!request.context.current_package_uuid.canonical.empty()) {
    return request.context.current_package_uuid.canonical;
  }
  return OptionValue(request, "package_uuid:");
}

std::string ExecutorKind(const EngineApiRequest& request,
                         const EngineExecutableObjectRecord* existing = nullptr) {
  auto executor = LowerAscii(OptionValue(request, "executor:"));
  if (executor.empty()) { executor = LowerAscii(OptionValue(request, "execution_engine:")); }
  if (executor == "internal") { executor = "internal_procedure"; }
  if (executor.empty() && existing != nullptr) { return existing->executor_kind; }
  return executor.empty() ? "sblr" : executor;
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
  if (!request.descriptors.empty()) {
    fields.push_back("descriptor=" + request.descriptors.front().canonical_type_name);
  }
  if (!request.rows.empty()) { fields.push_back("row_parameter_count=" + std::to_string(request.rows.size())); }
  return Join(fields, ';');
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
  return HasOptionToken(request, "permission:manage_executable") ||
         HasOptionToken(request, "permission:manage_routine") ||
         HasOptionToken(request, "right:MANAGE_EXECUTABLE_OBJECT") ||
         HasOptionToken(request, "role:DBA");
}

bool HasInspectPermission(const EngineApiRequest& request) {
  return HasManagePermission(request) ||
         HasOptionToken(request, "permission:inspect_executable") ||
         HasOptionToken(request, "right:INSPECT_EXECUTABLE_OBJECT");
}

bool HasInvokePermission(const EngineApiRequest& request, const std::string& object_uuid) {
  return HasManagePermission(request) ||
         HasOptionToken(request, "permission:invoke_executable") ||
         HasOptionToken(request, "permission:execute") ||
         HasOptionToken(request, "execute:" + object_uuid) ||
         HasOptionToken(request, "right:EXECUTE_EXECUTABLE_OBJECT");
}

bool HasEventTriggerManagePermission(const EngineApiRequest& request) {
  return HasManagePermission(request) &&
         (HasOptionToken(request, "permission:manage_event_trigger") ||
          HasOptionToken(request, "right:MANAGE_EVENT_TRIGGER") ||
          HasOptionToken(request, "role:DBA"));
}

bool HasEventTriggerDispatchAuthority(const EngineApiRequest& request) {
  return HasOptionToken(request, "engine_event_trigger_dispatch:true") ||
         HasOptionToken(request, "permission:fire_event_trigger") ||
         HasManagePermission(request);
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

  if (*executor_kind != "sblr" && *executor_kind != "internal_procedure") {
    return ExecDiagnostic(kExecutableObjectDiagnosticExecutionBoundaryRefused, *executor_kind);
  }
  if (*executor_kind == "sblr") {
    if (stored_sblr_hash->empty() || !StartsWith(*stored_sblr_hash, "sha256:")) {
      return ExecDiagnostic(kExecutableObjectDiagnosticStoredSblrRequired, "sha256_sblr_hash");
    }
    if (stored_sblr_provenance->empty()) {
      return ExecDiagnostic(kExecutableObjectDiagnosticStoredSblrProvenanceRequired, "sblr_provenance");
    }
  } else if (internal_procedure_id->empty()) {
    return ExecDiagnostic(kExecutableObjectDiagnosticInternalProcedureRequired, "internal_procedure_id");
  }
  if (object_kind == "event_trigger") {
    if (!ValidEventTriggerEvent(*event_trigger_event)) {
      return ExecDiagnostic(kExecutableObjectDiagnosticEventTriggerEventUnsupported, *event_trigger_event);
    }
  }
  return OkDiagnostic();
}

std::string ObjectEvent(const EngineExecutableObjectRecord& record) {
  return std::string(kExecutableObjectLifecycleEventMagic) + "\tOBJECT\t" +
         std::to_string(record.creator_tx) + "\t" + record.object_uuid + "\t" + record.object_kind + "\t" +
         record.schema_uuid + "\t" + record.owner_principal_uuid + "\t" + record.package_uuid + "\t" +
         record.lifecycle_state + "\t" + std::to_string(record.executable_generation) + "\t" +
         std::to_string(record.metadata_epoch) + "\t" + record.executor_kind + "\t" +
         HexEncode(record.stored_sblr_hash) + "\t" + HexEncode(record.stored_sblr_provenance) + "\t" +
         HexEncode(record.internal_procedure_id) + "\t" + record.side_effect_class + "\t" +
         record.event_trigger_event + "\t" + HexEncode(record.payload) + "\t" +
         (record.deleted ? "1" : "0");
}

std::string DependencyEvent(const EngineExecutableDependencyRecord& record) {
  return std::string(kExecutableObjectLifecycleEventMagic) + "\tDEPENDENCY\t" +
         std::to_string(record.creator_tx) + "\t" + record.source_uuid + "\t" + record.source_kind + "\t" +
         record.dependency_uuid + "\t" + record.dependency_kind + "\t" +
         std::to_string(record.dependency_generation) + "\t" + std::to_string(record.metadata_epoch) + "\t" +
         (record.deleted ? "1" : "0");
}

std::string InvalidationEvent(std::uint64_t tx,
                              const std::string& object_uuid,
                              const std::string& reason_uuid,
                              std::uint64_t dependency_generation,
                              std::uint64_t metadata_epoch) {
  return std::string(kExecutableObjectLifecycleEventMagic) + "\tINVALIDATE\t" +
         std::to_string(tx) + "\t" + object_uuid + "\t" + reason_uuid + "\t" +
         std::to_string(dependency_generation) + "\t" + std::to_string(metadata_epoch);
}

std::string InvocationEvent(const EngineExecutableInvocationRecord& record) {
  return std::string(kExecutableObjectLifecycleEventMagic) + "\tINVOCATION\t" +
         std::to_string(record.creator_tx) + "\t" + record.invocation_lease_uuid + "\t" +
         record.object_uuid + "\t" + std::to_string(record.executable_generation) + "\t" +
         record.lifecycle_state + "\t" + std::to_string(record.metadata_epoch);
}

std::string CacheInvalidateEvent(const EngineRequestContext& context,
                                 const std::string& operation_id,
                                 const std::string& object_uuid,
                                 std::uint64_t metadata_epoch) {
  return std::string(kExecutableObjectLifecycleEventMagic) + "\tCACHE_INVALIDATE\t" +
         std::to_string(context.local_transaction_id) + "\t" + HexEncode(operation_id) + "\t" +
         object_uuid + "\t" + std::to_string(metadata_epoch) + "\t" +
         std::to_string(context.security_epoch) + "\t" + std::to_string(context.resource_epoch);
}

std::string EventTriggerFireEvent(const EngineRequestContext& context,
                                  const std::string& trigger_uuid,
                                  const std::string& event_name,
                                  const std::string& command_tag,
                                  std::uint64_t metadata_epoch) {
  return std::string(kExecutableObjectLifecycleEventMagic) + "\tEVENT_TRIGGER_FIRE\t" +
         std::to_string(context.local_transaction_id) + "\t" + trigger_uuid + "\t" +
         event_name + "\t" + HexEncode(command_tag) + "\t" + std::to_string(metadata_epoch);
}

EngineApiDiagnostic AppendEvent(const EngineRequestContext& context, const std::string& event) {
  if (context.database_path.empty()) {
    return ExecDiagnostic(kExecutableObjectDiagnosticDatabasePathRequired, "database_path");
  }
  std::ofstream out(EventPath(context), std::ios::binary | std::ios::app);
  if (!out) { return ExecDiagnostic(kExecutableObjectDiagnosticDatabaseWriteFailed, "open"); }
  out << event << '\n';
  out.flush();
  if (!out) { return ExecDiagnostic(kExecutableObjectDiagnosticDatabaseWriteFailed, "flush"); }
  return OkDiagnostic();
}

EngineApiDiagnostic AppendCacheInvalidation(const EngineRequestContext& context,
                                            const std::string& operation_id,
                                            const std::string& object_uuid,
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
    result.ok = true;
    result.diagnostic = OkDiagnostic();
    return result;
  }

  std::map<std::string, EngineExecutableObjectRecord> objects;
  std::map<std::string, EngineExecutableDependencyRecord> dependencies;
  std::map<std::string, EngineExecutableInvocationRecord> active_invocations;
  std::uint64_t event_sequence = 0;
  std::string line;
  while (std::getline(in, line)) {
    ++event_sequence;
    if (!StartsWith(line, kExecutableObjectLifecycleEventMagic)) { continue; }
    const auto parts = Split(line, '\t');
    if (parts.size() < 2) { continue; }
    const std::uint64_t creator_tx = parts.size() >= 3 ? ParseU64(parts[2]) : 0;
    if (options.enforce_visibility && !EventVisible(context, creator_tx)) { continue; }
    const std::string& event = parts[1];
    if (event == "OBJECT" && parts.size() >= 19) {
      EngineExecutableObjectRecord record;
      record.event_sequence = event_sequence;
      record.creator_tx = creator_tx;
      record.object_uuid = parts[3];
      record.object_kind = parts[4];
      record.schema_uuid = parts[5];
      record.owner_principal_uuid = parts[6];
      record.package_uuid = parts[7];
      record.lifecycle_state = parts[8].empty() ? "active" : parts[8];
      record.executable_generation = ParseU64(parts[9]);
      record.metadata_epoch = ParseU64(parts[10]);
      record.executor_kind = parts[11].empty() ? "sblr" : parts[11];
      record.stored_sblr_hash = HexDecode(parts[12]);
      record.stored_sblr_provenance = HexDecode(parts[13]);
      record.internal_procedure_id = HexDecode(parts[14]);
      record.side_effect_class = parts[15].empty() ? "none" : parts[15];
      record.event_trigger_event = parts[16];
      record.payload = HexDecode(parts[17]);
      record.deleted = ParseBool(parts[18]);
      result.state.metadata_epoch = std::max(result.state.metadata_epoch, record.metadata_epoch);
      if (record.deleted || record.lifecycle_state == "dropped") {
        objects.erase(record.object_uuid);
      } else {
        objects[record.object_uuid] = std::move(record);
      }
    } else if (event == "DEPENDENCY" && parts.size() >= 10) {
      EngineExecutableDependencyRecord record;
      record.event_sequence = event_sequence;
      record.creator_tx = creator_tx;
      record.source_uuid = parts[3];
      record.source_kind = parts[4];
      record.dependency_uuid = parts[5];
      record.dependency_kind = parts[6];
      record.dependency_generation = ParseU64(parts[7]);
      record.metadata_epoch = ParseU64(parts[8]);
      record.deleted = ParseBool(parts[9]);
      result.state.metadata_epoch = std::max(result.state.metadata_epoch, record.metadata_epoch);
      result.state.dependency_generation =
          std::max(result.state.dependency_generation, record.dependency_generation);
      const std::string key = record.source_uuid + "\t" + record.dependency_uuid + "\t" + record.dependency_kind;
      if (record.deleted) {
        dependencies.erase(key);
      } else {
        dependencies[key] = std::move(record);
      }
    } else if (event == "INVALIDATE" && parts.size() >= 7) {
      const std::string object_uuid = parts[3];
      const std::string reason_uuid = parts[4];
      const std::uint64_t dependency_generation = ParseU64(parts[5]);
      const std::uint64_t metadata_epoch = ParseU64(parts[6]);
      result.state.metadata_epoch = std::max(result.state.metadata_epoch, metadata_epoch);
      result.state.dependency_generation = std::max(result.state.dependency_generation, dependency_generation);
      auto found = objects.find(object_uuid);
      if (found != objects.end()) {
        found->second.invalidated = true;
        found->second.invalidated_generation = dependency_generation;
        found->second.invalidation_reason_uuid = reason_uuid;
      }
    } else if (event == "INVOCATION" && parts.size() >= 8) {
      EngineExecutableInvocationRecord record;
      record.event_sequence = event_sequence;
      record.creator_tx = creator_tx;
      record.invocation_lease_uuid = parts[3];
      record.object_uuid = parts[4];
      record.executable_generation = ParseU64(parts[5]);
      record.lifecycle_state = parts[6];
      record.metadata_epoch = ParseU64(parts[7]);
      result.state.metadata_epoch = std::max(result.state.metadata_epoch, record.metadata_epoch);
      if (record.lifecycle_state == "active") {
        active_invocations[record.invocation_lease_uuid] = std::move(record);
      } else {
        active_invocations.erase(record.invocation_lease_uuid);
      }
    } else if (event == "CACHE_INVALIDATE" && parts.size() >= 6) {
      result.state.metadata_epoch = std::max(result.state.metadata_epoch, ParseU64(parts[5]));
    } else if (event == "EVENT_TRIGGER_FIRE" && parts.size() >= 7) {
      result.state.metadata_epoch = std::max(result.state.metadata_epoch, ParseU64(parts[6]));
    }
  }

  std::set<std::string> active_objects;
  for (auto& [_, object] : objects) {
    active_objects.insert(object.object_uuid);
    result.state.objects.push_back(std::move(object));
  }
  for (auto& [_, dependency] : dependencies) {
    if (active_objects.count(dependency.source_uuid) != 0 &&
        active_objects.count(dependency.dependency_uuid) != 0) {
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
                                               const std::string& object_uuid) {
  for (const auto& object : state.objects) {
    if (object.object_uuid == object_uuid) { return &object; }
  }
  return nullptr;
}

std::vector<EngineExecutableDependencyRecord> DependenciesForSource(
    const EngineExecutableObjectLifecycleState& state,
    const std::string& source_uuid) {
  std::vector<EngineExecutableDependencyRecord> dependencies;
  for (const auto& dependency : state.dependencies) {
    if (dependency.source_uuid == source_uuid) { dependencies.push_back(dependency); }
  }
  return dependencies;
}

std::vector<EngineExecutableDependencyRecord> DependentsOf(
    const EngineExecutableObjectLifecycleState& state,
    const std::string& dependency_uuid) {
  std::vector<EngineExecutableDependencyRecord> dependents;
  for (const auto& dependency : state.dependencies) {
    if (dependency.dependency_uuid == dependency_uuid) { dependents.push_back(dependency); }
  }
  return dependents;
}

std::uint64_t ActiveInvocationCount(const EngineExecutableObjectLifecycleState& state,
                                    const std::string& object_uuid) {
  std::uint64_t count = 0;
  for (const auto& invocation : state.active_invocations) {
    if (invocation.object_uuid == object_uuid) { ++count; }
  }
  return count;
}

EngineApiDiagnostic FindVisibleObject(const EngineApiRequest& request,
                                      const std::string& object_uuid,
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
    if (related.uuid.canonical.empty()) { continue; }
    const auto* dependency = FindObject(state, related.uuid.canonical);
    if (dependency == nullptr) {
      return ExecDiagnostic(kExecutableObjectDiagnosticDependencyNotVisible, related.uuid.canonical);
    }
    if (dependency->invalidated) {
      return ExecDiagnostic(kExecutableObjectDiagnosticDependencyInvalidated, related.uuid.canonical);
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateExecutableDependencies(const EngineExecutableObjectLifecycleState& state,
                                                   const EngineExecutableObjectRecord& object) {
  for (const auto& dependency : DependenciesForSource(state, object.object_uuid)) {
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
                                            const std::string& source_uuid,
                                            const std::string& source_kind,
                                            std::uint64_t dependency_generation,
                                            std::uint64_t metadata_epoch) {
  for (const auto& related : request.related_objects) {
    if (related.uuid.canonical.empty()) { continue; }
    EngineExecutableDependencyRecord dependency;
    dependency.creator_tx = request.context.local_transaction_id;
    dependency.source_uuid = source_uuid;
    dependency.source_kind = source_kind;
    dependency.dependency_uuid = related.uuid.canonical;
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
                                            const std::string& source_uuid,
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
                                         const std::string& changed_uuid,
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
  result->primary_object.uuid.canonical = object.object_uuid;
  result->primary_object.object_kind = object.object_kind;
  result->bound_object_identity.object_uuid.canonical = object.object_uuid;
  result->bound_object_identity.resolved_object_type = object.object_kind;
  result->bound_object_identity.resolved_schema_uuid.canonical = object.schema_uuid;
  result->bound_object_identity.parent_object_uuid.canonical = object.package_uuid;
  result->bound_object_identity.catalog_generation_id = object.metadata_epoch;
  result->bound_object_identity.security_epoch = context.security_epoch;
  result->bound_object_identity.resource_epoch = context.resource_epoch;
  result->catalog_row_uuid.canonical = "exec-catalog-row-" + object.object_uuid;
  result->metadata_cache_epoch = object.metadata_epoch;
  result->executable_generation = object.executable_generation;
  result->active_invocation_count = active_invocation_count;
  AddEvidence(result, "executable_generation", std::to_string(object.executable_generation));
  AddEvidence(result, "metadata_cache_invalidation",
              object.object_uuid + ":" + std::to_string(object.metadata_epoch));
  AddEvidence(result, "stored_sblr", "hash_and_provenance_recorded");
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

std::string RequestedLeaseUuid(const EngineApiRequest& request,
                               const std::string& object_uuid,
                               std::uint64_t active_count) {
  auto lease = OptionValue(request, "invocation_lease_uuid:");
  if (!lease.empty()) { return lease; }
  return "exec-lease-" + object_uuid + "-" + std::to_string(request.context.local_transaction_id) +
         "-" + std::to_string(active_count + 1);
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
  if (object.lifecycle_state == "quiescing") {
    return ExecDiagnostic(kExecutableObjectDiagnosticQuiescing, object.object_uuid);
  }
  if (object.lifecycle_state == "unloaded") {
    return ExecDiagnostic(kExecutableObjectDiagnosticUnloaded, object.object_uuid);
  }
  if (object.invalidated) {
    return ExecDiagnostic(kExecutableObjectDiagnosticDependencyInvalidated,
                          object.invalidation_reason_uuid.empty() ? object.object_uuid
                                                                  : object.invalidation_reason_uuid);
  }
  const auto dependencies = ValidateExecutableDependencies(state, object);
  if (dependencies.error) { return dependencies; }
  if (!SideEffectAllowed(request, object.side_effect_class)) {
    return ExecDiagnostic(kExecutableObjectDiagnosticSideEffectPolicyDenied, object.side_effect_class);
  }
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
  const std::string object_uuid = ObjectUuid(request);
  if (object_uuid.empty()) {
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
  const std::string object_uuid = ObjectUuid(request);
  const std::string lease_uuid = OptionValue(request, "invocation_lease_uuid:");
  if (object_uuid.empty()) {
    return DiagnosticResult<EngineFinishExecutableObjectInvocationResult>(
        request.context,
        operation_id,
        ExecDiagnostic(kExecutableObjectDiagnosticUuidRequired, "target_object.uuid"));
  }
  if (lease_uuid.empty()) {
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
  AddEvidence(&result, "invocation_lifecycle", "active_invocation_released");
  return result;
}

}  // namespace

EngineLoadExecutableObjectLifecycleStateResult LoadExecutableObjectLifecycleState(
    const EngineRequestContext& context) {
  return LoadState(context, {.enforce_visibility = true});
}

EngineCreateExecutableObjectResult EngineCreateExecutableObject(
    const EngineCreateExecutableObjectRequest& request) {
  const std::string object_uuid = ObjectUuid(request);
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
  if (object_uuid.empty()) {
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
  if (object_kind != "event_trigger" && request.target_schema.uuid.canonical.empty()) {
    return DiagnosticResult<EngineCreateExecutableObjectResult>(
        request.context,
        kOperationCreate,
        ExecDiagnostic(kExecutableObjectDiagnosticSchemaUuidRequired, "target_schema.uuid"));
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
  record.object_kind = object_kind;
  record.schema_uuid = request.target_schema.uuid.canonical;
  record.owner_principal_uuid = request.context.principal_uuid.canonical;
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
  if (object_kind == "event_trigger") {
    AddEvidence(&result, "event_trigger_boundary", "ddl_event_filter_registered");
  }
  return result;
}

EngineAlterExecutableObjectResult EngineAlterExecutableObject(
    const EngineAlterExecutableObjectRequest& request) {
  const std::string object_uuid = ObjectUuid(request);
  const auto authority = ValidateManageAuthority(request, ObjectKind(request));
  if (authority.error) {
    return DiagnosticResult<EngineAlterExecutableObjectResult>(
        request.context, kOperationAlter, authority);
  }
  if (object_uuid.empty()) {
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
  replacement.invalidation_reason_uuid.clear();

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
  const std::string object_uuid = ObjectUuid(request);
  const auto authority = ValidateManageAuthority(request, ObjectKind(request));
  if (authority.error) {
    return DiagnosticResult<EngineDropExecutableObjectResult>(
        request.context, kOperationDrop, authority);
  }
  if (object_uuid.empty()) {
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
  const std::string object_uuid = ObjectUuid(request);
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
  const std::string object_uuid = ObjectUuid(request);
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
  EngineFinishExecutableObjectInvocationRequest finish;
  static_cast<EngineApiRequest&>(finish) = request;
  finish.option_envelopes.push_back("invocation_lease_uuid:" + begun.invocation_lease_uuid);
  auto finished = FinishInvocationWithOperation(finish, kOperationInvoke);
  if (!finished.ok) {
    EngineInvokeExecutableObjectResult failed;
    static_cast<EngineExecutableObjectLifecycleResult&>(failed) = finished;
    return failed;
  }
  EngineInvokeExecutableObjectResult result;
  static_cast<EngineExecutableObjectLifecycleResult&>(result) = finished;
  result.invocation_lease_uuid = begun.invocation_lease_uuid;
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
  std::string first_trigger_uuid;
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
    if (first_trigger_uuid.empty()) { first_trigger_uuid = object.object_uuid; }
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
    if (!request.target_object.uuid.canonical.empty() &&
        object.object_uuid != request.target_object.uuid.canonical) {
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
