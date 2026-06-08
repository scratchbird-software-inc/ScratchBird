// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_domain_runtime.hpp"

#include "domain_support/domain_store.hpp"
#include "query/expression_api.hpp"

#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace {
namespace api = scratchbird::engine::internal_api;

api::EngineRequestContext ToEngineContext(const SblrExecutionContext& context) {
  api::EngineRequestContext out;
  out.database_path = context.database_path;
  out.database_uuid.canonical = context.database_uuid;
  out.cluster_uuid.canonical = context.cluster_uuid;
  out.node_uuid.canonical = context.node_uuid;
  out.principal_uuid.canonical = context.user_uuid;
  out.current_role_uuid.canonical = context.current_role_uuid;
  out.current_schema_uuid.canonical = context.current_schema_uuid;
  out.session_uuid.canonical = context.session_uuid;
  out.transaction_uuid.canonical = context.transaction_uuid;
  out.local_transaction_id = context.local_transaction_id;
  out.snapshot_visible_through_local_transaction_id = context.snapshot_visible_through_local_transaction_id;
  out.transaction_isolation_level = context.transaction_isolation_level;
  out.application_name = context.application_name;
  out.security_context_present = context.security_context_present;
  out.cluster_authority_available = context.cluster_authority_available;
  out.read_only_mode = context.read_only_mode;
  out.trace_tags.push_back("sblr.domain_runtime");
  return out;
}

api::EngineDescriptor DomainDescriptorFromUuid(std::string domain_uuid) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = std::move(domain_uuid);
  descriptor.descriptor_kind = "domain";
  descriptor.canonical_type_name = "domain";
  descriptor.encoded_descriptor = "domain_uuid=" + descriptor.descriptor_uuid.canonical;
  return descriptor;
}

api::EngineDescriptor DescriptorFromSblrValue(const SblrValue& value) {
  api::EngineDescriptor descriptor;
  descriptor.canonical_type_name = value.descriptor_id.empty() ? "character" : value.descriptor_id;
  descriptor.descriptor_kind = LooksLikeSblrDomainDescriptor(value.descriptor_id) ? "domain" : "scalar";
  descriptor.encoded_descriptor = descriptor.descriptor_kind == "domain"
                                      ? "domain_uuid=" + std::string(value.descriptor_id.rfind("domain:", 0) == 0
                                                                        ? value.descriptor_id.substr(7)
                                                                        : value.descriptor_id)
                                      : "canonical=" + descriptor.canonical_type_name;
  if (descriptor.descriptor_kind == "domain") {
    descriptor.descriptor_uuid.canonical = value.descriptor_id.rfind("domain:", 0) == 0
                                               ? value.descriptor_id.substr(7)
                                               : value.descriptor_id;
  }
  return descriptor;
}

api::EngineTypedValue ToEngineValue(const SblrValue& value) {
  api::EngineTypedValue out;
  out.descriptor = DescriptorFromSblrValue(value);
  out.is_null = value.is_null;
  if (value.is_null) {
    out.encoded_value = "<NULL>";
  } else if (!value.encoded_value.empty()) {
    out.encoded_value = value.encoded_value;
  } else if (!value.text_value.empty()) {
    out.encoded_value = value.text_value;
  } else if (value.has_int64_value) {
    out.encoded_value = std::to_string(value.int64_value);
  } else if (value.has_uint64_value) {
    out.encoded_value = std::to_string(value.uint64_value);
  } else if (value.has_real64_value) {
    out.encoded_value = std::to_string(value.real64_value);
  }
  return out;
}

SblrValue FromEngineValue(const api::EngineTypedValue& value) {
  SblrValue out;
  out.descriptor_id = value.descriptor.descriptor_kind == "domain" && !value.descriptor.descriptor_uuid.canonical.empty()
                          ? "domain:" + value.descriptor.descriptor_uuid.canonical
                          : value.descriptor.canonical_type_name;
  out.is_null = value.is_null || value.encoded_value == "<NULL>";
  if (out.is_null) return out;
  out.encoded_value = value.encoded_value;
  out.text_value = value.encoded_value;
  out.payload_kind = SblrValuePayloadKind::text;
  return out;
}

SblrResult SblrDomainFailure(std::string operation_id,
                             const SblrExecutionContext& context,
                             const api::EngineApiDiagnostic& api_diagnostic,
                             std::string domain_uuid) {
  auto diagnostic = MakeSblrRefusalDiagnostic(api_diagnostic.code.empty() ? "SB_DIAG_DOMAIN_RUNTIME_FAILED"
                                                                          : api_diagnostic.code,
                                              context,
                                              api_diagnostic.detail.empty() ? api_diagnostic.message_key
                                                                            : api_diagnostic.detail);
  diagnostic.fields.push_back({"domain_uuid", std::move(domain_uuid)});
  diagnostic.fields.push_back({"api_message_key", api_diagnostic.message_key});
  return MakeSblrFailure(SblrStatusCode::execution_failed, std::move(operation_id), std::move(diagnostic));
}

SblrResult ScalarDomainResult(std::string operation_id, SblrValue value) {
  SblrResult out = MakeSblrSuccess(std::move(operation_id));
  out.scalar_values.push_back(std::move(value));
  return out;
}

std::string ValueText(const SblrValue& value) {
  if (value.is_null) return "<NULL>";
  if (!value.encoded_value.empty()) return value.encoded_value;
  if (!value.text_value.empty()) return value.text_value;
  if (value.has_int64_value) return std::to_string(value.int64_value);
  if (value.has_uint64_value) return std::to_string(value.uint64_value);
  if (value.has_real64_value) return std::to_string(value.real64_value);
  return {};
}

SblrResult ValidateViaEngine(const SblrDomainRequest& request, std::string operation_id) {
  api::EngineValidateDomainValueRequest api_request;
  api_request.context = ToEngineContext(request.context);
  api_request.operation_id = "query.validate_domain_value";
  api_request.domain_descriptor = DomainDescriptorFromUuid(request.domain_uuid);
  api_request.input_value = ToEngineValue(request.value);
  const auto result = api::EngineValidateDomainValue(api_request);
  if (!result.ok) {
    const auto diagnostic = result.diagnostics.empty() ? api::EngineApiDiagnostic{"SB_DIAG_DOMAIN_VALIDATION_FAILED",
                                                                                  "domain.validation.failed",
                                                                                  "domain validation failed",
                                                                                  true}
                                                       : result.diagnostics.front();
    return SblrDomainFailure(std::move(operation_id), request.context, diagnostic, request.domain_uuid);
  }
  return ScalarDomainResult(std::move(operation_id), FromEngineValue(result.value));
}

}  // namespace

bool LooksLikeSblrDomainDescriptor(std::string_view descriptor_id) {
  return descriptor_id.rfind("domain:", 0) == 0 ||
         (descriptor_id.size() >= 32 && descriptor_id.find('-') != std::string_view::npos);
}

SblrResult CastSblrValueToDomain(const SblrDomainRequest& request) {
  return ValidateViaEngine(request, "sblr.domain.cast_in");
}

SblrResult CastSblrDomainValueToBase(const SblrDomainRequest& request) {
  auto validated = ValidateViaEngine(request, "sblr.domain.cast_out");
  if (!validated.ok() || validated.scalar_values.empty()) return validated;
  SblrValue out = validated.scalar_values.front();
  if (out.descriptor_id.rfind("domain:", 0) == 0) {
    out.descriptor_id = request.value.descriptor_id.empty() ? "character" : request.value.descriptor_id;
  }
  return ScalarDomainResult("sblr.domain.cast_out", std::move(out));
}

SblrResult ValidateSblrDomainValue(const SblrDomainRequest& request) {
  return ValidateViaEngine(request, "sblr.domain.validate");
}

SblrResult ApplySblrDomainReadPolicy(const SblrDomainRequest& request) {
  const auto engine_context = ToEngineContext(request.context);
  const std::vector<std::pair<std::string, std::string>> columns = {{"value", "domain:" + request.domain_uuid}};
  const std::vector<std::pair<std::string, std::string>> values = {{"value", ValueText(request.value)}};
  const auto result = api::ApplyDomainReadPoliciesToCrudValues(engine_context,
                                                               columns,
                                                               values,
                                                               engine_context.local_transaction_id);
  if (!result.ok) {
    return SblrDomainFailure("sblr.domain.read_policy", request.context, result.diagnostic, request.domain_uuid);
  }
  SblrValue out = request.value;
  for (const auto& [key, value] : result.values) {
    if (key == "value") {
      out.is_null = value == "<NULL>";
      out.encoded_value = out.is_null ? "" : value;
      out.text_value = out.encoded_value;
      out.descriptor_id = "domain:" + request.domain_uuid;
      out.payload_kind = out.is_null ? SblrValuePayloadKind::none : SblrValuePayloadKind::text;
      break;
    }
  }
  return ScalarDomainResult("sblr.domain.read_policy", std::move(out));
}

SblrResult InvokeSblrDomainMethod(const SblrDomainRequest& request) {
  api::EngineInvokeDomainMethodRequest api_request;
  api_request.context = ToEngineContext(request.context);
  api_request.operation_id = "query.invoke_domain_method";
  api_request.domain_descriptor = DomainDescriptorFromUuid(request.domain_uuid);
  api_request.input_value = ToEngineValue(request.value);
  api_request.method_name = request.method_name;
  const auto result = api::EngineInvokeDomainMethod(api_request);
  if (!result.ok) {
    const auto diagnostic = result.diagnostics.empty() ? api::EngineApiDiagnostic{"SB_DIAG_DOMAIN_METHOD_FAILED",
                                                                                  "domain.method.failed",
                                                                                  "domain method failed",
                                                                                  true}
                                                       : result.diagnostics.front();
    return SblrDomainFailure("sblr.domain.method", request.context, diagnostic, request.domain_uuid);
  }
  return ScalarDomainResult("sblr.domain.method", FromEngineValue(result.value));
}

SblrAssignmentDomainValidator MakeSblrDomainAssignmentValidator() {
  return [](const SblrAssignmentSlot& slot, const SblrValue& value, const SblrExecutionContext& context) {
    SblrDomainRequest request;
    request.context = context;
    request.domain_uuid = slot.domain_descriptor_id.rfind("domain:", 0) == 0
                              ? slot.domain_descriptor_id.substr(7)
                              : slot.domain_descriptor_id;
    request.value = value;
    const auto validation = ValidateSblrDomainValue(request);
    SblrAssignmentValidationResult out;
    out.ok = validation.ok();
    if (!out.ok && !validation.diagnostics.empty()) {
      out.diagnostic_id = validation.diagnostics.front().diagnostic_id;
      out.detail = validation.diagnostics.front().detail;
    }
    return out;
  };
}

SblrDomainOptimizerMetadata DomainOptimizerMetadata() {
  return SblrDomainOptimizerMetadata{};
}

}  // namespace scratchbird::engine::sblr
