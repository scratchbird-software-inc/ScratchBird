// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by create_api.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
  record.base_descriptor_kind = descriptor.descriptor_kind.empty() ? "scalar" : descriptor.descriptor_kind;
  record.base_canonical_type_name = descriptor.canonical_type_name;
  record.base_encoded_descriptor = descriptor.encoded_descriptor;
  if (record.base_canonical_type_name.empty() && DomainUuidFromDescriptor(descriptor).empty()) {
    return MakeCrudDiagnosticResult<EngineCreateDomainResult>(
        request.context,
        "ddl.create_domain",
        MakeInvalidRequestDiagnostic("ddl.create_domain", "base_canonical_type_name_required"));
  }
  record.nullable = true;
  for (const auto& profile : request.policy_profile.encoded_profiles) {
    if (profile == "nullable:false") { record.nullable = false; }
    else if (StartsWithLocal(profile, "numeric_metadata:")) { record.numeric_metadata = profile.substr(17); }
    else if (StartsWithLocal(profile, "domain_cast_policy:")) { record.cast_policy_envelope = profile.substr(19); }
    else if (StartsWithLocal(profile, "domain_mutation_policy:")) { record.mutation_policy_envelope = profile.substr(23); }
    else if (StartsWithLocal(profile, "domain_masking_policy:")) { record.masking_policy_envelope = profile.substr(22); }
    else if (StartsWithLocal(profile, "domain_visibility_policy:")) { record.visibility_policy_envelope = profile.substr(25); }
    else if (StartsWithLocal(profile, "domain_encryption_policy:")) { record.encryption_policy_ref = profile.substr(25); }
    else if (StartsWithLocal(profile, "domain_reference_alias:")) {
      if (!record.reference_alias_envelope.empty()) { record.reference_alias_envelope.push_back('|'); }
      record.reference_alias_envelope.append(profile.substr(19));
    }
  }
  for (const auto& profile : request.compatibility_profile.encoded_profiles) {
    if (StartsWithLocal(profile, "reference_alias:")) {
      if (!record.reference_alias_envelope.empty()) { record.reference_alias_envelope.push_back('|'); }
      record.reference_alias_envelope.append(profile.substr(12));
    }
  }
  for (const auto& option : request.option_envelopes) {
    if (option == "nullable:true") {
      record.nullable = true;
    } else if (option == "nullable:false") {
      record.nullable = false;
    } else if (StartsWithLocal(option, "default_expression:")) {
      record.default_expression_envelope = option.substr(19);
    } else if (StartsWithLocal(option, "check_constraint:")) {
      record.check_constraint_envelope = option.substr(17);
      if (!IsSupportedDomainCheckEnvelope(record.check_constraint_envelope)) {
        return MakeCrudDiagnosticResult<EngineCreateDomainResult>(
            request.context,
            "ddl.create_domain",
            UnsupportedCrudFeatureDiagnostic("ddl.create_domain", "unsupported_domain_check_constraint"));
      }
    } else if (StartsWithLocal(option, "collation:")) {
      record.charset_or_collation_ref = option.substr(10);
    } else if (StartsWithLocal(option, "charset:")) {
      record.charset_or_collation_ref = option.substr(8);
    } else if (StartsWithLocal(option, "cast_policy:")) {
      record.cast_policy_envelope = option.substr(12);
    } else if (StartsWithLocal(option, "mutation_policy:")) {
      record.mutation_policy_envelope = option.substr(16);
    } else if (StartsWithLocal(option, "masking_policy:")) {
      record.masking_policy_envelope = option.substr(15);
    } else if (StartsWithLocal(option, "visibility_policy:")) {
      record.visibility_policy_envelope = option.substr(18);
    } else if (StartsWithLocal(option, "encryption_policy:")) {
      record.encryption_policy_ref = option.substr(18);
    } else if (StartsWithLocal(option, "driver_metadata:")) {
      record.driver_metadata_envelope = option.substr(16);
    } else if (StartsWithLocal(option, "wire_metadata:")) {
      record.wire_metadata_envelope = option.substr(14);
    } else if (StartsWithLocal(option, "element_path:")) {
      record.element_path_envelope = option.substr(13);
    } else if (StartsWithLocal(option, "method_binding:")) {
      record.method_binding_envelope = option.substr(15);
    } else if (StartsWithLocal(option, "comment:")) {
      record.comment_envelope = option.substr(8);
    } else if (StartsWithLocal(option, "localized_comment:")) {
      if (!record.comment_envelope.empty()) { record.comment_envelope.push_back('|'); }
      record.comment_envelope.append(option.substr(18));
    } else if (StartsWithLocal(option, "reference_alias:")) {
      if (!record.reference_alias_envelope.empty()) { record.reference_alias_envelope.push_back('|'); }
      record.reference_alias_envelope.append(option.substr(12));
    } else if (!option.empty()) {
      return MakeCrudDiagnosticResult<EngineCreateDomainResult>(
          request.context,
          "ddl.create_domain",
          UnsupportedCrudFeatureDiagnostic("ddl.create_domain", "unsupported_domain_option"));
    }
  }
  for (const auto& constraint : request.constraints) {
    if (constraint.constraint_kind == "check" && !constraint.canonical_constraint_envelope.empty()) {
      if (!IsSupportedDomainCheckEnvelope(constraint.canonical_constraint_envelope)) {
        return MakeCrudDiagnosticResult<EngineCreateDomainResult>(
            request.context,
            "ddl.create_domain",
            UnsupportedCrudFeatureDiagnostic("ddl.create_domain", "unsupported_domain_check_constraint"));
      }
      record.check_constraint_envelope = constraint.canonical_constraint_envelope;
    } else if (!constraint.constraint_kind.empty()) {
      return MakeCrudDiagnosticResult<EngineCreateDomainResult>(
          request.context,
          "ddl.create_domain",
          UnsupportedCrudFeatureDiagnostic("ddl.create_domain", "unsupported_domain_constraint"));
    }
  }
  const std::string base_domain_uuid = DomainUuidFromDescriptor(descriptor);
  if (!base_domain_uuid.empty()) {
    if (base_domain_uuid == record.domain_uuid ||
        DomainChainContainsUuid(request.context, base_domain_uuid, record.domain_uuid, request.context.local_transaction_id)) {
      return MakeCrudDiagnosticResult<EngineCreateDomainResult>(
          request.context,
          "ddl.create_domain",
          MakeInvalidRequestDiagnostic("ddl.create_domain", "domain_base_cycle_detected"));
    }
    const auto base_domain = FindVisibleDomain(request.context, base_domain_uuid, request.context.local_transaction_id);
    if (!base_domain) {
      return MakeCrudDiagnosticResult<EngineCreateDomainResult>(
          request.context,
          "ddl.create_domain",
          MakeInvalidRequestDiagnostic("ddl.create_domain", "base_domain_not_visible"));
    }
    if (record.nullable && !base_domain->nullable) {
      return MakeCrudDiagnosticResult<EngineCreateDomainResult>(
          request.context,
          "ddl.create_domain",
          MakeInvalidRequestDiagnostic("ddl.create_domain", "domain_chain_cannot_widen_nullability"));
    }
    record.base_descriptor_uuid = base_domain_uuid;
    record.base_descriptor_kind = "domain";
    record.base_canonical_type_name = base_domain->base_canonical_type_name;
    record.base_encoded_descriptor = DomainDescriptor(*base_domain).encoded_descriptor;
  }
  SetDomainValidationHookStatus(&record);
  const auto appended = AppendDomainEvent(request.context, MakeDomainCreateEvent(record));
  if (appended.error) {
    return MakeCrudDiagnosticResult<EngineCreateDomainResult>(request.context, "ddl.create_domain", appended);
  }
  const auto domain_name_appended = PersistNameRegistryEntriesForObject(request.context,
                                                                       "ddl.create_domain",
                                                                       record.domain_uuid,
                                                                       "domain",
                                                                       record.schema_uuid,
                                                                       request.localized_names,
                                                                       record.default_name);
  if (domain_name_appended.error) {
    return MakeCrudDiagnosticResult<EngineCreateDomainResult>(request.context, "ddl.create_domain", domain_name_appended);
  }
  auto result = MakeCrudSuccessResult<EngineCreateDomainResult>(request.context, "ddl.create_domain");
  result.primary_object.uuid.canonical = record.domain_uuid;
  result.primary_object.object_kind = "domain";
  result.catalog_row_uuid.canonical = record.catalog_row_uuid;
  result.evidence.push_back({"domain_event", "domain_create"});
  result.evidence.push_back({"validation_hook", record.validation_hook_status});
  AddApiBehaviorRow(&result, {{"object_uuid", record.domain_uuid},
                              {"object_kind", "domain"},
                              {"schema_uuid", record.schema_uuid},
                              {"name", record.default_name},
                              {"state", "active"},
                              {"payload", "domain_event=domain_create"}});
  AddDdlPublicationResult(&result,
                          "ddl.create_domain",
                          "domain",
                          record.domain_uuid,
                          result.catalog_row_uuid.canonical,
                          "domain_event");
  return result;
}

namespace {

std::string CreateSequenceOptionValue(const EngineCreateSequenceRequest& request,
                                      std::string_view prefix) {
  return SecurityOptionValue(request, std::string(prefix));
}

bool ParseSequenceI64(std::string value, std::int64_t* out) {
  if (out == nullptr || value.empty() || value == "default") return false;
  bool negative = false;
  std::size_t index = 0;
  if (value[index] == '-' || value[index] == '+') {
    negative = value[index] == '-';
    ++index;
    if (index == value.size()) return false;
  }
  std::int64_t parsed = 0;
  for (; index < value.size(); ++index) {
    const unsigned char ch = static_cast<unsigned char>(value[index]);
    if (!std::isdigit(ch)) return false;
    const int digit = static_cast<int>(ch - '0');
    if (parsed > (std::numeric_limits<std::int64_t>::max() - digit) / 10) return false;
    parsed = parsed * 10 + digit;
  }
  *out = negative ? -parsed : parsed;
  return true;
}

std::uint64_t ParseSequenceU64(std::string value, std::uint64_t fallback) {
  if (value.empty() || value == "default" || value == "none") return fallback;
  std::uint64_t parsed = 0;
  for (const unsigned char ch : value) {
    if (!std::isdigit(ch)) return fallback;
    parsed = parsed * 10u + static_cast<std::uint64_t>(ch - '0');
  }
  return parsed == 0 ? fallback : parsed;
}

scratchbird::engine::sblr::SblrExecutionContext SequenceSblrContext(
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

scratchbird::engine::sblr::SblrSequenceDefinition BuildSequenceDefinition(
    const EngineCreateSequenceRequest& request,
    const std::string& sequence_uuid) {
  scratchbird::engine::sblr::SblrSequenceDefinition definition;
  definition.sequence_uuid = sequence_uuid;
  std::string descriptor = CreateSequenceOptionValue(request, "sequence_descriptor:");
  const auto fields = DescriptorFieldsLocal(descriptor);
  const std::string type =
      CreateSequenceOptionValue(request, "sequence_type:").empty()
          ? DescriptorFieldLocal(fields, {"type", "descriptor", "canonical_type"})
          : CreateSequenceOptionValue(request, "sequence_type:");
  definition.descriptor_id = type.empty() ? "int64" : LowerAsciiLocal(type);
  std::int64_t parsed = 0;
  if (ParseSequenceI64(CreateSequenceOptionValue(request, "sequence_start_value:"), &parsed) ||
      ParseSequenceI64(DescriptorFieldLocal(fields, {"start", "start_value"}), &parsed)) {
    definition.start_value = parsed;
  }
  if (ParseSequenceI64(CreateSequenceOptionValue(request, "sequence_increment:"), &parsed) ||
      ParseSequenceI64(DescriptorFieldLocal(fields, {"increment", "increment_by"}), &parsed)) {
    definition.increment = parsed;
  }
  if (ParseSequenceI64(CreateSequenceOptionValue(request, "sequence_min_value:"), &parsed) ||
      ParseSequenceI64(DescriptorFieldLocal(fields, {"min", "minvalue"}), &parsed)) {
    definition.minimum_value = parsed;
  }
  if (ParseSequenceI64(CreateSequenceOptionValue(request, "sequence_max_value:"), &parsed) ||
      ParseSequenceI64(DescriptorFieldLocal(fields, {"max", "maxvalue"}), &parsed)) {
    definition.maximum_value = parsed;
  }
  const bool no_cache = SecurityOptionBool(request, "sequence_no_cache:", false) ||
                        LowerAsciiLocal(DescriptorFieldLocal(fields, {"cache"})) == "none";
  if (no_cache) {
    definition.cache_size = 1;
  } else {
    definition.cache_size = ParseSequenceU64(
        CreateSequenceOptionValue(request, "sequence_cache:").empty()
            ? DescriptorFieldLocal(fields, {"cache"})
            : CreateSequenceOptionValue(request, "sequence_cache:"),
        1);
  }
  const std::string cycle_option = CreateSequenceOptionValue(request, "sequence_cycle:").empty()
                                       ? DescriptorFieldLocal(fields, {"cycle"})
                                       : CreateSequenceOptionValue(request, "sequence_cycle:");
  definition.cycle = LowerAsciiLocal(cycle_option) == "true" ||
                     LowerAsciiLocal(cycle_option) == "1" ||
                     LowerAsciiLocal(cycle_option) == "yes";
  return definition;
}

EngineApiDiagnostic RegisterSequenceDefinitionAlias(
    const EngineCreateSequenceRequest& request,
    std::string canonical_sequence_uuid,
    std::string sequence_key) {
  if (sequence_key.empty()) {
    return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  }
  canonical_sequence_uuid = LowerAsciiLocal(std::move(canonical_sequence_uuid));
  sequence_key = LowerAsciiLocal(std::move(sequence_key));
  const auto registered = scratchbird::engine::sblr::RegisterSblrSequenceAlias(
      &scratchbird::engine::sblr::ProcessSblrSequenceRegistry(),
      std::move(canonical_sequence_uuid),
      std::move(sequence_key),
      SequenceSblrContext(request.context));
  if (registered.ok()) {
    return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  }
  const std::string detail = registered.diagnostics.empty()
                                 ? "sequence_definition_registration_failed"
                                 : registered.diagnostics.front().detail;
  return MakeInvalidRequestDiagnostic("ddl.create_sequence", detail);
}

}  // namespace

EngineCreateSequenceResult EngineCreateSequence(const EngineCreateSequenceRequest& request) {
  auto result = PersistedCreateWithNameRegistry<EngineCreateSequenceResult>(
      request, "ddl.create_sequence", "sequence", true, "created");
  if (!result.ok) { return result; }

  auto definition = BuildSequenceDefinition(request, result.primary_object.uuid.canonical);
  const auto registered = scratchbird::engine::sblr::RegisterSblrSequence(
      &scratchbird::engine::sblr::ProcessSblrSequenceRegistry(),
      definition,
      SequenceSblrContext(request.context));
  if (!registered.ok()) {
    const std::string detail = registered.diagnostics.empty()
                                   ? "sequence_definition_registration_failed"
                                   : registered.diagnostics.front().detail;
    return MakeCrudDiagnosticResult<EngineCreateSequenceResult>(
        request.context, "ddl.create_sequence", MakeInvalidRequestDiagnostic("ddl.create_sequence", detail));
  }
  auto diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  const std::string lookup_key = CreateSequenceOptionValue(request, "sequence_lookup_key:");
  diagnostic = RegisterSequenceDefinitionAlias(
      request, result.primary_object.uuid.canonical, lookup_key);
  if (diagnostic.error) {
    return MakeCrudDiagnosticResult<EngineCreateSequenceResult>(
        request.context, "ddl.create_sequence", diagnostic);
  }
  const std::string name = ApiBehaviorPrimaryName(request, "sequence");
  if (!name.empty() && LowerAsciiLocal(name) != LowerAsciiLocal(lookup_key)) {
    diagnostic = RegisterSequenceDefinitionAlias(
        request, result.primary_object.uuid.canonical, name);
    if (diagnostic.error) {
      return MakeCrudDiagnosticResult<EngineCreateSequenceResult>(
          request.context, "ddl.create_sequence", diagnostic);
    }
  }
  result.evidence.push_back({"sequence_runtime_definition", result.primary_object.uuid.canonical});
  if (!lookup_key.empty()) {
    result.evidence.push_back({"sequence_runtime_lookup_key", LowerAsciiLocal(lookup_key)});
  }
  return result;
}

EngineCreateViewResult EngineCreateView(const EngineCreateViewRequest& request) {
  const std::string object_kind =
      request.target_object.object_kind.empty() ? "view" : request.target_object.object_kind;
  if (IsEngineGlobalAggregateViewCreateRequest(request)) {
    const auto prepared = PrepareEngineGlobalAggregateViewCreate(request);
    if (!prepared.ok) {
      return MakeApiBehaviorDiagnostic<EngineCreateViewResult>(
          request.context, "ddl.create_view", prepared.diagnostic);
    }
    EngineCreateViewRequest effective = request;
    effective.operation_id.clear();
    effective.target_object.uuid = prepared.descriptor.view_uuid;
    effective.target_object.object_kind = "view";
    effective.related_objects.clear();
    effective.descriptors.clear();
    effective.columns.clear();
    effective.constraints.clear();
    effective.indexes.clear();
    effective.rows.clear();
    effective.assignments.clear();
    effective.projection.canonical_projection_envelopes.clear();
    effective.option_envelopes = prepared.canonical_persisted_options;
    auto result = PersistedCreateWithNameRegistry<EngineCreateViewResult>(
        effective, "ddl.create_view", "view", true, "created");
    if (result.ok) {
      AddApiBehaviorEvidence(&result,
                             "global_aggregate_view_marker",
                             kEngineGlobalAggregateViewMarkerV1);
      AddApiBehaviorEvidence(
          &result,
          "global_aggregate_view_descriptor_uuid",
          prepared.descriptor.view_descriptor_uuid.canonical);
      AddApiBehaviorEvidence(
          &result,
          "global_aggregate_view_descriptor_generation",
          std::to_string(
              prepared.descriptor.view_descriptor_generation));
      AddApiBehaviorEvidence(
          &result,
          "global_aggregate_view_create_or_alter_resolution",
          prepared.altered_existing ? "alter" : "create");
      AddApiBehaviorEvidence(&result,
                             "global_aggregate_view_parser_sql",
                             "false");
    }
    return result;
  }
  if (IsEngineRelationProjectionViewCreateRequest(request)) {
    const auto prepared =
        PrepareEngineRelationProjectionViewCreate(request);
    if (!prepared.ok) {
      return MakeApiBehaviorDiagnostic<EngineCreateViewResult>(
          request.context, "ddl.create_view", prepared.diagnostic);
    }
    EngineCreateViewRequest effective = request;
    effective.operation_id.clear();
    effective.target_object.uuid = prepared.descriptor.view_uuid;
    effective.target_object.object_kind = "view";
    effective.related_objects.clear();
    effective.descriptors.clear();
    effective.columns.clear();
    effective.constraints.clear();
    effective.indexes.clear();
    effective.rows.clear();
    effective.assignments.clear();
    effective.projection.canonical_projection_envelopes.clear();
    effective.option_envelopes = prepared.canonical_persisted_options;
    auto result = PersistedCreateWithNameRegistry<EngineCreateViewResult>(
        effective, "ddl.create_view", "view", true, "created");
    if (result.ok) {
      AddApiBehaviorEvidence(&result,
                             "relation_projection_view_marker",
                             prepared.descriptor.marker);
      AddApiBehaviorEvidence(
          &result,
          "relation_projection_view_descriptor_uuid",
          prepared.descriptor.view_descriptor_uuid.canonical);
      AddApiBehaviorEvidence(
          &result,
          "relation_projection_view_descriptor_generation",
          std::to_string(
              prepared.descriptor.view_descriptor_generation));
      AddApiBehaviorEvidence(
          &result,
          "relation_projection_view_source_resource_epoch",
          std::to_string(prepared.descriptor.source_resource_epoch));
      AddApiBehaviorEvidence(&result,
                             "relation_projection_view_output_count",
                             std::to_string(
                                 prepared.descriptor.outputs.size()));
      AddApiBehaviorEvidence(&result,
                             "relation_projection_view_parser_sql",
                             "false");
      AddApiBehaviorEvidence(&result,
                             "relation_projection_view_row_storage",
                             "none_mga_source_scan_only");
    }
    return result;
  }
  if (IsRelationDescriptorProjectionViewCreateRequest(request)) {
    const auto admitted =
        ValidateRelationDescriptorProjectionViewCreate(request);
    if (admitted.error) {
      return MakeApiBehaviorDiagnostic<EngineCreateViewResult>(
          request.context, "ddl.create_view", admitted);
    }
    EngineCreateViewRequest effective = request;
    effective.option_envelopes =
        CanonicalRelationDescriptorProjectionViewOptions(request);
    auto result = PersistedCreateWithNameRegistry<EngineCreateViewResult>(
        effective, "ddl.create_view", object_kind, true, "created");
    if (result.ok) {
      AddApiBehaviorEvidence(&result,
                             "catalog_projection_marker",
                             kRelationDescriptorProjectionMarkerV1);
      AddApiBehaviorEvidence(&result,
                             "catalog_projection_parser_sql",
                             "false");
    }
    return result;
  }
  return PersistedCreateWithNameRegistry<EngineCreateViewResult>(
      request, "ddl.create_view", object_kind, true, "created");
}

EngineCreateIndexTemplateResult EngineCreateIndexTemplate(const EngineCreateIndexTemplateRequest& request) {
  constexpr const char* kOperation = "ddl.create_index_template";
  EngineCreateIndexTemplateRequest effective = request;
  std::string object_kind = effective.target_object.object_kind;
  if (object_kind.empty()) {
    object_kind = SecurityOptionValue(effective, "index_template_kind:");
  }
  if (object_kind.empty()) { object_kind = "index_template"; }
  if (object_kind != "index_template" && object_kind != "component_template") {
    return MakeCrudDiagnosticResult<EngineCreateIndexTemplateResult>(
        effective.context,
        kOperation,
        MakeInvalidRequestDiagnostic(kOperation, "index_template_kind_invalid"));
  }
  effective.target_object.object_kind = object_kind;
  const bool template_document_present =
      SecurityOptionBool(effective, "template_document_present:", false) ||
      !SecurityOptionValue(effective, "template_document:").empty() ||
      !SecurityOptionValue(effective, "template_document_hash:").empty() ||
      !effective.descriptors.empty();
  if (!template_document_present) {
    return MakeCrudDiagnosticResult<EngineCreateIndexTemplateResult>(
        effective.context,
        kOperation,
        MakeInvalidRequestDiagnostic(kOperation, "template_document_required"));
  }
  effective.option_envelopes.push_back("index_template_kind:" + object_kind);
  auto result = PersistedCreateWithNameRegistry<EngineCreateIndexTemplateResult>(
      effective,
      kOperation,
      object_kind,
      true,
      "created");
  if (!result.ok) { return result; }
  AddApiBehaviorEvidence(&result, "index_template_descriptor_created", result.primary_object.uuid.canonical);
  AddApiBehaviorEvidence(&result, "index_template_kind", object_kind);
  AddApiBehaviorEvidence(&result, "template_document_bound", "descriptor_payload");
  AddApiBehaviorEvidence(&result, "ddl_catalog_route", object_kind == "component_template"
                                                   ? "sys.catalog.component_template"
                                                   : "sys.catalog.index_template");
  return result;
}

EngineCreateConstraintResult EngineCreateConstraint(const EngineCreateConstraintRequest& request) {
  constexpr const char* kOperation = "ddl.constraint.create";
  EngineCatalogApplyConstraintsRequest catalog_request;
  static_cast<EngineApiRequest&>(catalog_request) = request;
  catalog_request.operation_id = kOperation;
  const auto applied = EngineCatalogApplyConstraintsToObject(catalog_request);

  EngineCreateConstraintResult result;
  result.ok = applied.ok;
  result.operation_id = kOperation;
  result.diagnostics = applied.diagnostics;
  result.unsupported_features = applied.unsupported_features;
  result.evidence = applied.evidence;
  result.result_shape = applied.result_shape;
  result.primary_object = applied.primary_object;
  result.catalog_row_uuid = applied.catalog_row_uuid;
  result.transaction_uuid = applied.transaction_uuid;
  result.local_transaction_id = applied.local_transaction_id;
  result.embedded_trust_mode_observed = applied.embedded_trust_mode_observed;
  result.cluster_authority_required = applied.cluster_authority_required;
  result.bound_object_identity = applied.bound_object_identity;
  result.metadata_cache_epoch = applied.metadata_cache_epoch;
  if (result.ok) {
    result.evidence.push_back({"ddl_catalog_route", "sys.constraint_descriptor"});
    AddDdlPublicationResult(&result,
                            kOperation,
                            "constraint",
                            result.primary_object.uuid.canonical,
                            result.catalog_row_uuid.canonical,
                            "constraint_descriptor");
  }
  return result;
}

bool CreateOrAlterProcedureRouteRequested(const EngineApiRequest& request) {
  return SecurityOptionValue(request, "create_or_alter:") == "true" ||
         SecurityOptionValue(request, "procedure_create_mode:") ==
             "create_or_alter" ||
         SecurityOptionValue(request, "executable_descriptor_kind:") ==
             "create_or_alter_procedure";
}

template <typename TResult>
TResult ExecutableCreateLifecycleFailure(const EngineRequestContext& context,
                                         const std::string& operation_id,
                                         const EngineApiResult& lifecycle) {
  return MakeApiBehaviorDiagnostic<TResult>(
      context,
      operation_id,
      MakeInvalidRequestDiagnostic(
          operation_id,
          lifecycle.diagnostics.empty()
              ? "executable_lifecycle_create_failed"
              : lifecycle.diagnostics.front().code));
}

template <typename TResult>
TResult AdoptCreateOrAlterLifecycleResult(
    const EngineRequestContext& context,
    const std::string& operation_id,
    const std::string& object_kind,
    const EngineCreateExecutableObjectResult& lifecycle) {
  if (!lifecycle.ok) {
    return ExecutableCreateLifecycleFailure<TResult>(
        context, operation_id, lifecycle);
  }
  TResult result;
  static_cast<EngineApiResult&>(result) =
      static_cast<const EngineApiResult&>(lifecycle);
  result.operation_id = operation_id;
  // FillObjectResult supplies a lifecycle projection identifier in this
  // field. CREATE OR ALTER resolved to ALTER did not append a new DDL catalog
  // row, so do not expose that inherited projection as a create result.
  result.catalog_row_uuid.canonical.clear();
  result.primary_object.uuid.canonical =
      lifecycle.bound_object_identity.object_uuid.canonical;
  result.primary_object.object_kind = object_kind;
  AddApiBehaviorEvidence(
      &result, "executable_lifecycle", result.primary_object.uuid.canonical);
  AddApiBehaviorEvidence(&result,
                         "executable_generation",
                         std::to_string(lifecycle.executable_generation));
  AddApiBehaviorEvidence(&result,
                         "create_or_alter_catalog_mutation",
                         "no_create_or_name_append_on_alter");
  return result;
}

template <typename TResult, typename TRequest>
TResult PersistedExecutableCreateWithLifecycle(const TRequest& request,
                                               const std::string& operation_id,
                                               const std::string& object_kind) {
  TRequest effective = request;
  const bool create_or_alter =
      object_kind == "procedure" &&
      CreateOrAlterProcedureRouteRequested(effective);
  if (create_or_alter) {
    const auto exact_mga =
        ValidateExecutableObjectExactMgaSelector(effective.context);
    if (exact_mga.error) {
      return MakeApiBehaviorDiagnostic<TResult>(effective.context,
                                                operation_id,
                                                exact_mga);
    }
    const auto lifecycle_state =
        LoadExecutableObjectLifecycleState(effective.context);
    if (!lifecycle_state.ok) {
      return MakeApiBehaviorDiagnostic<TResult>(effective.context,
                                                operation_id,
                                                lifecycle_state.diagnostic);
    }

    const auto find_visible_procedure = [&](const std::string& object_uuid) {
      return std::find_if(
          lifecycle_state.state.objects.begin(),
          lifecycle_state.state.objects.end(),
          [&](const auto& object) {
            return object.object_uuid == object_uuid &&
                   object.object_kind == "procedure";
          });
    };

    bool alter_existing = false;
    bool resolved_existing_by_name = false;
    if (!effective.target_object.uuid.canonical.empty()) {
      const auto existing =
          find_visible_procedure(effective.target_object.uuid.canonical);
      if (existing == lifecycle_state.state.objects.end()) {
        return MakeApiBehaviorDiagnostic<TResult>(
            effective.context,
            operation_id,
            MakeInvalidRequestDiagnostic(
                operation_id,
                "create_or_alter_uuid_not_engine_resolved"));
      }
      alter_existing = true;
    } else {
      const auto resolved = ResolveNameRegistryPrivate(effective, "procedure");
      if (resolved.ok) {
        if (resolved.matches.size() != 1) {
          return MakeApiBehaviorDiagnostic<TResult>(
              effective.context,
              operation_id,
              MakeInvalidRequestDiagnostic(
                  operation_id,
                  "create_or_alter_name_resolution_ambiguous"));
        }
        const auto existing =
            find_visible_procedure(resolved.matches.front().object_uuid);
        if (existing == lifecycle_state.state.objects.end()) {
          return MakeApiBehaviorDiagnostic<TResult>(
              effective.context,
              operation_id,
              MakeInvalidRequestDiagnostic(
                  operation_id,
                  "create_or_alter_catalog_lifecycle_mismatch"));
        }
        effective.target_object.uuid.canonical = existing->object_uuid;
        alter_existing = true;
        resolved_existing_by_name = true;
      } else if (resolved.diagnostic.code != "CATALOG.NAME.NOT_FOUND") {
        return MakeApiBehaviorDiagnostic<TResult>(effective.context,
                                                  operation_id,
                                                  resolved.diagnostic);
      }
    }

    if (alter_existing) {
      EngineCreateExecutableObjectRequest executable_request;
      static_cast<EngineApiRequest&>(executable_request) = effective;
      executable_request.target_object.object_kind = object_kind;
      executable_request.target_schema = effective.target_schema;
      executable_request.localized_names = effective.localized_names;
      const auto lifecycle =
          EngineCreateExecutableObject(executable_request);
      auto result = AdoptCreateOrAlterLifecycleResult<TResult>(effective.context,
                                                               operation_id,
                                                               object_kind,
                                                               lifecycle);
      if (result.ok) {
        AddApiBehaviorEvidence(
            &result,
            "create_or_alter_binding",
            resolved_existing_by_name ? "engine_name_schema"
                                      : "engine_published_uuid");
      }
      return result;
    }

    // The create half owns identity allocation. A parser may reuse an
    // engine-published UUID for ALTER, but cannot invent a procedure UUID.
    effective.target_object.uuid.canonical = GenerateCrudEngineUuid("object");
    if (effective.target_object.uuid.canonical.empty()) {
      return MakeApiBehaviorDiagnostic<TResult>(
          effective.context,
          operation_id,
          MakeInvalidRequestDiagnostic(
              operation_id,
              "create_or_alter_identity_allocation_failed"));
    }
    effective.target_object.object_kind = object_kind;

    EngineCreateExecutableObjectRequest preflight_request;
    static_cast<EngineApiRequest&>(preflight_request) = effective;
    preflight_request.target_object.object_kind = object_kind;
    preflight_request.target_schema = effective.target_schema;
    preflight_request.localized_names = effective.localized_names;
    preflight_request.create_or_alter_identity_engine_allocated = true;
    const auto preflight =
        PreflightCreateExecutableObject(preflight_request);
    if (preflight.error) {
      return MakeApiBehaviorDiagnostic<TResult>(effective.context,
                                                operation_id,
                                                preflight);
    }
  }

  auto result = PersistedCreateWithNameRegistry<TResult>(
      effective, operation_id, object_kind, true, "created");
  if (!result.ok) { return result; }

  EngineCreateExecutableObjectRequest executable_request;
  static_cast<EngineApiRequest&>(executable_request) = effective;
  executable_request.target_object.uuid.canonical = result.primary_object.uuid.canonical;
  executable_request.target_object.object_kind = object_kind;
  executable_request.target_schema = effective.target_schema;
  executable_request.localized_names = effective.localized_names;
  executable_request.create_or_alter_identity_engine_allocated =
      create_or_alter;
  const auto lifecycle = EngineCreateExecutableObject(executable_request);
  if (!lifecycle.ok) {
    return ExecutableCreateLifecycleFailure<TResult>(
        effective.context, operation_id, lifecycle);
  }
  AddApiBehaviorEvidence(&result, "executable_lifecycle", lifecycle.bound_object_identity.object_uuid.canonical);
  AddApiBehaviorEvidence(&result, "executable_generation", std::to_string(lifecycle.executable_generation));
  for (const auto& evidence : lifecycle.evidence) {
    if (evidence.evidence_kind == "create_or_alter_resolution" ||
        evidence.evidence_kind == "create_or_alter_authority") {
      AddApiBehaviorEvidence(&result,
                             evidence.evidence_kind,
                             evidence.evidence_id);
    }
  }
  if (create_or_alter) {
    AddApiBehaviorEvidence(&result,
                           "create_or_alter_binding",
                           "engine_allocated_uuid");
    AddApiBehaviorEvidence(&result,
                           "create_or_alter_preflight",
                           "validated_before_catalog_persistence");
  }
  return result;
}

EngineCreateFunctionResult EngineCreateFunction(const EngineCreateFunctionRequest& request) {
  return PersistedExecutableCreateWithLifecycle<EngineCreateFunctionResult>(
      request, "ddl.create_function", "function");
}

EngineCreateProcedureResult EngineCreateProcedure(const EngineCreateProcedureRequest& request) {
  return PersistedExecutableCreateWithLifecycle<EngineCreateProcedureResult>(
      request, "ddl.create_procedure", "procedure");
}

EngineCreateTriggerResult EngineCreateTrigger(const EngineCreateTriggerRequest& request) {
  return PersistedExecutableCreateWithLifecycle<EngineCreateTriggerResult>(
      request, "ddl.create_trigger", "trigger");
}

}  // namespace scratchbird::engine::internal_api
