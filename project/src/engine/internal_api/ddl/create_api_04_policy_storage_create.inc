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
    scratchbird::engine::sblr::SblrSequenceDefinition definition,
    std::string sequence_key) {
  if (sequence_key.empty()) {
    return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  }
  definition.sequence_uuid = LowerAsciiLocal(std::move(sequence_key));
  const auto registered = scratchbird::engine::sblr::RegisterSblrSequence(
      &scratchbird::engine::sblr::ProcessSblrSequenceRegistry(),
      definition,
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
  auto diagnostic = RegisterSequenceDefinitionAlias(
      request, definition, result.primary_object.uuid.canonical);
  if (diagnostic.error) {
    return MakeCrudDiagnosticResult<EngineCreateSequenceResult>(
        request.context, "ddl.create_sequence", diagnostic);
  }
  const std::string lookup_key = CreateSequenceOptionValue(request, "sequence_lookup_key:");
  diagnostic = RegisterSequenceDefinitionAlias(request, definition, lookup_key);
  if (diagnostic.error) {
    return MakeCrudDiagnosticResult<EngineCreateSequenceResult>(
        request.context, "ddl.create_sequence", diagnostic);
  }
  const std::string name = ApiBehaviorPrimaryName(request, "sequence");
  if (!name.empty() && LowerAsciiLocal(name) != LowerAsciiLocal(lookup_key)) {
    diagnostic = RegisterSequenceDefinitionAlias(request, definition, name);
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
  return PersistedCreateWithNameRegistry<EngineCreateViewResult>(request, "ddl.create_view", "view", true, "created");
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
  }
  return result;
}

EngineCreateFunctionResult EngineCreateFunction(const EngineCreateFunctionRequest& request) {
  return PersistedCreateWithNameRegistry<EngineCreateFunctionResult>(request, "ddl.create_function", "function", true, "created");
}

EngineCreateProcedureResult EngineCreateProcedure(const EngineCreateProcedureRequest& request) {
  return PersistedCreateWithNameRegistry<EngineCreateProcedureResult>(request, "ddl.create_procedure", "procedure", true, "created");
}

EngineCreateTriggerResult EngineCreateTrigger(const EngineCreateTriggerRequest& request) {
  return PersistedCreateWithNameRegistry<EngineCreateTriggerResult>(request, "ddl.create_trigger", "trigger", true, "created");
}

}  // namespace scratchbird::engine::internal_api
