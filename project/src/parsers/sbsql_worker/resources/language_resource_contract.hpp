// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::parser::sbsql {

// SEARCH_KEY: SBSQL_LANGUAGE_RESOURCE_OPERATIONAL_EDGE_SAFETY

enum class LanguageResourceChannel {
  kExperimental,
  kPreview,
  kBeta,
  kReleaseSupported,
  kDeprecated,
  kRevoked,
  kRemoved,
};

enum class LanguageResourceSupportState {
  kMachineBootstrap,
  kNativeReviewed,
  kReleaseSupported,
};

enum class ResourceValidationSeverity {
  kInfo,
  kWarning,
  kError,
};

struct ResourceValidationIssue {
  ResourceValidationSeverity severity{ResourceValidationSeverity::kError};
  std::string code;
  std::string detail;
};

struct ResourceValidationResult {
  bool accepted{true};
  std::vector<ResourceValidationIssue> issues;

  bool HasIssue(std::string_view code) const;
  void AddError(std::string code, std::string detail);
  void AddWarning(std::string code, std::string detail);
};

enum class CanonicalElementKind {
  kCommand,
  kClause,
  kIdentifier,
  kLiteral,
  kOperator,
  kPunctuation,
  kDiagnosticMarker,
};

struct CanonicalElementSourceSpan {
  std::size_t offset{0};
  std::size_t length{0};
};

struct CanonicalElement {
  CanonicalElementKind kind{CanonicalElementKind::kIdentifier};
  std::string canonical_id;
  std::string surface_id;
  std::string slot_id;
  std::string alias_id;
  std::string topology_role;
  std::string localized_text_hash;
  CanonicalElementSourceSpan source_span;
};

struct CanonicalElementStream {
  std::string resource_identity;
  std::string language_profile_uuid;
  std::string exact_tag;
  std::string dialect_profile_uuid;
  std::string topology_profile_uuid;
  std::string common_resource_hash;
  std::string source_hash;
  std::string canonical_order_id{"sbsql.canonical_order.v1"};
  bool normalized_before_uuid_resolution{true};
  bool server_revalidation_required{true};
  std::vector<CanonicalElement> elements;
};

enum class ParseProfileStep {
  kExplicitSyntaxProfile,
  kPreferredLanguageAndDialect,
  kCanonicalEnglishFallback,
  kFailClosed,
};

struct ParseProfileDecisionInput {
  bool explicit_syntax_profile_available{false};
  bool preferred_language_parse_succeeded{false};
  bool canonical_english_parse_succeeded{false};
};

enum class ParseProfileDecision {
  kUseExplicitSyntaxProfile,
  kUsePreferredLanguageAndDialect,
  kUseCanonicalEnglishFallback,
  kFailClosed,
};

struct SblrRenderRequest {
  bool sblr_uuid_authority_valid{false};
  bool preferred_renderer_available{false};
  bool canonical_english_renderer_available{false};
  bool resource_revoked{false};
  bool resource_incompatible{false};
  bool source_reconstruction_requested{false};
};

enum class SblrRenderDecision {
  kPreferredLanguage,
  kCanonicalEnglishFallback,
  kRefuseMissingCanonicalAuthority,
  kRefuseRevokedResource,
  kRefuseIncompatibleResource,
  kRefuseSourceReconstruction,
  kRefuseRendererUnavailable,
};

struct LanguageResourceLimits {
  std::uint64_t max_predictive_table_bytes{1024 * 1024};
  std::uint64_t max_transition_fanout{128};
  std::uint64_t max_completion_results{256};
  std::uint64_t max_generation_millis{100};
  std::uint64_t max_nested_expansion_depth{16};
};

struct LocaleLiteralPolicy {
  bool admits_decimal_comma{false};
  bool admits_localized_digits{false};
  bool admits_localized_month_names{false};
  bool admits_non_gregorian_calendar{false};
  bool admits_rtl_date_layout{false};
  bool admits_mirrored_punctuation{false};
};

struct ConfusablePolicy {
  bool allow_mixed_script_identifiers{false};
  bool allow_transliteration_aliases{false};
  bool allow_bidi_controls{false};
  bool allow_mirrored_punctuation{false};
  bool preserve_hidden_missing_equivalence{true};
};

struct LanguageDataProvenance {
  std::string source_name;
  std::string source_version;
  std::string license_id;
  std::string transformation_id;
  std::string sbom_component_id;
  std::string third_party_notice_id;
  bool community_contribution{false};
  bool machine_generated{false};
  bool redistribution_allowed{false};
};

struct LanguageResourceManifest {
  std::string profile_uuid;
  std::string exact_tag;
  std::string common_resource_hash;
  std::string canonical_surface_registry_hash;
  std::string sblr_registry_hash;
  std::string predictive_grammar_hash;
  std::string renderer_registry_hash;
  std::string diagnostic_pack_hash;
  std::string signature_id;
  std::string signing_key_id;
  std::string governance_evidence_id;
  std::string native_review_evidence_id;
  std::string support_owner_id;
  std::string trace_oracle_id;
  std::string fallback_parent_uuid;
  LanguageResourceChannel channel{LanguageResourceChannel::kExperimental};
  LanguageResourceSupportState support_state{LanguageResourceSupportState::kMachineBootstrap};
  LanguageResourceLimits limits;
  LocaleLiteralPolicy literal_policy;
  ConfusablePolicy confusable_policy;
  std::vector<std::string> canonical_ids;
  std::vector<std::string> renderer_edges;
  std::vector<LanguageDataProvenance> provenance;
  bool built_in_recovery_profile{false};
  bool externally_replaceable{true};
  bool expired{false};
  bool revoked{false};
  bool removed{false};
};

enum class LocaleLiteralClassification {
  kCanonical,
  kRequiresExplicitProfile,
  kRefuseAmbiguous,
};

enum class RestoreLanguageResourceState {
  kExactResourceAvailable,
  kCanonicalAuthorityValidRendererFallback,
  kRefuseRevokedResource,
  kRefuseMissingCanonicalAuthority,
  kRefuseIncompatibleResource,
};

struct LanguageResourceRestoreRequest {
  bool sblr_uuid_authority_valid{false};
  bool exact_resource_available{false};
  bool preferred_renderer_available{false};
  bool resource_revoked{false};
  bool resource_incompatible{false};
};

struct EditorToolProtocol {
  std::string protocol_version{"sbsql.editor_tool.v1"};
  std::string resource_identity;
  bool syntax_profile_selection{true};
  bool canonical_element_preview{true};
  bool diagnostic_vector_schema{true};
  bool completion_schema{true};
  bool hidden_object_no_disclosure{true};
  bool renderer_schema{true};
  bool local_draft_sblr_eligibility{true};
  bool cancellation_and_limits{true};
  bool offline_cache_status{true};
  bool support_bundle_redaction_metadata{true};
};

const LanguageResourceManifest& BuiltInCanonicalEnglishRecoveryProfile();
ResourceValidationResult ValidateLanguageResourceManifest(const LanguageResourceManifest& manifest);
ResourceValidationResult ValidateEditorToolProtocol(const EditorToolProtocol& protocol);
ResourceValidationResult ValidateCanonicalElementStream(const CanonicalElementStream& stream);
ResourceValidationResult ValidateParseProfileOrder(const std::vector<ParseProfileStep>& order);
LocaleLiteralClassification ClassifyLocaleLiteral(std::string_view literal,
                                                  const LocaleLiteralPolicy& policy);
bool HasMixedScriptOrConfusableRisk(std::string_view text, const ConfusablePolicy& policy);
RestoreLanguageResourceState ClassifyRestoreLanguageResourceState(
    const LanguageResourceRestoreRequest& request);
ParseProfileDecision SelectParseProfile(const ParseProfileDecisionInput& input);
SblrRenderDecision ClassifySblrRenderRequest(const SblrRenderRequest& request);
std::vector<ParseProfileStep> DefaultParseProfileOrder();

std::string_view LanguageResourceChannelName(LanguageResourceChannel channel);
std::string_view ParseProfileStepName(ParseProfileStep step);
std::string_view ParseProfileDecisionName(ParseProfileDecision decision);
std::string_view SblrRenderDecisionName(SblrRenderDecision decision);
std::string_view RestoreLanguageResourceStateName(RestoreLanguageResourceState state);

} // namespace scratchbird::parser::sbsql
