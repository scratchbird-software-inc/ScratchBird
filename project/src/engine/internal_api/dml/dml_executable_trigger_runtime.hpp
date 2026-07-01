// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "crud_support/crud_store.hpp"
#include "catalog/name_resolution_api.hpp"
#include "dml/insert_api.hpp"
#include "extensibility/executable_object_lifecycle.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "sblr_sequence_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api::dml_trigger_runtime {

struct DmlExecutableTriggerRuntimeResult {
  bool ok = true;
  EngineApiDiagnostic diagnostic;
  std::size_t fired_count = 0;
  std::vector<EngineEvidenceReference> evidence;
};

struct DmlTriggerUpdateRowImage {
  std::vector<std::pair<std::string, std::string>> old_values;
  std::vector<std::pair<std::string, std::string>> new_values;
};

inline std::string LowerAscii(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

inline bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

inline bool EndsWith(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

inline std::string ResolveVisibleTableByPresentedName(const EngineRequestContext& context,
                                                      const std::string& presented_name);
inline std::string ResolveVisibleObjectByPresentedName(const EngineRequestContext& context,
                                                       const std::string& presented_name,
                                                       std::string expected_object_type);

inline bool OptionPresent(const std::vector<std::string>& options,
                          std::string_view token) {
  return std::any_of(options.begin(), options.end(), [&](const auto& option) {
    return option == token;
  });
}

inline std::string PayloadFieldValue(const std::string& payload,
                                     std::string_view prefix) {
  std::size_t offset = 0;
  while (offset <= payload.size()) {
    const auto next = payload.find(';', offset);
    const auto end = next == std::string::npos ? payload.size() : next;
    const std::string_view field(payload.data() + offset, end - offset);
    if (StartsWith(field, prefix)) {
      return std::string(field.substr(prefix.size()));
    }
    if (next == std::string::npos) break;
    offset = next + 1;
  }
  return {};
}

inline bool TriggerEventMatches(const std::string& trigger_event,
                                std::string_view event_name) {
  const auto event = LowerAscii(trigger_event);
  const std::string target(event_name);
  std::size_t offset = 0;
  while (offset <= event.size()) {
    const auto next = event.find('_', offset);
    const auto end = next == std::string::npos ? event.size() : next;
    if (event.substr(offset, end - offset) == target) return true;
    if (next == std::string::npos) break;
    offset = next + 1;
  }
  return event == target;
}

inline bool TriggerDescriptorMatches(const EngineExecutableObjectRecord& object,
                                     const EngineRequestContext& context,
                                     const std::string& target_table_uuid,
                                     const std::string& target_table_name,
                                     std::string_view event_name,
                                     std::string_view scope,
                                     std::string_view descriptor) {
  if (LowerAscii(object.object_kind) != "trigger") return false;
  if (object.lifecycle_state != "active" || object.deleted || object.invalidated) return false;
  const auto descriptor_target_uuid =
      PayloadFieldValue(object.payload, "trigger_target_table_uuid:");
  if (!descriptor_target_uuid.empty() && descriptor_target_uuid != target_table_uuid) {
    return false;
  }
  if (descriptor_target_uuid.empty()) {
    const auto descriptor_target_name =
        LowerAscii(PayloadFieldValue(object.payload, "trigger_target_table_name:"));
    const auto expected_name = LowerAscii(target_table_name);
    if (descriptor_target_name.empty()) {
      return false;
    }
    if (descriptor_target_name.find('.') != std::string::npos) {
      const std::string resolved_target_uuid =
          ResolveVisibleTableByPresentedName(context, descriptor_target_name);
      if (resolved_target_uuid != target_table_uuid) {
        return false;
      }
    } else if (expected_name.empty() || descriptor_target_name != expected_name) {
      return false;
    }
  }
  if (LowerAscii(PayloadFieldValue(object.payload, "trigger_timing:")) != "after") {
    return false;
  }
  if (LowerAscii(PayloadFieldValue(object.payload, "trigger_scope:")) != scope) {
    return false;
  }
  if (!TriggerEventMatches(PayloadFieldValue(object.payload, "trigger_event:"), event_name)) {
    return false;
  }
  return PayloadFieldValue(object.payload, "compiled_body_descriptor:") == descriptor;
}

inline std::string ValueFor(const std::vector<std::pair<std::string, std::string>>& values,
                            const std::string& field) {
  return CrudFieldValue(values, field);
}

inline EngineTypedValue TypedValue(std::string type_name,
                                   std::string value,
                                   bool is_null = false) {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(type_name);
  return EngineTypedValue(std::move(descriptor), std::move(value), is_null);
}

inline std::uint64_t ParseAuditId(std::string_view value) {
  if (value.empty()) return 0;
  try {
    std::size_t parsed = 0;
    const auto id = static_cast<std::uint64_t>(
        std::stoull(std::string(value), &parsed));
    return parsed == value.size() ? id : 0;
  } catch (...) {
    return 0;
  }
}

inline std::uint64_t NextAuditId(const CrudState& state,
                                 const std::string& audit_table_uuid,
                                 const EngineRequestContext& context) {
  std::uint64_t max_audit_id = 0;
  for (const auto& row : VisibleCrudRowsForContext(state, audit_table_uuid, context)) {
    max_audit_id =
        std::max(max_audit_id, ParseAuditId(CrudFieldValue(row.values, "audit_id")));
  }
  return max_audit_id + 1;
}

inline scratchbird::engine::sblr::SblrExecutionContext TriggerSblrContext(
    const EngineRequestContext& context) {
  scratchbird::engine::sblr::SblrExecutionContext out;
  out.database_path = context.database_path;
  out.database_uuid = context.database_uuid.canonical;
  out.cluster_uuid = context.cluster_uuid.canonical;
  out.node_uuid = context.node_uuid.canonical;
  out.transaction_uuid = context.transaction_uuid.canonical;
  out.local_transaction_id = context.local_transaction_id;
  out.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  out.transaction_isolation_level = context.transaction_isolation_level;
  out.statement_uuid = context.statement_uuid.canonical;
  out.session_uuid = context.session_uuid.canonical;
  out.user_uuid = context.principal_uuid.canonical;
  out.current_role_uuid = context.current_role_uuid.canonical;
  out.current_schema_uuid = context.current_schema_uuid.canonical;
  out.statement_timestamp = context.statement_timestamp;
  out.transaction_timestamp = context.transaction_timestamp;
  out.current_timestamp = context.current_timestamp;
  out.current_monotonic_ns = context.current_monotonic_ns;
  out.security_context_present = context.security_context_present;
  out.transaction_context_present =
      context.local_transaction_id != 0 || !context.transaction_uuid.canonical.empty();
  out.cluster_authority_available = context.cluster_authority_available;
  out.read_only_mode = context.read_only_mode;
  return out;
}

inline bool NextSequenceAuditId(const EngineRequestContext& context,
                                const std::string& sequence_uuid,
                                std::uint64_t* audit_id,
                                EngineApiDiagnostic* diagnostic) {
  if (audit_id == nullptr) return false;
  if (sequence_uuid.empty()) {
    if (diagnostic != nullptr) {
      *diagnostic = EngineApiDiagnostic{
          "TRIGGER.RUNTIME.SEQUENCE_UNRESOLVED",
          "trigger.runtime.sequence_unresolved",
          "compiled trigger audit sequence could not be resolved",
          true};
    }
    return false;
  }
  scratchbird::engine::sblr::SblrSequenceRequest request;
  request.context = TriggerSblrContext(context);
  request.sequence_uuid = LowerAscii(sequence_uuid);
  request.result_descriptor_id = "int64";
  const auto result = scratchbird::engine::sblr::NextSblrSequenceValue(
      &scratchbird::engine::sblr::ProcessSblrSequenceRegistry(), request);
  if (!result.ok() || result.scalar_values.empty()) {
    if (diagnostic != nullptr) {
      const std::string detail =
          result.diagnostics.empty()
              ? "compiled trigger audit sequence did not return a value"
              : result.diagnostics.front().detail;
      *diagnostic = EngineApiDiagnostic{
          "TRIGGER.RUNTIME.SEQUENCE_NEXT_FAILED",
          "trigger.runtime.sequence_next_failed",
          detail,
          true};
    }
    return false;
  }
  const std::uint64_t parsed = ParseAuditId(result.scalar_values.front().encoded_value);
  if (parsed == 0) {
    if (diagnostic != nullptr) {
      *diagnostic = EngineApiDiagnostic{
          "TRIGGER.RUNTIME.SEQUENCE_VALUE_INVALID",
          "trigger.runtime.sequence_value_invalid",
          "compiled trigger audit sequence returned a non-positive or invalid audit id",
          true};
    }
    return false;
  }
  *audit_id = parsed;
  return true;
}

inline EngineRowValue AuditRow(std::uint64_t audit_id,
                               std::string event_kind,
                               std::string item_id,
                               std::string old_price,
                               bool old_price_null,
                               std::string new_price,
                               bool new_price_null,
                               std::string note) {
  EngineRowValue row;
  const bool item_id_null = item_id.empty();
  row.fields.push_back({"audit_id", TypedValue("integer", std::to_string(audit_id))});
  row.fields.push_back({"event_kind", TypedValue("varchar", std::move(event_kind))});
  row.fields.push_back({"item_id", TypedValue("integer", std::move(item_id), item_id_null)});
  row.fields.push_back({"old_price", TypedValue("decimal", std::move(old_price), old_price_null)});
  row.fields.push_back({"new_price", TypedValue("decimal", std::move(new_price), new_price_null)});
  row.fields.push_back({"audit_note", TypedValue("varchar", std::move(note))});
  return row;
}

inline std::string FindVisibleTableByName(const CrudState& state,
                                          const EngineRequestContext& context,
                                          std::string_view table_name) {
  const auto target = LowerAscii(std::string(table_name));
  for (const auto& table : state.tables) {
    if (!CrudCreatorVisible(state,
                            table.creator_tx,
                            table.event_sequence,
                            context.local_transaction_id)) {
      continue;
    }
    if (LowerAscii(table.default_name) == target) return table.table_uuid;
  }
  return {};
}

inline std::string SiblingPresentedName(std::string_view qualified_name,
                                        std::string_view sibling_leaf) {
  const auto dot = qualified_name.rfind('.');
  if (dot == std::string_view::npos) return std::string(sibling_leaf);
  std::string sibling(qualified_name.substr(0, dot + 1));
  sibling.append(sibling_leaf);
  return sibling;
}

inline EngineLocalizedName ResolverName(const EngineRequestContext& context,
                                        const std::string& presented_name) {
  EngineLocalizedName name;
  name.language_tag = context.language_context.language_tag.empty()
                          ? "en"
                          : context.language_context.language_tag;
  name.name_class = "primary";
  name.name = presented_name;
  name.raw_name_text = presented_name;
  name.display_name = presented_name;
  name.default_name = true;
  return name;
}

inline EngineIdentifierAtom ResolverIdentifierAtom(std::string text) {
  EngineIdentifierAtom atom;
  atom.raw_text = std::move(text);
  atom.was_quoted = false;
  atom.quote_style = "none";
  atom.identifier_profile_uuid = "sbsql_v3";
  atom.requires_exact_match = false;
  return atom;
}

inline std::string ResolveVisibleTableByPresentedName(const EngineRequestContext& context,
                                                      const std::string& presented_name) {
  return ResolveVisibleObjectByPresentedName(context, presented_name, "table");
}

inline std::string ResolveVisibleObjectByPresentedName(const EngineRequestContext& context,
                                                       const std::string& presented_name,
                                                       std::string expected_object_type) {
  std::vector<std::string> parts;
  std::size_t offset = 0;
  while (offset <= presented_name.size()) {
    const auto next = presented_name.find('.', offset);
    const auto end = next == std::string::npos ? presented_name.size() : next;
    if (end > offset) {
      parts.emplace_back(presented_name.substr(offset, end - offset));
    }
    if (next == std::string::npos) break;
    offset = next + 1;
  }
  if (parts.empty()) return {};
  EngineResolveNameRequest request;
  request.context = context;
  request.sql_object_reference.expected_object_type = expected_object_type;
  request.sql_object_reference.path_type =
      parts.size() > 1 ? "qualified" : "unqualified";
  request.sql_object_reference.no_search_path = parts.size() > 1;
  for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
    request.sql_object_reference.path_components.push_back(
        ResolverIdentifierAtom(parts[index]));
  }
  request.sql_object_reference.object_name = ResolverIdentifierAtom(parts.back());
  const auto resolved = EngineResolveName(request);
  if (!resolved.ok) return {};
  return resolved.bound_object_identity.object_uuid.canonical;
}

inline std::string ResolveVisibleSequenceByPresentedName(const EngineRequestContext& context,
                                                         const std::string& presented_name) {
  return ResolveVisibleObjectByPresentedName(context, presented_name, "sequence");
}

inline std::string ResolveTriggerAuditTableUuid(const EngineRequestContext& context,
                                                const CrudState& state,
                                                const EngineExecutableObjectRecord& trigger,
                                                std::string_view audit_table_leaf) {
  const auto target_name =
      LowerAscii(PayloadFieldValue(trigger.payload, "trigger_target_table_name:"));
  if (!target_name.empty() && target_name.find('.') != std::string::npos) {
    const std::string sibling_name = SiblingPresentedName(target_name, audit_table_leaf);
    const std::string resolved = ResolveVisibleTableByPresentedName(context, sibling_name);
    if (!resolved.empty()) return resolved;
    return {};
  }
  return FindVisibleTableByName(state, context, audit_table_leaf);
}

inline std::string ResolveTriggerAuditSequenceUuid(const EngineRequestContext& context,
                                                   const EngineExecutableObjectRecord& trigger,
                                                   std::string_view audit_sequence_leaf) {
  const auto target_name =
      LowerAscii(PayloadFieldValue(trigger.payload, "trigger_target_table_name:"));
  if (!target_name.empty() && target_name.find('.') != std::string::npos) {
    const std::string sibling_name = SiblingPresentedName(target_name, audit_sequence_leaf);
    const std::string resolved = ResolveVisibleSequenceByPresentedName(context, sibling_name);
    if (!resolved.empty()) return resolved;
  }
  return ResolveVisibleSequenceByPresentedName(context, std::string(audit_sequence_leaf));
}

inline std::string FindVisibleTableNameByUuid(const CrudState& state,
                                              const EngineRequestContext& context,
                                              const std::string& table_uuid) {
  for (const auto& table : state.tables) {
    if (!CrudCreatorVisible(state,
                            table.creator_tx,
                            table.event_sequence,
                            context.local_transaction_id)) {
      continue;
    }
    if (table.table_uuid == table_uuid) return table.default_name;
  }
  return {};
}

inline DmlExecutableTriggerRuntimeResult ResolveTriggerCrudState(
    const EngineRequestContext& context,
    const CrudState& scoped_state,
    const std::string& target_table_uuid,
    std::string_view required_table_name,
    CrudState* trigger_state) {
  DmlExecutableTriggerRuntimeResult result;
  *trigger_state = scoped_state;

  const auto loaded = LoadMgaRelationStoreState(context);
  if (!loaded.ok) {
    result.ok = false;
    result.diagnostic = loaded.diagnostic;
    return result;
  }
  *trigger_state = BuildCrudCompatibilityStateFromMga(loaded.state);
  result.evidence.push_back({"trigger_relation_state_scope", "full_reload_for_trigger_dispatch"});
  return result;
}

inline DmlExecutableTriggerRuntimeResult InsertAuditRows(
    const EngineRequestContext& context,
    const std::string& audit_table_uuid,
    std::vector<EngineRowValue> rows) {
  DmlExecutableTriggerRuntimeResult result;
  if (rows.empty()) return result;
  EngineInsertRowsRequest insert;
  insert.context = context;
  insert.target_table.uuid.canonical = audit_table_uuid;
  insert.target_table.object_kind = "table";
  insert.input_rows = std::move(rows);
  insert.require_generated_row_uuid = true;
  insert.option_envelopes.push_back("trigger_runtime_internal:true");
  insert.option_envelopes.push_back("policy:executable.side_effect:allow");
  auto inserted = EngineInsertRows(insert);
  result.ok = inserted.ok;
  if (!inserted.ok) {
    if (!inserted.diagnostics.empty()) {
      result.diagnostic = inserted.diagnostics.front();
    } else {
      result.diagnostic = EngineApiDiagnostic{
          "TRIGGER.RUNTIME.AUDIT_INSERT_FAILED",
          "trigger.runtime.audit_insert_failed",
          "audit insert failed without diagnostic",
          true};
    }
    return result;
  }
  result.fired_count = static_cast<std::size_t>(inserted.inserted_count);
  result.evidence.push_back({"trigger_runtime_audit_rows_inserted",
                             std::to_string(inserted.inserted_count)});
  return result;
}

inline DmlExecutableTriggerRuntimeResult LoadExecutableState(
    const EngineRequestContext& context,
    EngineExecutableObjectLifecycleState* state) {
  DmlExecutableTriggerRuntimeResult result;
  const auto loaded = LoadExecutableObjectLifecycleStateForRuntimeDispatch(context);
  if (!loaded.ok) {
    result.ok = false;
    result.diagnostic = loaded.diagnostic;
    return result;
  }
  *state = loaded.state;
  return result;
}

inline std::map<std::string, bool>& ActiveTriggerDescriptorCache() {
  static thread_local std::map<std::string, bool> cache;
  return cache;
}

inline std::string ActiveTriggerDescriptorCacheKey(
    const EngineRequestContext& context,
    const std::string& target_table_uuid) {
  std::string key;
  key.reserve(context.database_path.size() + target_table_uuid.size() + 192);
  key += context.database_uuid.canonical;
  key.push_back('|');
  key += context.database_path;
  key.push_back('|');
  key += target_table_uuid;
  key.push_back('|');
  key += context.principal_uuid.canonical;
  key.push_back('|');
  key += context.current_role_uuid.canonical;
  key.push_back('|');
  key += std::to_string(context.catalog_generation_id);
  key.push_back('|');
  key += std::to_string(context.security_epoch);
  key.push_back('|');
  key += std::to_string(context.resource_epoch);
  return key;
}

inline void TrimActiveTriggerDescriptorCacheIfNeeded() {
  auto& cache = ActiveTriggerDescriptorCache();
  constexpr std::size_t kMaxEntries = 4096;
  constexpr std::size_t kTrimEntries = 512;
  if (cache.size() <= kMaxEntries) return;
  auto erase_end = cache.begin();
  for (std::size_t count = 0;
       count < kTrimEntries && erase_end != cache.end();
       ++count) {
    ++erase_end;
  }
  cache.erase(cache.begin(), erase_end);
}

inline bool ScanActiveTableTriggerDescriptors(const EngineRequestContext& context,
                                              const std::string& target_table_uuid) {
  const auto loaded = LoadExecutableObjectLifecycleStateForRuntimeDispatch(context);
  if (!loaded.ok) return false;
  auto state = loaded.state;
  for (const auto& object : state.objects) {
    if (LowerAscii(object.object_kind) != "trigger") continue;
    if (object.lifecycle_state != "active" || object.deleted || object.invalidated) continue;
    const auto descriptor_target_uuid =
        PayloadFieldValue(object.payload, "trigger_target_table_uuid:");
    if (!descriptor_target_uuid.empty() && descriptor_target_uuid == target_table_uuid) {
      return true;
    }
    if (descriptor_target_uuid.empty()) {
      const auto descriptor_target_name =
          PayloadFieldValue(object.payload, "trigger_target_table_name:");
      if (descriptor_target_name.empty()) {
        continue;
      }
      const std::string resolved_target_uuid =
          ResolveVisibleTableByPresentedName(context, descriptor_target_name);
      if (resolved_target_uuid == target_table_uuid) {
        return true;
      }
    }
  }
  return false;
}

inline bool HasActiveTableTriggerDescriptors(const EngineRequestContext& context,
                                             const std::string& target_table_uuid) {
  if (target_table_uuid.empty()) return false;
  const std::string cache_key =
      ActiveTriggerDescriptorCacheKey(context, target_table_uuid);
  auto& cache = ActiveTriggerDescriptorCache();
  const auto cached = cache.find(cache_key);
  if (cached != cache.end()) {
    return cached->second;
  }
  const bool present = ScanActiveTableTriggerDescriptors(context, target_table_uuid);
  cache.emplace(cache_key, present);
  TrimActiveTriggerDescriptorCacheIfNeeded();
  return present;
}

inline DmlExecutableTriggerRuntimeResult FireAfterInsertTableTriggers(
    const EngineRequestContext& context,
    const CrudState& crud_state,
    const std::string& target_table_uuid,
    const std::vector<CrudRowVersionRecord>& inserted_rows,
    const std::vector<std::string>& caller_options) {
  DmlExecutableTriggerRuntimeResult result;
  if (inserted_rows.empty() || OptionPresent(caller_options, "trigger_runtime_internal:true")) {
    return result;
  }
  EngineExecutableObjectLifecycleState executable_state;
  result = LoadExecutableState(context, &executable_state);
  if (!result.ok) return result;
  CrudState trigger_state;
  auto state_result =
      ResolveTriggerCrudState(context, crud_state, target_table_uuid, "trig_audit", &trigger_state);
  if (!state_result.ok) return state_result;
  result.evidence.insert(result.evidence.end(),
                         state_result.evidence.begin(),
                         state_result.evidence.end());
  const std::string target_table_name =
      FindVisibleTableNameByUuid(trigger_state, context, target_table_uuid);
  std::string audit_table_uuid;
  std::string audit_sequence_uuid;
  std::vector<EngineRowValue> audit_rows;
  for (const auto& trigger : executable_state.objects) {
    if (!TriggerDescriptorMatches(trigger,
                                  context,
                                  target_table_uuid,
                                  target_table_name,
                                  "insert",
                                  "row",
                                  "sbsql.compiled.trigger.audit_after_insert_row.v1")) {
      continue;
    }
    if (audit_table_uuid.empty()) {
      audit_table_uuid = ResolveTriggerAuditTableUuid(context, trigger_state, trigger, "trig_audit");
      if (audit_table_uuid.empty()) return result;
      audit_sequence_uuid =
          ResolveTriggerAuditSequenceUuid(context, trigger, "trig_audit_seq");
      if (audit_sequence_uuid.empty()) {
        result.ok = false;
        result.diagnostic = EngineApiDiagnostic{
            "TRIGGER.RUNTIME.SEQUENCE_UNRESOLVED",
            "trigger.runtime.sequence_unresolved",
            "compiled trigger audit sequence could not be resolved",
            true};
        return result;
      }
      result.evidence.push_back({"trigger_runtime_audit_sequence_uuid", audit_sequence_uuid});
    }
    for (const auto& row : inserted_rows) {
      std::uint64_t audit_id = 0;
      if (!NextSequenceAuditId(context, audit_sequence_uuid, &audit_id, &result.diagnostic)) {
        result.ok = false;
        return result;
      }
      audit_rows.push_back(AuditRow(audit_id,
                                    "INSERT",
                                    ValueFor(row.values, "item_id"),
                                    {},
                                    true,
                                    ValueFor(row.values, "item_price"),
                                    false,
                                    "item inserted"));
    }
    result.evidence.push_back({"trigger_descriptor_executed", trigger.object_uuid});
  }
  auto inserted = InsertAuditRows(context, audit_table_uuid, std::move(audit_rows));
  if (!inserted.ok) return inserted;
  result.fired_count += inserted.fired_count;
  result.evidence.insert(result.evidence.end(), inserted.evidence.begin(), inserted.evidence.end());
  return result;
}

inline DmlExecutableTriggerRuntimeResult FireAfterUpdateTableTriggers(
    const EngineRequestContext& context,
    const CrudState& crud_state,
    const std::string& target_table_uuid,
    const std::vector<DmlTriggerUpdateRowImage>& updated_rows,
    const std::vector<std::string>& caller_options) {
  DmlExecutableTriggerRuntimeResult result;
  if (updated_rows.empty() || OptionPresent(caller_options, "trigger_runtime_internal:true")) {
    return result;
  }
  EngineExecutableObjectLifecycleState executable_state;
  result = LoadExecutableState(context, &executable_state);
  if (!result.ok) return result;
  CrudState trigger_state;
  auto state_result =
      ResolveTriggerCrudState(context, crud_state, target_table_uuid, "trig_audit", &trigger_state);
  if (!state_result.ok) return state_result;
  result.evidence.insert(result.evidence.end(),
                         state_result.evidence.begin(),
                         state_result.evidence.end());
  const std::string target_table_name =
      FindVisibleTableNameByUuid(trigger_state, context, target_table_uuid);
  std::string audit_table_uuid;
  std::string audit_sequence_uuid;
  std::vector<EngineRowValue> audit_rows;
  for (const auto& trigger : executable_state.objects) {
    if (TriggerDescriptorMatches(trigger,
                                 context,
                                 target_table_uuid,
                                 target_table_name,
                                 "update",
                                 "row",
                                 "sbsql.compiled.trigger.audit_after_update_row.v1")) {
      if (audit_table_uuid.empty()) {
        audit_table_uuid = ResolveTriggerAuditTableUuid(context, trigger_state, trigger, "trig_audit");
        if (audit_table_uuid.empty()) return result;
        audit_sequence_uuid =
            ResolveTriggerAuditSequenceUuid(context, trigger, "trig_audit_seq");
        if (audit_sequence_uuid.empty()) {
          result.ok = false;
          result.diagnostic = EngineApiDiagnostic{
              "TRIGGER.RUNTIME.SEQUENCE_UNRESOLVED",
              "trigger.runtime.sequence_unresolved",
              "compiled trigger audit sequence could not be resolved",
              true};
          return result;
        }
        result.evidence.push_back({"trigger_runtime_audit_sequence_uuid", audit_sequence_uuid});
      }
      for (const auto& row : updated_rows) {
        std::uint64_t audit_id = 0;
        if (!NextSequenceAuditId(context, audit_sequence_uuid, &audit_id, &result.diagnostic)) {
          result.ok = false;
          return result;
        }
        audit_rows.push_back(AuditRow(audit_id,
                                      "UPDATE",
                                      ValueFor(row.new_values, "item_id"),
                                      ValueFor(row.old_values, "item_price"),
                                      false,
                                      ValueFor(row.new_values, "item_price"),
                                      false,
                                      "item price changed"));
      }
      result.evidence.push_back({"trigger_descriptor_executed", trigger.object_uuid});
    } else if (TriggerDescriptorMatches(trigger,
                                        context,
                                        target_table_uuid,
                                        target_table_name,
                                        "update",
                                        "statement",
                                        "sbsql.compiled.trigger.audit_after_update_statement.v1")) {
      if (audit_table_uuid.empty()) {
        audit_table_uuid = ResolveTriggerAuditTableUuid(context, trigger_state, trigger, "trig_audit");
        if (audit_table_uuid.empty()) return result;
        audit_sequence_uuid =
            ResolveTriggerAuditSequenceUuid(context, trigger, "trig_audit_seq");
        if (audit_sequence_uuid.empty()) {
          result.ok = false;
          result.diagnostic = EngineApiDiagnostic{
              "TRIGGER.RUNTIME.SEQUENCE_UNRESOLVED",
              "trigger.runtime.sequence_unresolved",
              "compiled trigger audit sequence could not be resolved",
              true};
          return result;
        }
        result.evidence.push_back({"trigger_runtime_audit_sequence_uuid", audit_sequence_uuid});
      }
      std::uint64_t audit_id = 0;
      if (!NextSequenceAuditId(context, audit_sequence_uuid, &audit_id, &result.diagnostic)) {
        result.ok = false;
        return result;
      }
      audit_rows.push_back(AuditRow(audit_id,
                                    "UPDATE_STMT",
                                    {},
                                    {},
                                    true,
                                    {},
                                    true,
                                    "statement-level update audit"));
      result.evidence.push_back({"trigger_descriptor_executed", trigger.object_uuid});
    }
  }
  auto inserted = InsertAuditRows(context, audit_table_uuid, std::move(audit_rows));
  if (!inserted.ok) return inserted;
  result.fired_count += inserted.fired_count;
  result.evidence.insert(result.evidence.end(), inserted.evidence.begin(), inserted.evidence.end());
  return result;
}

inline DmlExecutableTriggerRuntimeResult FireAfterDeleteTableTriggers(
    const EngineRequestContext& context,
    const CrudState& crud_state,
    const std::string& target_table_uuid,
    const std::vector<CrudRowVersionRecord>& deleted_rows,
    const std::vector<std::string>& caller_options) {
  DmlExecutableTriggerRuntimeResult result;
  if (deleted_rows.empty() || OptionPresent(caller_options, "trigger_runtime_internal:true")) {
    return result;
  }
  EngineExecutableObjectLifecycleState executable_state;
  result = LoadExecutableState(context, &executable_state);
  if (!result.ok) return result;
  CrudState trigger_state;
  auto state_result =
      ResolveTriggerCrudState(context, crud_state, target_table_uuid, "trig_audit", &trigger_state);
  if (!state_result.ok) return state_result;
  result.evidence.insert(result.evidence.end(),
                         state_result.evidence.begin(),
                         state_result.evidence.end());
  const std::string target_table_name =
      FindVisibleTableNameByUuid(trigger_state, context, target_table_uuid);
  std::string audit_table_uuid;
  std::string audit_sequence_uuid;
  std::vector<EngineRowValue> audit_rows;
  for (const auto& trigger : executable_state.objects) {
    if (!TriggerDescriptorMatches(trigger,
                                  context,
                                  target_table_uuid,
                                  target_table_name,
                                  "delete",
                                  "row",
                                  "sbsql.compiled.trigger.audit_after_delete_row.v1")) {
      continue;
    }
    if (audit_table_uuid.empty()) {
      audit_table_uuid = ResolveTriggerAuditTableUuid(context, trigger_state, trigger, "trig_audit");
      if (audit_table_uuid.empty()) return result;
      audit_sequence_uuid =
          ResolveTriggerAuditSequenceUuid(context, trigger, "trig_audit_seq");
      if (audit_sequence_uuid.empty()) {
        result.ok = false;
        result.diagnostic = EngineApiDiagnostic{
            "TRIGGER.RUNTIME.SEQUENCE_UNRESOLVED",
            "trigger.runtime.sequence_unresolved",
            "compiled trigger audit sequence could not be resolved",
            true};
        return result;
      }
      result.evidence.push_back({"trigger_runtime_audit_sequence_uuid", audit_sequence_uuid});
    }
    for (const auto& row : deleted_rows) {
      std::uint64_t audit_id = 0;
      if (!NextSequenceAuditId(context, audit_sequence_uuid, &audit_id, &result.diagnostic)) {
        result.ok = false;
        return result;
      }
      audit_rows.push_back(AuditRow(audit_id,
                                    "DELETE",
                                    ValueFor(row.values, "item_id"),
                                    ValueFor(row.values, "item_price"),
                                    false,
                                    {},
                                    true,
                                    "item deleted"));
    }
    result.evidence.push_back({"trigger_descriptor_executed", trigger.object_uuid});
  }
  auto inserted = InsertAuditRows(context, audit_table_uuid, std::move(audit_rows));
  if (!inserted.ok) return inserted;
  result.fired_count += inserted.fired_count;
  result.evidence.insert(result.evidence.end(), inserted.evidence.begin(), inserted.evidence.end());
  return result;
}

}  // namespace scratchbird::engine::internal_api::dml_trigger_runtime
