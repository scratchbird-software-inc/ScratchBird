// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "resources/language_resource_contract.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <set>
#include <utility>

namespace scratchbird::parser::sbsql {
namespace {

enum class ScriptClass {
  kAscii,
  kLatin,
  kGreek,
  kCyrillic,
  kArabic,
  kHebrew,
  kOther,
};

bool Empty(std::string_view value) {
  return value.empty();
}

bool ContainsEdgeToSelf(std::string_view profile_uuid,
                        const std::vector<std::string>& renderer_edges) {
  const std::string self(profile_uuid);
  return std::any_of(renderer_edges.begin(), renderer_edges.end(),
                     [&](const std::string& edge) { return edge == self; });
}

bool HasDuplicate(const std::vector<std::string>& values) {
  std::set<std::string> seen;
  for (const auto& value : values) {
    if (value.empty()) continue;
    if (!seen.insert(value).second) return true;
  }
  return false;
}

bool DecodeUtf8(std::string_view text, std::vector<std::uint32_t>* codepoints) {
  codepoints->clear();
  for (std::size_t i = 0; i < text.size();) {
    const auto byte = static_cast<unsigned char>(text[i]);
    if (byte < 0x80) {
      codepoints->push_back(byte);
      ++i;
      continue;
    }
    std::uint32_t cp = 0;
    std::size_t needed = 0;
    if ((byte & 0xE0) == 0xC0) {
      cp = byte & 0x1F;
      needed = 1;
    } else if ((byte & 0xF0) == 0xE0) {
      cp = byte & 0x0F;
      needed = 2;
    } else if ((byte & 0xF8) == 0xF0) {
      cp = byte & 0x07;
      needed = 3;
    } else {
      return false;
    }
    if (i + needed >= text.size()) return false;
    for (std::size_t j = 1; j <= needed; ++j) {
      const auto cont = static_cast<unsigned char>(text[i + j]);
      if ((cont & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cont & 0x3F);
    }
    codepoints->push_back(cp);
    i += needed + 1;
  }
  return true;
}

ScriptClass ClassifyScript(std::uint32_t cp) {
  if (cp < 0x80) return ScriptClass::kAscii;
  if ((cp >= 0x00C0 && cp <= 0x024F) || (cp >= 0x1E00 && cp <= 0x1EFF)) {
    return ScriptClass::kLatin;
  }
  if ((cp >= 0x0370 && cp <= 0x03FF) || (cp >= 0x1F00 && cp <= 0x1FFF)) {
    return ScriptClass::kGreek;
  }
  if (cp >= 0x0400 && cp <= 0x052F) return ScriptClass::kCyrillic;
  if (cp >= 0x0590 && cp <= 0x05FF) return ScriptClass::kHebrew;
  if (cp >= 0x0600 && cp <= 0x06FF) return ScriptClass::kArabic;
  return ScriptClass::kOther;
}

bool IsAsciiDigit(std::uint32_t cp) {
  return cp >= '0' && cp <= '9';
}

bool IsLocalizedDigit(std::uint32_t cp) {
  return (cp >= 0x0660 && cp <= 0x0669) || (cp >= 0x06F0 && cp <= 0x06F9) ||
         (cp >= 0x0966 && cp <= 0x096F);
}

bool IsBidiControl(std::uint32_t cp) {
  return (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069);
}

bool IsMirroredPunctuation(std::uint32_t cp) {
  return cp == 0x061B || cp == 0x061F || cp == 0xFD3E || cp == 0xFD3F;
}

bool LooksLikeLocalizedMonth(std::string_view literal) {
  static constexpr std::string_view kMonthTokens[] = {
      "janvier", "fevrier", "février", "mars", "avril", "mai",
      "juin", "juillet", "aout", "août", "septembre", "octobre",
      "novembre", "decembre", "décembre"};
  for (const auto token : kMonthTokens) {
    if (literal.find(token) != std::string_view::npos) return true;
  }
  return false;
}

bool HasDecimalCommaShape(std::string_view literal) {
  for (std::size_t i = 1; i + 1 < literal.size(); ++i) {
    if (literal[i] != ',') continue;
    if (literal[i - 1] >= '0' && literal[i - 1] <= '9' &&
        literal[i + 1] >= '0' && literal[i + 1] <= '9') {
      return true;
    }
  }
  return false;
}

std::vector<std::string> ExpectedParseProfileOrderNames() {
  return {
      "explicit_syntax_profile",
      "preferred_language_and_dialect",
      "canonical_english_fallback_when_preferred_fails",
      "fail_closed",
  };
}

std::vector<std::string> ExpectedRendererLossinessClasses() {
  return {
      "lossless_canonical",
      "canonical_equivalent",
      "preferred_language_partial",
      "canonical_english_fallback",
      "not_renderable",
  };
}

std::vector<std::string> ExpectedFallbackDiagnosticCodes() {
  return {
      "SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH",
      "SBSQL.LANG_RESOURCE.FAIL_CLOSED_ON_PROFILE_MISMATCH",
  };
}

std::vector<std::string> ExpectedRenderingDiagnosticCodes() {
  return {
      "SBSQL.LANG_RESOURCE.RENDERER_LOSSINESS_CLASSIFIED",
      "SBSQL.LANG_RESOURCE.RENDERER_SOURCE_RECONSTRUCTION_FORBIDDEN",
      "SBSQL.LANG_RESOURCE.RENDERER_NOT_RENDERABLE",
  };
}

void SetRenderDiagnostic(SblrRenderSelection* selection,
                         std::string code,
                         std::string message) {
  if (selection == nullptr) return;
  selection->diagnostic_code = std::move(code);
  selection->diagnostic_message = std::move(message);
}

} // namespace

bool ResourceValidationResult::HasIssue(std::string_view code) const {
  return std::any_of(issues.begin(), issues.end(), [&](const ResourceValidationIssue& issue) {
    return issue.code == code;
  });
}

void ResourceValidationResult::AddError(std::string code, std::string detail) {
  accepted = false;
  issues.push_back(ResourceValidationIssue{ResourceValidationSeverity::kError,
                                           std::move(code), std::move(detail)});
}

void ResourceValidationResult::AddWarning(std::string code, std::string detail) {
  issues.push_back(ResourceValidationIssue{ResourceValidationSeverity::kWarning,
                                           std::move(code), std::move(detail)});
}

std::vector<ParseProfileStep> DefaultParseProfileOrder() {
  return {
      ParseProfileStep::kExplicitSyntaxProfile,
      ParseProfileStep::kPreferredLanguageAndDialect,
      ParseProfileStep::kCanonicalEnglishFallback,
      ParseProfileStep::kFailClosed,
  };
}

const LanguageResourceManifest& BuiltInCanonicalEnglishRecoveryProfile() {
  static const LanguageResourceManifest profile = [] {
    LanguageResourceManifest manifest;
    manifest.profile_uuid = "sbsql.builtin.recovery.en";
    manifest.exact_tag = "en";
    manifest.common_resource_hash = "builtin.common.sbsql.v1";
    manifest.canonical_surface_registry_hash = "builtin.surface.sbsql.v1";
    manifest.sblr_registry_hash = "builtin.sblr.registry.v1";
    manifest.predictive_grammar_hash = "builtin.predictive.disabled";
    manifest.renderer_registry_hash = "builtin.renderer.canonical_en.v1";
    manifest.diagnostic_pack_hash = "builtin.diagnostics.canonical_en.v1";
    manifest.signature_id = "builtin";
    manifest.signing_key_id = "binary";
    manifest.governance_evidence_id = "builtin.recovery.profile";
    manifest.native_review_evidence_id = "builtin.recovery.native-reviewed";
    manifest.support_owner_id = "scratchbird.release";
    manifest.trace_oracle_id = "builtin.recovery.trace";
    manifest.channel = LanguageResourceChannel::kReleaseSupported;
    manifest.support_state = LanguageResourceSupportState::kReleaseSupported;
    manifest.canonical_ids = {"SBSQL.CANONICAL.EN", "SBLR.REGISTRY.V1"};
    manifest.provenance.push_back(LanguageDataProvenance{
        "ScratchBird built-in canonical English SBsql", "1",
        "MPL-2.0", "binary-built-in", "sbom.sbsql.builtin.recovery.en",
        "notice.scratchbird.builtin", false, false, true});
    manifest.built_in_recovery_profile = true;
    manifest.externally_replaceable = false;
    return manifest;
  }();
  return profile;
}

ResourceValidationResult ValidateLanguageResourceManifest(const LanguageResourceManifest& manifest) {
  ResourceValidationResult result;
  if (Empty(manifest.profile_uuid)) result.AddError("SBSQL.LANG_RESOURCE.PROFILE_UUID_MISSING", "profile uuid is required");
  if (Empty(manifest.exact_tag)) result.AddError("SBSQL.LANG_RESOURCE.EXACT_TAG_MISSING", "exact language tag is required");
  if (Empty(manifest.common_resource_hash)) result.AddError("SBSQL.LANG_RESOURCE.COMMON_HASH_MISSING", "common resource hash is required");
  if (Empty(manifest.canonical_surface_registry_hash)) {
    result.AddError("SBSQL.LANG_RESOURCE.SURFACE_REGISTRY_HASH_MISSING", "canonical surface registry hash is required");
  }
  if (Empty(manifest.sblr_registry_hash)) result.AddError("SBSQL.LANG_RESOURCE.SBLR_REGISTRY_HASH_MISSING", "SBLR registry hash is required");
  if (Empty(manifest.signature_id)) result.AddError("SBSQL.LANG_RESOURCE.UNSIGNED", "signature identity is required");
  if (Empty(manifest.signing_key_id)) result.AddError("SBSQL.LANG_RESOURCE.SIGNING_KEY_MISSING", "signing key identity is required");
  if (manifest.revoked || manifest.channel == LanguageResourceChannel::kRevoked) {
    result.AddError("SBSQL.LANG_RESOURCE.REVOKED", "revoked language resources fail closed");
  }
  if (manifest.removed || manifest.channel == LanguageResourceChannel::kRemoved) {
    result.AddError("SBSQL.LANG_RESOURCE.REMOVED", "removed language resources fail closed");
  }
  if (manifest.expired) result.AddError("SBSQL.LANG_RESOURCE.EXPIRED", "expired language resources fail closed");
  if (manifest.fallback_parent_uuid == manifest.profile_uuid && !manifest.profile_uuid.empty()) {
    result.AddError("SBSQL.LANG_RESOURCE.CYCLIC_FALLBACK_PARENT", "fallback parent points to the same profile");
  }
  if (ContainsEdgeToSelf(manifest.profile_uuid, manifest.renderer_edges)) {
    result.AddError("SBSQL.LANG_RESOURCE.RENDERER_RECURSION", "renderer edge points to the same profile");
  }
  if (HasDuplicate(manifest.canonical_ids)) {
    result.AddError("SBSQL.LANG_RESOURCE.DUPLICATE_CANONICAL_ID", "canonical IDs must be unique");
  }
  if (manifest.limits.max_predictive_table_bytes > 8ull * 1024ull * 1024ull ||
      manifest.limits.max_transition_fanout > 1024 ||
      manifest.limits.max_completion_results > 4096 ||
      manifest.limits.max_nested_expansion_depth > 64) {
    result.AddError("SBSQL.LANG_RESOURCE.PREDICTIVE_LIMIT_EXCEEDED",
                    "predictive resources exceed release safety limits");
  }
  if (manifest.channel == LanguageResourceChannel::kReleaseSupported) {
    if (manifest.support_state != LanguageResourceSupportState::kReleaseSupported) {
      result.AddError("SBSQL.LANG_RESOURCE.SUPPORT_STATE_MISMATCH",
                      "release-supported resources require release-supported support state");
    }
    if (Empty(manifest.governance_evidence_id) || Empty(manifest.native_review_evidence_id) ||
        Empty(manifest.support_owner_id) || Empty(manifest.trace_oracle_id)) {
      result.AddError("SBSQL.LANG_RESOURCE.GOVERNANCE_EVIDENCE_MISSING",
                      "release-supported resources require governance native review support owner and trace evidence");
    }
  }
  if (manifest.built_in_recovery_profile && manifest.externally_replaceable) {
    result.AddError("SBSQL.LANG_RESOURCE.RECOVERY_PROFILE_REPLACEABLE",
                    "built-in recovery profile cannot be externally replaceable");
  }
  for (const auto& provenance : manifest.provenance) {
    if (Empty(provenance.source_name) || Empty(provenance.source_version) ||
        Empty(provenance.license_id) || Empty(provenance.transformation_id) ||
        Empty(provenance.sbom_component_id) || Empty(provenance.third_party_notice_id)) {
      result.AddError("SBSQL.LANG_RESOURCE.PROVENANCE_INCOMPLETE",
                      "language data provenance requires source license transform SBOM and notice IDs");
    }
    if (!provenance.redistribution_allowed) {
      result.AddError("SBSQL.LANG_RESOURCE.REDISTRIBUTION_NOT_ALLOWED",
                      "release resource data must be redistributable");
    }
  }
  if (manifest.channel == LanguageResourceChannel::kReleaseSupported &&
      manifest.provenance.empty()) {
    result.AddError("SBSQL.LANG_RESOURCE.PROVENANCE_MISSING",
                    "release-supported resources require provenance rows");
  }
  return result;
}

ResourceValidationResult ValidateLanguageResourceBundleManifest(
    const LanguageResourceBundleManifest& bundle) {
  ResourceValidationResult result;
  if (bundle.bundle_schema_version != "sbsql.language_resource_bundle.v1") {
    result.AddError("SBSQL.LANG_BUNDLE.SCHEMA_VERSION_UNSUPPORTED",
                    "language resource bundle schema version is unsupported");
  }
  if (Empty(bundle.bundle_uuid)) {
    result.AddError("SBSQL.LANG_BUNDLE.BUNDLE_UUID_MISSING",
                    "language resource bundle UUID is required");
  }
  if (Empty(bundle.bundle_contract_id)) {
    result.AddError("SBSQL.LANG_BUNDLE.CONTRACT_ID_MISSING",
                    "language resource bundle contract ID is required");
  }
  if (Empty(bundle.exact_tag)) {
    result.AddError("SBSQL.LANG_BUNDLE.EXACT_TAG_MISSING",
                    "language resource bundle exact language tag is required");
  }
  if (Empty(bundle.dialect_profile_uuid)) {
    result.AddError("SBSQL.LANG_BUNDLE.DIALECT_PROFILE_MISSING",
                    "language resource bundle dialect profile UUID is required");
  }
  if (Empty(bundle.topology_profile_uuid)) {
    result.AddError("SBSQL.LANG_BUNDLE.TOPOLOGY_PROFILE_MISSING",
                    "language resource bundle topology profile UUID is required");
  }
  if (Empty(bundle.common_resource_hash)) {
    result.AddError("SBSQL.LANG_BUNDLE.COMMON_HASH_MISSING",
                    "language resource bundle common resource hash is required");
  }
  if (Empty(bundle.canonical_element_stream_schema_hash)) {
    result.AddError("SBSQL.LANG_BUNDLE.CANONICAL_STREAM_SCHEMA_MISSING",
                    "language resource bundle canonical element stream schema hash is required");
  }
  if (Empty(bundle.predictive_resource_hash)) {
    result.AddError("SBSQL.LANG_BUNDLE.PREDICTIVE_RESOURCE_MISSING",
                    "language resource bundle predictive resource hash is required");
  }
  if (Empty(bundle.renderer_resource_hash)) {
    result.AddError("SBSQL.LANG_BUNDLE.RENDERER_RESOURCE_MISSING",
                    "language resource bundle renderer resource hash is required");
  }
  if (Empty(bundle.diagnostic_resource_hash)) {
    result.AddError("SBSQL.LANG_BUNDLE.DIAGNOSTIC_RESOURCE_MISSING",
                    "language resource bundle diagnostic resource hash is required");
  }
  if (Empty(bundle.compatibility_identity)) {
    result.AddError("SBSQL.LANG_BUNDLE.COMPATIBILITY_IDENTITY_MISSING",
                    "language resource bundle compatibility identity is required");
  }
  if (Empty(bundle.lifecycle_state)) {
    result.AddError("SBSQL.LANG_BUNDLE.LIFECYCLE_STATE_MISSING",
                    "language resource bundle lifecycle state is required");
  }
  if (!bundle.signed_bundle && !bundle.parser_language_library) {
    result.AddError("SBSQL.LANG_BUNDLE.UNSIGNED",
                    "language resource bundle must be signed unless it is a parser language library");
  }

  const auto profile_result = ValidateLanguageResourceManifest(bundle.language_profile);
  for (const auto& issue : profile_result.issues) {
    if (issue.severity == ResourceValidationSeverity::kError) {
      result.AddError(issue.code, issue.detail);
    } else {
      result.AddWarning(issue.code, issue.detail);
    }
  }

  if (!Empty(bundle.language_profile.exact_tag) &&
      bundle.exact_tag != bundle.language_profile.exact_tag) {
    result.AddError("SBSQL.LANG_BUNDLE.EXACT_TAG_PROFILE_MISMATCH",
                    "bundle exact language tag must match the language profile");
  }
  if (!Empty(bundle.language_profile.common_resource_hash) &&
      bundle.common_resource_hash != bundle.language_profile.common_resource_hash) {
    result.AddError("SBSQL.LANG_BUNDLE.COMMON_HASH_PROFILE_MISMATCH",
                    "bundle common resource hash must match the language profile");
  }
  if (!Empty(bundle.language_profile.predictive_grammar_hash) &&
      bundle.predictive_resource_hash != bundle.language_profile.predictive_grammar_hash) {
    result.AddError("SBSQL.LANG_BUNDLE.PREDICTIVE_HASH_PROFILE_MISMATCH",
                    "bundle predictive resource hash must match the language profile");
  }
  if (!Empty(bundle.language_profile.renderer_registry_hash) &&
      bundle.renderer_resource_hash != bundle.language_profile.renderer_registry_hash) {
    result.AddError("SBSQL.LANG_BUNDLE.RENDERER_HASH_PROFILE_MISMATCH",
                    "bundle renderer resource hash must match the language profile");
  }
  if (!Empty(bundle.language_profile.diagnostic_pack_hash) &&
      bundle.diagnostic_resource_hash != bundle.language_profile.diagnostic_pack_hash) {
    result.AddError("SBSQL.LANG_BUNDLE.DIAGNOSTIC_HASH_PROFILE_MISMATCH",
                    "bundle diagnostic resource hash must match the language profile");
  }
  if (bundle.provenance.empty() && bundle.language_profile.provenance.empty()) {
    result.AddError("SBSQL.LANG_BUNDLE.PROVENANCE_MISSING",
                    "language resource bundle requires provenance in the bundle or profile");
  }
  for (const auto& provenance : bundle.provenance) {
    if (Empty(provenance.source_name) || Empty(provenance.source_version) ||
        Empty(provenance.license_id) || Empty(provenance.transformation_id) ||
        Empty(provenance.sbom_component_id) || Empty(provenance.third_party_notice_id)) {
      result.AddError("SBSQL.LANG_BUNDLE.PROVENANCE_INCOMPLETE",
                      "bundle provenance requires source license transform SBOM and notice IDs");
    }
    if (!provenance.redistribution_allowed) {
      result.AddError("SBSQL.LANG_BUNDLE.REDISTRIBUTION_NOT_ALLOWED",
                      "bundle provenance must be redistributable");
    }
  }
  return result;
}

ResourceValidationResult AdmitLanguageResourceBundleOperation(
    const LanguageBundleAdmissionRequest& request) {
  ResourceValidationResult result =
      ValidateLanguageResourceBundleManifest(request.bundle);
  if (request.operation == LanguageBundleOperation::kLoad ||
      request.operation == LanguageBundleOperation::kValidate) {
    if (!request.bundle.compatible_with_parser) {
      result.AddError("SBSQL.LANG_BUNDLE.INCOMPATIBLE",
                      "language resource bundle is not compatible with this parser");
    }
    if (!request.bundle.admitted_by_security_policy) {
      result.AddError("SBSQL.LANG_BUNDLE.SECURITY_ADMISSION_REQUIRED",
                      "language resource bundle requires security admission before use");
    }
  }
  if (request.operation == LanguageBundleOperation::kUnload) {
    if (request.bundle.active_profile) {
      result.AddError("SBSQL.LANG_BUNDLE.ACTIVE_PROFILE_IN_USE",
                      "active language profiles cannot be unloaded");
    }
    if (request.bundle.required_profile) {
      result.AddError("SBSQL.LANG_BUNDLE.REQUIRED_PROFILE",
                      "required language profiles cannot be unloaded");
    }
  }
  return result;
}

ResourceValidationResult ValidateCanonicalElementStream(const CanonicalElementStream& stream) {
  ResourceValidationResult result;
  if (Empty(stream.resource_identity)) {
    result.AddError("SBSQL.CANONICAL_STREAM.RESOURCE_IDENTITY_MISSING",
                    "canonical element stream requires a common resource identity");
  }
  if (Empty(stream.language_profile_uuid)) {
    result.AddError("SBSQL.CANONICAL_STREAM.LANGUAGE_PROFILE_MISSING",
                    "canonical element stream requires a language profile UUID");
  }
  if (Empty(stream.exact_tag)) {
    result.AddError("SBSQL.CANONICAL_STREAM.EXACT_TAG_MISSING",
                    "canonical element stream requires an exact language tag");
  }
  if (Empty(stream.dialect_profile_uuid)) {
    result.AddError("SBSQL.CANONICAL_STREAM.DIALECT_PROFILE_MISSING",
                    "canonical element stream requires a dialect profile UUID");
  }
  if (Empty(stream.topology_profile_uuid)) {
    result.AddError("SBSQL.CANONICAL_STREAM.TOPOLOGY_PROFILE_MISSING",
                    "canonical element stream requires a topology profile UUID");
  }
  if (Empty(stream.common_resource_hash)) {
    result.AddError("SBSQL.CANONICAL_STREAM.COMMON_HASH_MISSING",
                    "canonical element stream requires the common resource pack hash");
  }
  if (Empty(stream.source_hash)) {
    result.AddError("SBSQL.CANONICAL_STREAM.SOURCE_HASH_MISSING",
                    "canonical element stream requires the localized source hash");
  }
  if (Empty(stream.canonical_order_id)) {
    result.AddError("SBSQL.CANONICAL_STREAM.CANONICAL_ORDER_MISSING",
                    "canonical element stream requires a canonical order identifier");
  }
  if (!stream.normalized_before_uuid_resolution) {
    result.AddError("SBSQL.CANONICAL_STREAM.POST_UUID_NORMALIZATION",
                    "language topology normalization must happen before UUID resolution");
  }
  if (!stream.server_revalidation_required) {
    result.AddError("SBSQL.CANONICAL_STREAM.SERVER_REVALIDATION_REQUIRED",
                    "client or parser canonical streams remain untrusted until server revalidation");
  }
  if (stream.elements.empty()) {
    result.AddError("SBSQL.CANONICAL_STREAM.EMPTY",
                    "canonical element stream must contain at least one normalized element");
  }

  for (const auto& element : stream.elements) {
    if (Empty(element.canonical_text)) {
      result.AddError("SBSQL.CANONICAL_STREAM.ELEMENT_CANONICAL_TEXT_MISSING",
                      "each normalized element requires canonical parser text");
    }
    if (Empty(element.canonical_id)) {
      result.AddError("SBSQL.CANONICAL_STREAM.ELEMENT_CANONICAL_ID_MISSING",
                      "each normalized element requires a canonical token or surface ID");
    }
    if (Empty(element.localized_text_hash)) {
      result.AddError("SBSQL.CANONICAL_STREAM.ELEMENT_SOURCE_HASH_MISSING",
                      "each normalized element must retain a localized source text hash");
    }
    if (element.source_span.length == 0) {
      result.AddError("SBSQL.CANONICAL_STREAM.ELEMENT_SOURCE_SPAN_MISSING",
                      "each normalized element must retain its localized source span");
    }
  }
  return result;
}

ResourceValidationResult ValidateParseProfileOrder(const std::vector<ParseProfileStep>& order) {
  ResourceValidationResult result;
  const auto expected = DefaultParseProfileOrder();
  if (order != expected) {
    result.AddError("SBSQL.PARSE_PROFILE.ORDER_UNSUPPORTED",
                    "parse profile order must be explicit profile, preferred language, canonical English fallback, fail closed");
  }
  return result;
}

ResourceValidationResult ValidateEditorToolProtocol(const EditorToolProtocol& protocol) {
  ResourceValidationResult result;
  if (protocol.protocol_version != "sbsql.editor_tool.v1") {
    result.AddError("SBSQL.EDITOR_PROTOCOL.VERSION_UNSUPPORTED", "unsupported editor protocol version");
  }
  if (protocol.resource_identity.empty()) {
    result.AddError("SBSQL.EDITOR_PROTOCOL.RESOURCE_IDENTITY_MISSING", "resource identity is required");
  }
  if (!protocol.syntax_profile_selection ||
      !protocol.canonical_element_preview ||
      !protocol.diagnostic_vector_schema ||
      !protocol.completion_schema ||
      !protocol.hidden_object_no_disclosure ||
      !protocol.renderer_schema ||
      !protocol.local_draft_sblr_eligibility ||
      !protocol.cancellation_and_limits ||
      !protocol.offline_cache_status ||
      !protocol.support_bundle_redaction_metadata) {
    result.AddError("SBSQL.EDITOR_PROTOCOL.REQUIRED_FEATURE_MISSING",
                    "common editor tool protocol must expose all required surfaces");
  }
  if (protocol.syntax_profile_order != ExpectedParseProfileOrderNames()) {
    result.AddError("SBSQL.EDITOR_PROTOCOL.PARSE_PROFILE_ORDER_UNSUPPORTED",
                    "common editor tool protocol must declare deterministic parse profile order");
  }
  if (protocol.renderer_lossiness_classes != ExpectedRendererLossinessClasses()) {
    result.AddError("SBSQL.EDITOR_PROTOCOL.RENDERER_LOSSINESS_CLASSES_MISSING",
                    "common editor tool protocol must declare every renderer lossiness class");
  }
  if (protocol.fallback_diagnostic_codes != ExpectedFallbackDiagnosticCodes()) {
    result.AddError("SBSQL.EDITOR_PROTOCOL.FALLBACK_DIAGNOSTICS_MISSING",
                    "common editor tool protocol must declare canonical English fallback diagnostics");
  }
  if (protocol.rendering_diagnostic_codes != ExpectedRenderingDiagnosticCodes()) {
    result.AddError("SBSQL.EDITOR_PROTOCOL.RENDERING_DIAGNOSTICS_MISSING",
                    "common editor tool protocol must declare renderer classification diagnostics");
  }
  if (!protocol.fail_closed_on_mismatch || !protocol.server_revalidation_authority) {
    result.AddError("SBSQL.EDITOR_PROTOCOL.AUTHORITY_METADATA_MISSING",
                    "common editor tool protocol must fail closed and keep server revalidation authority");
  }
  if (protocol.authority_boundary !=
      "client_resources_are_untrusted_until_server_revalidation") {
    result.AddError("SBSQL.EDITOR_PROTOCOL.AUTHORITY_BOUNDARY_UNSUPPORTED",
                    "common editor tool protocol must preserve client-resource authority boundary");
  }
  return result;
}

ParseProfileDecision SelectParseProfile(const ParseProfileDecisionInput& input) {
  if (input.explicit_syntax_profile_available) {
    return ParseProfileDecision::kUseExplicitSyntaxProfile;
  }
  if (input.preferred_language_parse_succeeded) {
    return ParseProfileDecision::kUsePreferredLanguageAndDialect;
  }
  if (input.canonical_english_parse_succeeded) {
    return ParseProfileDecision::kUseCanonicalEnglishFallback;
  }
  return ParseProfileDecision::kFailClosed;
}

SblrRenderDecision ClassifySblrRenderRequest(const SblrRenderRequest& request) {
  if (!request.sblr_uuid_authority_valid) {
    return SblrRenderDecision::kRefuseMissingCanonicalAuthority;
  }
  if (request.source_reconstruction_requested) {
    return SblrRenderDecision::kRefuseSourceReconstruction;
  }
  if (request.resource_revoked) return SblrRenderDecision::kRefuseRevokedResource;
  if (request.resource_incompatible) return SblrRenderDecision::kRefuseIncompatibleResource;
  if (request.preferred_renderer_available) return SblrRenderDecision::kPreferredLanguage;
  if (request.canonical_english_renderer_available) {
    return SblrRenderDecision::kCanonicalEnglishFallback;
  }
  return SblrRenderDecision::kRefuseRendererUnavailable;
}

SblrRenderSelection ClassifySblrRenderSelection(const SblrRenderRequest& request) {
  SblrRenderSelection selection;
  selection.decision = ClassifySblrRenderRequest(request);
  selection.fallback_language_profile = request.canonical_english_profile;
  selection.server_revalidation_required = true;

  switch (selection.decision) {
    case SblrRenderDecision::kPreferredLanguage:
      selection.selected_language_profile = request.preferred_language_profile;
      if (request.preferred_language_is_canonical_english) {
        selection.lossiness = SblrRenderLossiness::kLosslessCanonical;
      } else if (request.preferred_renderer_partial) {
        selection.lossiness = SblrRenderLossiness::kPreferredLanguagePartial;
      } else {
        selection.lossiness = SblrRenderLossiness::kCanonicalEquivalent;
      }
      SetRenderDiagnostic(
          &selection,
          "SBSQL.LANG_RESOURCE.RENDERER_LOSSINESS_CLASSIFIED",
          "SBLR-to-SBsql output renderer lossiness was classified");
      break;
    case SblrRenderDecision::kCanonicalEnglishFallback:
      selection.selected_language_profile = request.canonical_english_profile;
      selection.used_canonical_english_fallback = true;
      selection.lossiness = SblrRenderLossiness::kCanonicalEnglishFallback;
      SetRenderDiagnostic(
          &selection,
          "SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH",
          "Preferred-language renderer unavailable; canonical English renderer selected");
      break;
    case SblrRenderDecision::kRefuseMissingCanonicalAuthority:
      selection.lossiness = SblrRenderLossiness::kNotRenderable;
      SetRenderDiagnostic(
          &selection,
          "SBSQL.LANG_RESOURCE.MISSING_CANONICAL_AUTHORITY",
          "SBLR-to-SBsql rendering requires valid SBLR UUID authority");
      break;
    case SblrRenderDecision::kRefuseRevokedResource:
      selection.lossiness = SblrRenderLossiness::kNotRenderable;
      SetRenderDiagnostic(
          &selection,
          "SBSQL.LANG_RESOURCE.REVOKED",
          "Revoked language renderer resource cannot render SBLR");
      break;
    case SblrRenderDecision::kRefuseIncompatibleResource:
      selection.lossiness = SblrRenderLossiness::kNotRenderable;
      SetRenderDiagnostic(
          &selection,
          "SBSQL.LANG_RESOURCE.INCOMPATIBLE",
          "Incompatible language renderer resource cannot render SBLR");
      break;
    case SblrRenderDecision::kRefuseSourceReconstruction:
      selection.lossiness = SblrRenderLossiness::kNotRenderable;
      SetRenderDiagnostic(
          &selection,
          "SBSQL.LANG_RESOURCE.RENDERER_SOURCE_RECONSTRUCTION_FORBIDDEN",
          "SBLR-to-SBsql rendering cannot reconstruct source text as authority");
      break;
    case SblrRenderDecision::kRefuseRendererUnavailable:
      selection.lossiness = SblrRenderLossiness::kNotRenderable;
      SetRenderDiagnostic(
          &selection,
          "SBSQL.LANG_RESOURCE.RENDERER_NOT_RENDERABLE",
          "No admitted preferred-language or canonical English renderer is available");
      break;
  }

  return selection;
}

LocaleLiteralClassification ClassifyLocaleLiteral(std::string_view literal,
                                                  const LocaleLiteralPolicy& policy) {
  std::vector<std::uint32_t> cps;
  if (!DecodeUtf8(literal, &cps)) return LocaleLiteralClassification::kRefuseAmbiguous;
  bool localized_digit = false;
  bool bidi = false;
  bool mirrored = false;
  for (const auto cp : cps) {
    localized_digit = localized_digit || IsLocalizedDigit(cp);
    bidi = bidi || IsBidiControl(cp);
    mirrored = mirrored || IsMirroredPunctuation(cp);
  }
  if (localized_digit && !policy.admits_localized_digits) return LocaleLiteralClassification::kRequiresExplicitProfile;
  if (HasDecimalCommaShape(literal) && !policy.admits_decimal_comma) return LocaleLiteralClassification::kRequiresExplicitProfile;
  if (LooksLikeLocalizedMonth(literal) && !policy.admits_localized_month_names) {
    return LocaleLiteralClassification::kRequiresExplicitProfile;
  }
  if (bidi && !policy.admits_rtl_date_layout) return LocaleLiteralClassification::kRefuseAmbiguous;
  if (mirrored && !policy.admits_mirrored_punctuation) return LocaleLiteralClassification::kRefuseAmbiguous;
  return LocaleLiteralClassification::kCanonical;
}

bool HasMixedScriptOrConfusableRisk(std::string_view text, const ConfusablePolicy& policy) {
  std::vector<std::uint32_t> cps;
  if (!DecodeUtf8(text, &cps)) return true;
  std::set<ScriptClass> scripts;
  for (const auto cp : cps) {
    if (IsAsciiDigit(cp) || cp == '_' || cp == '$' || cp == '.') continue;
    if (IsBidiControl(cp) && !policy.allow_bidi_controls) return true;
    if (IsMirroredPunctuation(cp) && !policy.allow_mirrored_punctuation) return true;
    auto script = ClassifyScript(cp);
    if (script == ScriptClass::kAscii) continue;
    scripts.insert(script);
  }
  if (scripts.size() > 1 && !policy.allow_mixed_script_identifiers) return true;
  if (!scripts.empty() && !policy.allow_transliteration_aliases) {
    if (scripts.count(ScriptClass::kGreek) || scripts.count(ScriptClass::kCyrillic)) {
      return true;
    }
  }
  return false;
}

RestoreLanguageResourceState ClassifyRestoreLanguageResourceState(
    const LanguageResourceRestoreRequest& request) {
  if (request.resource_revoked) return RestoreLanguageResourceState::kRefuseRevokedResource;
  if (!request.sblr_uuid_authority_valid) {
    return RestoreLanguageResourceState::kRefuseMissingCanonicalAuthority;
  }
  if (request.resource_incompatible) return RestoreLanguageResourceState::kRefuseIncompatibleResource;
  if (request.exact_resource_available && request.preferred_renderer_available) {
    return RestoreLanguageResourceState::kExactResourceAvailable;
  }
  return RestoreLanguageResourceState::kCanonicalAuthorityValidRendererFallback;
}

std::string_view LanguageResourceChannelName(LanguageResourceChannel channel) {
  switch (channel) {
    case LanguageResourceChannel::kExperimental: return "experimental";
    case LanguageResourceChannel::kPreview: return "preview";
    case LanguageResourceChannel::kBeta: return "beta";
    case LanguageResourceChannel::kReleaseSupported: return "release_supported";
    case LanguageResourceChannel::kDeprecated: return "deprecated";
    case LanguageResourceChannel::kRevoked: return "revoked";
    case LanguageResourceChannel::kRemoved: return "removed";
  }
  return "unknown";
}

std::string_view ParseProfileStepName(ParseProfileStep step) {
  switch (step) {
    case ParseProfileStep::kExplicitSyntaxProfile:
      return "explicit_syntax_profile";
    case ParseProfileStep::kPreferredLanguageAndDialect:
      return "preferred_language_and_dialect";
    case ParseProfileStep::kCanonicalEnglishFallback:
      return "canonical_english_fallback_when_preferred_fails";
    case ParseProfileStep::kFailClosed:
      return "fail_closed";
  }
  return "unknown";
}

std::string_view ParseProfileDecisionName(ParseProfileDecision decision) {
  switch (decision) {
    case ParseProfileDecision::kUseExplicitSyntaxProfile:
      return "use_explicit_syntax_profile";
    case ParseProfileDecision::kUsePreferredLanguageAndDialect:
      return "use_preferred_language_and_dialect";
    case ParseProfileDecision::kUseCanonicalEnglishFallback:
      return "use_canonical_english_fallback";
    case ParseProfileDecision::kFailClosed:
      return "fail_closed";
  }
  return "unknown";
}

std::string_view SblrRenderDecisionName(SblrRenderDecision decision) {
  switch (decision) {
    case SblrRenderDecision::kPreferredLanguage:
      return "preferred_language";
    case SblrRenderDecision::kCanonicalEnglishFallback:
      return "canonical_english_fallback";
    case SblrRenderDecision::kRefuseMissingCanonicalAuthority:
      return "refuse_missing_canonical_authority";
    case SblrRenderDecision::kRefuseRevokedResource:
      return "refuse_revoked_resource";
    case SblrRenderDecision::kRefuseIncompatibleResource:
      return "refuse_incompatible_resource";
    case SblrRenderDecision::kRefuseSourceReconstruction:
      return "refuse_source_reconstruction";
    case SblrRenderDecision::kRefuseRendererUnavailable:
      return "refuse_renderer_unavailable";
  }
  return "unknown";
}

std::string_view SblrRenderLossinessName(SblrRenderLossiness lossiness) {
  switch (lossiness) {
    case SblrRenderLossiness::kLosslessCanonical:
      return "lossless_canonical";
    case SblrRenderLossiness::kCanonicalEquivalent:
      return "canonical_equivalent";
    case SblrRenderLossiness::kPreferredLanguagePartial:
      return "preferred_language_partial";
    case SblrRenderLossiness::kCanonicalEnglishFallback:
      return "canonical_english_fallback";
    case SblrRenderLossiness::kNotRenderable:
      return "not_renderable";
  }
  return "unknown";
}

std::string_view LanguageBundleOperationName(LanguageBundleOperation operation) {
  switch (operation) {
    case LanguageBundleOperation::kValidate:
      return "validate";
    case LanguageBundleOperation::kLoad:
      return "load";
    case LanguageBundleOperation::kUnload:
      return "unload";
  }
  return "unknown";
}

std::string_view RestoreLanguageResourceStateName(RestoreLanguageResourceState state) {
  switch (state) {
    case RestoreLanguageResourceState::kExactResourceAvailable:
      return "exact_resource_available";
    case RestoreLanguageResourceState::kCanonicalAuthorityValidRendererFallback:
      return "canonical_authority_valid_renderer_fallback";
    case RestoreLanguageResourceState::kRefuseRevokedResource:
      return "refuse_revoked_resource";
    case RestoreLanguageResourceState::kRefuseMissingCanonicalAuthority:
      return "refuse_missing_canonical_authority";
    case RestoreLanguageResourceState::kRefuseIncompatibleResource:
      return "refuse_incompatible_resource";
  }
  return "unknown";
}

} // namespace scratchbird::parser::sbsql
