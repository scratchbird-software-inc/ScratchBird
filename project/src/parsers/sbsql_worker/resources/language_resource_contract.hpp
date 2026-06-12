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

struct Diagnostic;
struct MessageVectorSet;

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
  std::string canonical_text;
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
  bool preferred_renderer_partial{false};
  bool preferred_language_is_canonical_english{false};
  std::string preferred_language_profile{"preferred"};
  std::string canonical_english_profile{"sbsql.builtin.recovery.en"};
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

enum class SblrRenderLossiness {
  kLosslessCanonical,
  kCanonicalEquivalent,
  kPreferredLanguagePartial,
  kCanonicalEnglishFallback,
  kNotRenderable,
};

struct SblrRenderSelection {
  SblrRenderDecision decision{SblrRenderDecision::kRefuseRendererUnavailable};
  SblrRenderLossiness lossiness{SblrRenderLossiness::kNotRenderable};
  std::string selected_language_profile;
  std::string fallback_language_profile;
  bool used_canonical_english_fallback{false};
  bool server_revalidation_required{true};
  std::string diagnostic_code;
  std::string diagnostic_message;
};

enum class LanguageResourceFailureKind {
  kMissingResource,
  kUnsignedResource,
  kRevokedResource,
  kExpiredResource,
  kIncompatibleResource,
  kUnsupportedChannel,
  kAmbiguousFallback,
  kUnsupportedRenderer,
  kTopologyDialectUnicodeUnsupported,
  kPredictiveResourceRefused,
  kLocalDraftSblrRefused,
};

struct LanguageResourceFailureDiagnosticInput {
  LanguageResourceFailureKind failure_kind{LanguageResourceFailureKind::kMissingResource};
  bool telemetry_export{true};
  bool support_bundle_export{true};
  bool server_revalidation_required{true};
  std::string resource_identity;
  std::string language_profile_uuid;
  std::string exact_tag;
  std::string dialect_profile_uuid;
  std::string topology_profile_uuid;
  std::string query_text;
  std::string hidden_identifier;
  std::string local_path;
  std::string local_sblr_payload;
};

struct LanguageResourceLimits {
  std::uint64_t max_predictive_table_bytes{1024 * 1024};
  std::uint64_t max_transition_fanout{128};
  std::uint64_t max_completion_results{256};
  std::uint64_t max_generation_millis{100};
  std::uint64_t max_predictive_memory_bytes{4 * 1024 * 1024};
  std::uint64_t max_nested_expansion_depth{16};
};

struct PredictiveTextResourceFootprint {
  std::string resource_identity;
  std::uint64_t table_bytes{0};
  std::uint64_t transition_fanout{0};
  std::uint64_t completion_results{0};
  std::uint64_t generation_millis{0};
  std::uint64_t memory_bytes{0};
  std::uint64_t nested_expansion_depth{0};
  bool deterministic_limit_enforcement{true};
  bool hidden_object_no_disclosure{true};
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
  std::string release_channel_evidence_id;
  std::string deprecation_notice_id;
  std::string deprecation_replacement_profile_uuid;
  std::string revocation_notice_id;
  std::string removal_notice_id;
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

struct LanguageResourceLifecycleClassification {
  bool validation_allowed{true};
  bool load_allowed{true};
  bool use_allowed{true};
  bool support_claim_allowed{false};
  bool emits_lifecycle_warning{false};
  bool emits_deprecation_warning{false};
  std::string support_class;
  std::string diagnostic_code;
  std::string diagnostic_message;
};

struct LanguageResourceBundleManifest {
  std::string bundle_schema_version{"sbsql.language_resource_bundle.v1"};
  std::string bundle_uuid;
  std::string bundle_contract_id;
  std::string exact_tag;
  std::string dialect_profile_uuid;
  std::string topology_profile_uuid;
  std::string common_resource_hash;
  std::string canonical_element_stream_schema_hash;
  std::string predictive_resource_hash;
  std::string renderer_resource_hash;
  std::string diagnostic_resource_hash;
  std::string compatibility_identity{"sbsql.resource.compat.v1"};
  std::string lifecycle_state{"staged"};
  LanguageResourceManifest language_profile;
  std::vector<LanguageDataProvenance> provenance;
  bool signed_bundle{true};
  bool compatible_with_parser{true};
  bool admitted_by_security_policy{true};
  bool parser_language_library{false};
  bool active_profile{false};
  bool required_profile{false};
};

struct LanguageElementKeyword {
  std::string keyword_id;
  std::string text;
  std::string canonical_id;
  std::string surface_id;
  bool reserved{false};
  bool contextual{true};
};

struct LanguageElementTopologySlot {
  std::string slot_id;
  std::string phrase_id;
  std::string topology_role;
  std::string canonical_id;
  std::string surface_id;
  std::uint32_t min_elements{1};
  std::uint32_t max_elements{1};
};

struct LanguageElementSurface {
  std::string surface_id;
  std::string canonical_name;
  std::string surface_kind;
  std::string family;
  std::string sblr_operation_family;
  std::string topology_slot_id;
  std::string predictive_state_id;
  std::string renderer_id;
  std::string compatibility_id;
  std::string diagnostic_code;
  std::string message_id;
  LanguageResourceChannel release_channel{LanguageResourceChannel::kExperimental};
};

struct LanguageElementPredictiveState {
  std::string state_id;
  std::string surface_id;
  std::string transition_table_hash;
  bool completion_enabled{true};
  bool server_revalidation_required{true};
};

struct LanguageElementRenderer {
  std::string renderer_id;
  std::string profile_uuid;
  std::string canonical_english_fallback_profile_uuid{"sbsql.builtin.recovery.en"};
  SblrRenderLossiness lossiness{SblrRenderLossiness::kCanonicalEquivalent};
  bool server_revalidation_required{true};
};

struct LanguageElementDiagnosticMessage {
  std::string diagnostic_code;
  std::string message_id;
  std::string severity;
  std::string message_template_hash;
};

struct LanguageElementManifest {
  std::string manifest_schema_version{"sbsql.language_element_manifest.v1"};
  std::string manifest_uuid;
  std::string profile_uuid;
  std::string exact_tag;
  std::string dialect_profile_uuid;
  std::string topology_profile_uuid;
  std::string common_resource_hash;
  std::string compatibility_identity{"sbsql.resource.compat.v1"};
  LanguageResourceManifest language_profile;
  LanguageResourceBundleManifest bundle_manifest;
  std::vector<LanguageElementKeyword> keywords;
  std::vector<LanguageElementTopologySlot> topology_slots;
  std::vector<LanguageElementSurface> surfaces;
  std::vector<LanguageElementPredictiveState> predictive_states;
  std::vector<LanguageElementRenderer> renderers;
  std::vector<std::string> compatibility_ids;
  std::vector<LanguageElementDiagnosticMessage> diagnostics;
  std::vector<LanguageDataProvenance> provenance;
};

enum class LanguageBundleOperation {
  kValidate,
  kLoad,
  kUnload,
};

struct LanguageBundleAdmissionRequest {
  LanguageBundleOperation operation{LanguageBundleOperation::kValidate};
  LanguageResourceBundleManifest bundle;
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
  bool fail_closed_on_mismatch{true};
  bool server_revalidation_authority{true};
  std::string authority_boundary{"client_resources_are_untrusted_until_server_revalidation"};
  std::vector<std::string> syntax_profile_order{
      "explicit_syntax_profile",
      "preferred_language_and_dialect",
      "canonical_english_fallback_when_preferred_fails",
      "fail_closed"};
  std::vector<std::string> renderer_lossiness_classes{
      "lossless_canonical",
      "canonical_equivalent",
      "preferred_language_partial",
      "canonical_english_fallback",
      "not_renderable"};
  std::vector<std::string> fallback_diagnostic_codes{
      "SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH",
      "SBSQL.LANG_RESOURCE.FAIL_CLOSED_ON_PROFILE_MISMATCH"};
  std::vector<std::string> rendering_diagnostic_codes{
      "SBSQL.LANG_RESOURCE.RENDERER_LOSSINESS_CLASSIFIED",
      "SBSQL.LANG_RESOURCE.RENDERER_SOURCE_RECONSTRUCTION_FORBIDDEN",
      "SBSQL.LANG_RESOURCE.RENDERER_NOT_RENDERABLE"};
};

const LanguageResourceManifest& BuiltInCanonicalEnglishRecoveryProfile();
ResourceValidationResult ValidateLanguageResourceManifest(const LanguageResourceManifest& manifest);
ResourceValidationResult ValidateLanguageElementManifest(const LanguageElementManifest& manifest);
ResourceValidationResult ValidatePredictiveTextResourceFootprint(
    const PredictiveTextResourceFootprint& footprint,
    const LanguageResourceLimits& limits);
ResourceValidationResult ValidateLanguageResourceBundleManifest(
    const LanguageResourceBundleManifest& bundle);
ResourceValidationResult AdmitLanguageResourceBundleOperation(
    const LanguageBundleAdmissionRequest& request);
ResourceValidationResult ValidateEditorToolProtocol(const EditorToolProtocol& protocol);
ResourceValidationResult ValidateCanonicalElementStream(const CanonicalElementStream& stream);
ResourceValidationResult ValidateParseProfileOrder(const std::vector<ParseProfileStep>& order);
LocaleLiteralClassification ClassifyLocaleLiteral(std::string_view literal,
                                                  const LocaleLiteralPolicy& policy);
bool HasMixedScriptOrConfusableRisk(std::string_view text, const ConfusablePolicy& policy);
RestoreLanguageResourceState ClassifyRestoreLanguageResourceState(
    const LanguageResourceRestoreRequest& request);
LanguageResourceLifecycleClassification ClassifyLanguageResourceLifecycle(
    const LanguageResourceManifest& manifest);
ParseProfileDecision SelectParseProfile(const ParseProfileDecisionInput& input);
SblrRenderDecision ClassifySblrRenderRequest(const SblrRenderRequest& request);
SblrRenderSelection ClassifySblrRenderSelection(const SblrRenderRequest& request);
Diagnostic MakeLanguageResourceFailureDiagnostic(
    const LanguageResourceFailureDiagnosticInput& input);
MessageVectorSet MakeLanguageResourceFailureMessageVector(
    const LanguageResourceFailureDiagnosticInput& input);
std::vector<ParseProfileStep> DefaultParseProfileOrder();

std::string_view LanguageResourceChannelName(LanguageResourceChannel channel);
std::string_view ParseProfileStepName(ParseProfileStep step);
std::string_view ParseProfileDecisionName(ParseProfileDecision decision);
std::string_view SblrRenderDecisionName(SblrRenderDecision decision);
std::string_view SblrRenderLossinessName(SblrRenderLossiness lossiness);
std::string_view LanguageBundleOperationName(LanguageBundleOperation operation);
std::string_view RestoreLanguageResourceStateName(RestoreLanguageResourceState state);
std::string_view LanguageResourceFailureKindName(LanguageResourceFailureKind kind);
std::string_view LanguageResourceFailureDiagnosticCode(LanguageResourceFailureKind kind);

} // namespace scratchbird::parser::sbsql
