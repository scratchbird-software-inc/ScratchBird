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
  return result;
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
