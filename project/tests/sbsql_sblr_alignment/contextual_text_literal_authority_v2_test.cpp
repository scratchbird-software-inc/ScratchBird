// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "engine/internal_api/query/contextual_text_graph_authority_verifier_v2.hpp"
#include "engine/internal_api/query/contextual_text_policy_registry_v2.hpp"
#include "engine/internal_api/query/contextual_text_target_authority_resolver_v2.hpp"
#include "engine/internal_api/sblr_executor_availability_registry.hpp"
#include "engine/sblr/contextual_text_literal_v2_codec.hpp"
#include "engine/sblr/sblr_engine_envelope.hpp"
#include "engine/sblr/sblr_literal_runtime.hpp"
#include "engine/sblr/sblr_opcode_stream.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef NDEBUG
#undef assert
#define assert(condition) \
  ((condition) ? static_cast<void>(0) : std::abort())
#endif

namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

namespace {

using Bytes = std::vector<std::uint8_t>;
using Sha256 = sblr::ContextualTextSha256V2;
using Uuid = sblr::ContextualTextUuidV2;

std::string NewIdentity(scratchbird::core::platform::UuidKind kind,
                        std::uint64_t ordinal) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
      kind, 1787900000000ull + ordinal);
  assert(generated.ok());
  return scratchbird::core::uuid::UuidToString(generated.value.value);
}

Uuid WireUuid(std::string_view text) {
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  assert(parsed.ok());
  Uuid result{};
  std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(),
            result.begin());
  return result;
}

Sha256 TaggedSha(std::uint8_t tag) {
  Sha256 result{};
  result[0] = tag;
  result[31] = static_cast<std::uint8_t>(tag ^ 0xa5u);
  return result;
}

void Put16(Bytes* bytes, std::size_t offset, std::uint16_t value) {
  (*bytes)[offset] = static_cast<std::uint8_t>(value);
  (*bytes)[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void Put32(Bytes* bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned index = 0; index != 4; ++index) {
    (*bytes)[offset + index] =
        static_cast<std::uint8_t>(value >> (8 * index));
  }
}

void Append16(Bytes* bytes, std::uint16_t value) {
  bytes->push_back(static_cast<std::uint8_t>(value));
  bytes->push_back(static_cast<std::uint8_t>(value >> 8));
}

void Append32(Bytes* bytes, std::uint32_t value) {
  for (unsigned index = 0; index != 4; ++index) {
    bytes->push_back(static_cast<std::uint8_t>(value >> (8 * index)));
  }
}

void Append64(Bytes* bytes, std::uint64_t value) {
  for (unsigned index = 0; index != 8; ++index) {
    bytes->push_back(static_cast<std::uint8_t>(value >> (8 * index)));
  }
}

std::string UuidText(const Uuid& value) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(36);
  for (std::size_t index = 0; index != value.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      result.push_back('-');
    }
    result.push_back(hex[value[index] >> 4]);
    result.push_back(hex[value[index] & 0x0f]);
  }
  return result;
}

std::string HexText(const std::string_view value) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() * 2);
  for (const auto ch : value) {
    const auto byte = static_cast<std::uint8_t>(ch);
    result.push_back(hex[byte >> 4]);
    result.push_back(hex[byte & 0x0f]);
  }
  return result;
}

sblr::SblrOperand TypedOperand(std::uint32_t ordinal,
                               std::string type,
                               std::string name,
                               std::string value,
                               const Uuid& type_uuid) {
  sblr::SblrOperand operand;
  operand.ordinal = ordinal;
  operand.type = std::move(type);
  operand.name = std::move(name);
  operand.value_kind = sblr::SblrValueKind::literal_typed;
  operand.value_body.insert(operand.value_body.end(), type_uuid.begin(),
                            type_uuid.end());
  Append64(&operand.value_body, value.size());
  operand.value_body.insert(operand.value_body.end(), value.begin(),
                            value.end());
  return operand;
}

sblr::SblrOperationEnvelope PackageFrame(
    bool begin, std::string_view parser_uuid, std::string_view registry_uuid,
    const Uuid& package_uuid) {
  auto operation = sblr::MakeSblrEnvelope(
      begin ? "engine.op.package_begin" : "engine.op.package_end",
      begin ? "SBLR_PACKAGE_BEGIN" : "SBLR_PACKAGE_END",
      begin ? "contextual.selector.begin" : "contextual.selector.end");
  operation.opcode_code = begin ? 1 : 2;
  operation.result_shape = "void";
  operation.diagnostic_shape = "diagnostic_vector";
  operation.parser_package_uuid = parser_uuid;
  operation.registry_snapshot_uuid = registry_uuid;
  operation.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = begin ? "package.header" : "package.footer";
  operand.name = "package_descriptor";
  operand.value_kind = sblr::SblrValueKind::descriptor_ref;
  operand.value_body.assign(package_uuid.begin(), package_uuid.end());
  operation.operands.push_back(std::move(operand));
  return operation;
}

Bytes LiteralReference(const std::uint64_t node_id, const Bytes& sbxn,
                       const Uuid& descriptor_uuid) {
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(sbxn);
  assert(digest.ok());
  Bytes result;
  Append16(&result, 1);
  Append16(&result, 0);
  Append32(&result, 1);
  Append64(&result, node_id);
  result.insert(result.end(), digest.digest.begin(), digest.digest.end());
  result.insert(result.end(), descriptor_uuid.begin(), descriptor_uuid.end());
  Append64(&result, 1);
  assert(result.size() == 72);
  return result;
}

api::EngineRequestContext Context(std::uint64_t ordinal) {
  api::EngineRequestContext context;
  context.database_path =
      (std::filesystem::temp_directory_path() /
       ("sb_contextual_text_authority_v2_" + std::to_string(ordinal)))
          .string();
  context.database_uuid.canonical = NewIdentity(
      scratchbird::core::platform::UuidKind::database, ordinal * 16 + 1);
  context.session_uuid.canonical = NewIdentity(
      scratchbird::core::platform::UuidKind::object, ordinal * 16 + 2);
  context.transaction_uuid.canonical = NewIdentity(
      scratchbird::core::platform::UuidKind::object, ordinal * 16 + 3);
  context.statement_uuid.canonical = NewIdentity(
      scratchbird::core::platform::UuidKind::object, ordinal * 16 + 4);
  context.statement_snapshot_uuid.canonical = NewIdentity(
      scratchbird::core::platform::UuidKind::object, ordinal * 16 + 5);
  context.statement_receipt_uuid.canonical = NewIdentity(
      scratchbird::core::platform::UuidKind::object, ordinal * 16 + 6);
  context.datatype_catalog_snapshot_uuid.canonical =
      "019d0000-0000-7000-8000-00000000d701";
  context.datatype_catalog_generation = 1;
  context.datatype_registry_generation = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.security_context_present = true;
  context.trace_tags = {"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};
  return context;
}

sblr::ContextualTextDescriptorV2 Descriptor(
    const api::EngineRequestContext& context) {
  const auto policies =
      api::LoadCurrentEngineContextualTextPolicyRowSetForPublicationV2();
  assert(policies.ok);
  sblr::ContextualTextDescriptorV2 descriptor;
  descriptor.flags = 1;
  descriptor.malformed_sequence_policy = 1;
  descriptor.null_encoding = 1;
  descriptor.descriptor_uuid =
      WireUuid("019d0000-0000-7000-8000-00000000d718");
  descriptor.descriptor_generation = 1;
  descriptor.type_uuid =
      WireUuid("019d0000-0000-7000-8000-00000000d719");
  descriptor.type_generation = 1;
  descriptor.codec_uuid =
      WireUuid("019d0000-0000-7000-8000-00000000d71a");
  descriptor.codec_version = 1;
  descriptor.codec_generation = 1;
  descriptor.character_limit = 256;
  descriptor.byte_limit = 1024;
  descriptor.charset_uuid = WireUuid(context.session_uuid.canonical);
  descriptor.charset_generation = 1;
  descriptor.collation_uuid = WireUuid(context.transaction_uuid.canonical);
  descriptor.collation_generation = 1;
  descriptor.normalization_policy_uuid =
      policies.rows.normalization.identity_uuid;
  descriptor.normalization_policy_generation =
      policies.rows.normalization.generation;
  descriptor.render_policy_uuid = policies.rows.render.identity_uuid;
  descriptor.render_policy_generation = policies.rows.render.generation;
  descriptor.canonicalization_profile_uuid =
      policies.rows.canonicalization.identity_uuid;
  descriptor.canonicalization_profile_generation =
      policies.rows.canonicalization.generation;
  descriptor.comparison_contract_uuid =
      policies.rows.comparison.identity_uuid;
  descriptor.comparison_contract_generation =
      policies.rows.comparison.generation;
  descriptor.equality_operation_uuid = policies.rows.equality.identity_uuid;
  descriptor.equality_operation_generation = policies.rows.equality.generation;
  descriptor.datatype_catalog_snapshot_uuid =
      WireUuid(context.datatype_catalog_snapshot_uuid.canonical);
  descriptor.datatype_catalog_generation = 1;
  descriptor.datatype_registry_generation = 1;
  descriptor.resource_epoch = context.resource_epoch;
  return descriptor;
}

std::array<std::string, 17> RelationalDescriptorFields(
    const sblr::ContextualTextLiteralProfileV2& profile,
    const std::string_view nullability) {
  return {UuidText(profile.descriptor_uuid),
          std::to_string(profile.descriptor_generation),
          UuidText(profile.type_uuid),
          std::to_string(profile.type_generation),
          sblr::kContextualTextCodecIdentifierV2,
          std::to_string(profile.codec_version),
          std::to_string(profile.codec_generation),
          std::string(nullability),
          UuidText(profile.collation_uuid),
          "-",
          std::to_string(profile.target_character_limit),
          "-",
          "-",
          UuidText(profile.statement_receipt_uuid),
          UuidText(profile.catalog_snapshot_uuid),
          std::to_string(profile.catalog_generation),
          std::to_string(profile.datatype_registry_generation)};
}

std::string JoinRelationalDescriptorFields(
    const std::array<std::string, 17>& fields) {
  std::string result;
  for (std::size_t index = 0; index != fields.size(); ++index) {
    if (index != 0) result.push_back('|');
    result.append(fields[index]);
  }
  return result;
}

class FakeTargetResolver final
    : public api::EngineContextualTextTargetAuthorityResolverV2 {
 public:
  explicit FakeTargetResolver(const api::EngineRequestContext& context,
                              const std::size_t target_name_padding = 0,
                              const std::string_view descriptor_kind =
                                  "canonical_type_descriptor") {
    const auto descriptor = Descriptor(context);
    sblr::ContextualTextCodecDiagnosticV2 diagnostic;
    assert(sblr::EncodeContextualTextDescriptorV2(
        descriptor, &exact_descriptor_, &diagnostic));
    api::EnginePublicRelationProjectionV3 projection;
    projection.relation_descriptor_uuid =
        WireUuid(context.transaction_uuid.canonical);
    projection.relation_uuid = WireUuid(context.session_uuid.canonical);
    projection.schema_uuid = WireUuid(context.database_uuid.canonical);
    projection.relation_descriptor_generation = 1;
    projection.resource_epoch = context.resource_epoch;
    projection.catalog_snapshot_uuid =
        WireUuid(context.datatype_catalog_snapshot_uuid.canonical);
    projection.catalog_generation = context.datatype_catalog_generation;
    projection.registry_generation = context.datatype_registry_generation;
    api::EnginePublicRelationProjectionColumnV3 column;
    column.column_uuid = WireUuid(context.statement_uuid.canonical);
    column.ordinal = 2;
    column.canonical_name =
        "value_text" + std::string(target_name_padding, 'p');
    column.descriptor_uuid = descriptor.descriptor_uuid;
    column.descriptor_kind = descriptor_kind;
    column.canonical_type_name = "text";
    column.encoded_type_descriptor =
        "canonical=text;datatype_descriptor_uuid=" +
        UuidText(descriptor.descriptor_uuid) +
        ";datatype_descriptor_generation=1;type_uuid=" +
        UuidText(descriptor.type_uuid) +
        ";type_generation=1;codec_uuid=" + UuidText(descriptor.codec_uuid) +
        ";codec_id=" + std::string(sblr::kContextualTextCodecIdentifierV2) +
        ";codec_version=1;codec_generation=1;null_encoding=1;"
        "nullability=nullable;charset_uuid=" +
        UuidText(descriptor.charset_uuid) +
        ";charset_generation=1;collation_uuid=" +
        UuidText(descriptor.collation_uuid) +
        ";collation_generation=1;resource_epoch=1;character_length=256";
    column.attributes = 0x01;
    column.charset_uuid = descriptor.charset_uuid;
    column.charset_name = "utf8";
    column.collation_uuid = descriptor.collation_uuid;
    column.collation_name = "binary";
    column.character_length = 256;
    column.charset_min_bytes = 1;
    column.charset_max_bytes = 4;
    column.identity_present = true;
    column.descriptor_generation = descriptor.descriptor_generation;
    column.type_uuid = descriptor.type_uuid;
    column.type_generation = descriptor.type_generation;
    column.codec_id = sblr::kContextualTextCodecIdentifierV2;
    column.codec_version = descriptor.codec_version;
    column.codec_generation = descriptor.codec_generation;
    column.canonical_value_width = 0;
    column.null_encoding = 1;
    projection.columns.push_back(std::move(column));
    api::EngineApiDiagnostic projection_diagnostic;
    assert(api::EncodeEnginePublicRelationProjectionV3(
        projection, &exact_projection_, &projection_diagnostic));
  }

  bool BindBudget(
      const api::EngineRequestContext&,
      const sblr::ContextualTextLiteralNegotiationRequestV2&,
      std::size_t,
      api::EngineContextualTextLiteralBudgetV2* budget,
      api::EngineApiDiagnostic*) const override {
    if (budget == nullptr) return false;
    budget->literal_negotiation_byte_grant = 65536;
    budget->canonical_body_aggregate_grant = 32563;
    return true;
  }

  bool ResolveTarget(
      const api::EngineRequestContext&,
      const sblr::ContextualTextLiteralDemandV2& demand,
      api::EngineResolvedContextualTextTargetV2* target,
      api::EngineApiDiagnostic*) const override {
    if (target == nullptr) return false;
    target->literal_occurrence = demand.literal_occurrence;
    target->exact_public_relation_projection_v3 = exact_projection_;
    target->exact_sbtltd02 = exact_descriptor_;
    return true;
  }

  bool RevalidateTarget(
      const api::EngineRequestContext&,
      const sblr::ContextualTextLiteralDemandV2& demand,
      const api::EngineResolvedContextualTextTargetV2& target,
      api::EngineApiDiagnostic* diagnostic) const override {
    ++revalidation_count;
    if (!stale && target.literal_occurrence == demand.literal_occurrence &&
        target.exact_public_relation_projection_v3 == exact_projection_ &&
        target.exact_sbtltd02 == exact_descriptor_) {
      return true;
    }
    if (diagnostic != nullptr) {
      diagnostic->code = "CTB.TEXT.RESOURCE_EPOCH_MISMATCH";
      diagnostic->message_key = "test.contextual_target_stale";
      diagnostic->error = true;
    }
    return false;
  }

  bool CopyComparisonResourceSnapshot(
      const api::EngineRequestContext&,
      const sblr::ContextualTextLiteralDemandV2&,
      const api::EngineResolvedContextualTextTargetV2& target,
      const sblr::ContextualTextLiteralProfileV2& profile,
      api::EngineContextualTextComparisonResourceSnapshotV2* snapshot)
      const override {
    if (snapshot == nullptr) return false;
    ++comparison_snapshot_count;
    api::EngineContextualTextComparisonResourceSnapshotV2 value;
    value.charset_uuid = profile.charset_uuid;
    value.charset_generation = profile.charset_generation;
    value.charset_uuid_canonical = UuidText(profile.charset_uuid);
    value.charset_name = "utf8";
    value.charset_resource_epoch = profile.resource_epoch;
    value.charset_family_epoch = profile.charset_generation;
    value.collation_uuid = profile.collation_uuid;
    value.collation_generation = profile.collation_generation;
    value.collation_uuid_canonical = UuidText(profile.collation_uuid);
    value.collation_name = "binary";
    value.collation_resource_epoch = profile.resource_epoch;
    value.collation_family_epoch = profile.collation_generation;
    value.text_seed.active = true;
    value.text_seed.seed_pack_name =
        resource_mutation ? "test.seed.changed" : "test.seed";
    value.text_seed.seed_pack_version = "1";
    value.text_seed.charset_name = value.charset_name;
    value.text_seed.collation_name = value.collation_name;
    value.charset_resource.present = true;
    value.charset_resource.resource_family = "charset";
    value.charset_resource.canonical_name = value.charset_name;
    value.charset_resource.resource_uuid.canonical =
        value.charset_uuid_canonical;
    value.charset_resource.resource_epoch = value.charset_resource_epoch;
    value.charset_resource.family_epoch = value.charset_family_epoch;
    value.charset_resource.family_version = "1";
    value.charset_resource.min_bytes = 1;
    value.charset_resource.max_bytes = 4;
    value.charset_resource.variable_width = true;
    value.collation_resource.present = true;
    value.collation_resource.resource_family = "collation";
    value.collation_resource.canonical_name = value.collation_name;
    value.collation_resource.resource_uuid.canonical =
        value.collation_uuid_canonical;
    value.collation_resource.parent_resource_uuid.canonical =
        value.charset_uuid_canonical;
    value.collation_resource.parent_canonical_name = value.charset_name;
    value.collation_resource.seed_pack_name = value.text_seed.seed_pack_name;
    value.collation_resource.seed_pack_version =
        value.text_seed.seed_pack_version;
    value.collation_resource.resource_epoch =
        value.collation_resource_epoch;
    value.collation_resource.family_epoch = value.collation_family_epoch;
    value.collation_resource.family_version = "1";
    value.target_projection_sha256 = profile.target_projection_sha256;
    value.descriptor_evidence_sha256 = profile.descriptor_evidence_sha256;
    value.exact_public_relation_projection_v3 =
        target.exact_public_relation_projection_v3;
    value.exact_sbtltd02 = target.exact_sbtltd02;
    *snapshot = std::move(value);
    return true;
  }

  bool stale = false;
  bool resource_mutation = false;
  mutable std::uint32_t revalidation_count = 0;
  mutable std::uint32_t comparison_snapshot_count = 0;

  const Bytes& exact_descriptor() const noexcept { return exact_descriptor_; }
  const Bytes& exact_projection() const noexcept { return exact_projection_; }

 private:
  Bytes exact_projection_;
  Bytes exact_descriptor_;
};

class FakeGraphVerifier final
    : public api::EngineContextualTextGraphAuthorityVerifierV2 {
 public:
  explicit FakeGraphVerifier(bool accept, bool mutate_binding = false,
                             bool mutate_target_nullability = false)
      : accept_(accept),
        mutate_binding_(mutate_binding),
        mutate_target_nullability_(mutate_target_nullability) {}

  bool VerifyPrepareEvidence(
      const api::EngineRequestContext&,
      const sblr::ContextualTextLiteralNegotiationRequestV2& issued_request,
      const sblr::ContextualTextLiteralExecuteV2& execute,
      const Bytes&,
      const Bytes&,
      const api::EngineContextualTextComposedTransferRecordV2&,
      const Bytes&,
      std::uint32_t,
      const Bytes&,
      std::vector<api::EngineContextualTextVerifiedGraphBindingV2>*
          verified_bindings,
      api::EngineApiDiagnostic* diagnostic) const override {
    if (accept_ && verified_bindings != nullptr &&
        issued_request.demands.size() == execute.mappings.size()) {
      verified_bindings->clear();
      verified_bindings->reserve(execute.mappings.size());
      for (std::size_t index = 0; index != execute.mappings.size(); ++index) {
        const auto& mapping = execute.mappings[index];
        const auto& demand = issued_request.demands[index];
        api::EngineContextualTextVerifiedGraphBindingV2 binding;
        binding.literal_occurrence = mapping.literal_occurrence;
        binding.node_id = mapping.node_id;
        binding.literal_expression_id =
            static_cast<std::uint32_t>(20 + index * 3);
        binding.comparison_expression_id =
            static_cast<std::uint32_t>(mapping.profile.comparison_occurrence);
        binding.target_expression_id =
            static_cast<std::uint32_t>(22 + index * 3);
        binding.source_node_id =
            static_cast<std::uint32_t>(demand.source_node_id);
        binding.literal_descriptor_handle =
            mapping.literal_descriptor_handle;
        binding.target_descriptor_handle = mapping.target_descriptor_handle;
        binding.exact_relational_descriptor_v2_fields =
            RelationalDescriptorFields(mapping.profile, "0");
        binding.canonical_type_name = "text";
        binding.element_profile_empty = true;
        binding.exact_target_relational_descriptor_v2_fields =
            RelationalDescriptorFields(mapping.profile, "1");
        binding.target_canonical_type_name = "text";
        binding.target_element_profile_empty = true;
        if (mutate_binding_) ++binding.target_descriptor_handle;
        if (mutate_target_nullability_) {
          binding.exact_target_relational_descriptor_v2_fields[7] = "0";
        }
        verified_bindings->push_back(std::move(binding));
      }
      return true;
    }
    if (diagnostic != nullptr) {
      diagnostic->code = "SBLR.OPERAND_INVALID";
      diagnostic->message_key = "test.contextual_graph_mismatch";
      diagnostic->error = true;
    }
    return false;
  }

 private:
  bool accept_ = false;
  bool mutate_binding_ = false;
  bool mutate_target_nullability_ = false;
};

sblr::ContextualTextLiteralNegotiationRequestV2 Request(
    const api::EngineRequestContext& context,
    const std::string_view literal_body = "x") {
  assert(!literal_body.empty() &&
         literal_body.find('\'') == std::string_view::npos);
  sblr::ContextualTextLiteralNegotiationRequestV2 request;
  request.statement_receipt_uuid =
      WireUuid(context.statement_receipt_uuid.canonical);
  request.catalog_snapshot_uuid =
      WireUuid(context.datatype_catalog_snapshot_uuid.canonical);
  request.catalog_generation = context.datatype_catalog_generation;
  request.datatype_registry_generation = context.datatype_registry_generation;
  request.security_generation = context.security_epoch;
  request.resource_epoch = context.resource_epoch;
  request.mga_snapshot_uuid =
      WireUuid(context.statement_snapshot_uuid.canonical);
  sblr::ContextualTextLiteralDemandV2 demand;
  demand.literal_occurrence = 1;
  demand.literal_argument_ordinal = 1;
  demand.target_argument_ordinal = 2;
  demand.comparison_occurrence = 21;
  demand.source_node_id = 10;
  demand.source_operand_ordinal = 1;
  demand.source_ordinal = 0;
  demand.relation_uuid = WireUuid(context.session_uuid.canonical);
  demand.relation_descriptor_uuid =
      WireUuid(context.transaction_uuid.canonical);
  demand.relation_descriptor_generation = 1;
  demand.column_uuid = WireUuid(context.statement_uuid.canonical);
  demand.column_ordinal = 2;
  demand.parent_operand_ordinal = 1;
  demand.node_id = 101;
  demand.target_descriptor_handle = 3;
  demand.literal_descriptor_handle = 9;
  demand.scalar_count = static_cast<std::uint32_t>(literal_body.size());
  demand.raw_token.push_back('\'');
  demand.raw_token.insert(demand.raw_token.end(), literal_body.begin(),
                          literal_body.end());
  demand.raw_token.push_back('\'');
  demand.lexical_value.assign(literal_body.begin(), literal_body.end());
  request.demands.push_back(std::move(demand));
  return request;
}

api::EngineContextualTextLiteralAuthorityIssueResultV2 Issue(
    const api::EngineRequestContext& context,
    FakeTargetResolver* resolver,
    sblr::ContextualTextLiteralNegotiationRequestV2* decoded_request,
    const std::string_view literal_body = "x") {
  auto request = Request(context, literal_body);
  Bytes encoded;
  sblr::ContextualTextCodecDiagnosticV2 diagnostic;
  assert(sblr::EncodeContextualTextLiteralNegotiationRequestV2(
      request, &encoded, &diagnostic));
  assert(sblr::DecodeContextualTextLiteralNegotiationRequestV2(
      encoded.data(), encoded.size(), decoded_request, &diagnostic));
  api::EngineContextualTextLiteralAuthorityIssueRequestV2 issue;
  issue.context = context;
  issue.exact_sbtlnr02 = std::move(encoded);
  issue.target_resolver = resolver;
  return api::IssueContextualTextLiteralAuthorityV2(issue);
}

Bytes ContextualSbxn(const sblr::ContextualTextLiteralProfileSetV2& profiles) {
  assert(profiles.mappings.size() == 1);
  const auto& profile = profiles.mappings.front().profile;
  sblr::SblrExpressionNodeTableV1 table;
  sblr::SblrExpressionLiteralNodeV1 node;
  node.node_id = profile.node_id;
  node.parent_operand_ordinal = profile.parent_operand_ordinal;
  node.descriptor_uuid = profile.descriptor_uuid;
  node.descriptor_generation = profile.descriptor_generation;
  node.literal_body = profile.canonical_body;
  table.nodes.push_back(std::move(node));
  auto encoded = sblr::EncodeSblrExpressionNodeTableV1(table);
  assert(!encoded.empty());
  return encoded;
}

api::EngineContextualTextLiteralTransferResultV2 Transfer(
    const api::EngineRequestContext& context,
    api::EngineContextualTextLiteralAuthorityHandleV2* authority,
    const Bytes& sbxn,
    std::uint8_t tag) {
  api::EngineContextualTextLiteralTransferRequestV2 request;
  request.context = context;
  request.authority = authority;
  request.final_receipt_uuid = WireUuid(NewIdentity(
      scratchbird::core::platform::UuidKind::object, 1000 + tag * 2));
  request.admission_token_uuid = WireUuid(NewIdentity(
      scratchbird::core::platform::UuidKind::object, 1001 + tag * 2));
  request.admission_token_binding_sha256 = TaggedSha(31 + tag);
  request.v1_demand_sha256 = TaggedSha(41 + tag);
  request.v1_ordered_profile_sha256 = TaggedSha(51 + tag);
  request.bound_ast_sha256 = TaggedSha(61 + tag);
  request.complete_sbxn_sha256 =
      sblr::ComputeContextualTextSbxnSha256V2(sbxn);
  return api::TransferContextualTextLiteralAuthorityV2(request);
}

Bytes Sbel(const api::EngineContextualTextComposedTransferRecordV2& transfer,
           const Bytes& sbos) {
  const auto sbos_digest =
      scratchbird::core::hash::ComputeSha256Digest(sbos);
  assert(sbos_digest.ok());
  Bytes result(176, 0);
  std::copy_n(reinterpret_cast<const std::uint8_t*>("SBEL"), 4,
              result.begin());
  Put16(&result, 4, 1);
  Put16(&result, 6, 176);
  Put32(&result, 8, 176);
  std::copy(transfer.final_receipt_uuid.begin(),
            transfer.final_receipt_uuid.end(), result.begin() + 16);
  std::copy(transfer.admission_token_uuid.begin(),
            transfer.admission_token_uuid.end(), result.begin() + 32);
  std::copy(transfer.admission_token_binding_sha256.begin(),
            transfer.admission_token_binding_sha256.end(),
            result.begin() + 48);
  std::copy(transfer.bound_ast_sha256.begin(),
            transfer.bound_ast_sha256.end(), result.begin() + 80);
  std::copy(transfer.complete_sbxn_sha256.begin(),
            transfer.complete_sbxn_sha256.end(), result.begin() + 112);
  std::copy(sbos_digest.digest.begin(), sbos_digest.digest.end(),
            result.begin() + 144);
  return result;
}

Bytes Execute(const sblr::ContextualTextLiteralProfileSetV2& profiles,
              const Bytes& pre_contextual_records,
              const Bytes& sbxn,
              const std::uint32_t pre_contextual_operand_count = 1) {
  sblr::ContextualTextLiteralExecuteV2 execute;
  static_cast<sblr::ContextualTextLiteralProfileSetV2&>(execute) = profiles;
  execute.pre_contextual_operand_vector_sha256 =
      sblr::ComputeContextualTextPreContextualOperandVectorSha256V2(
          pre_contextual_records, pre_contextual_operand_count);
  execute.sbxn_sha256 = sblr::ComputeContextualTextSbxnSha256V2(sbxn);
  Bytes encoded;
  sblr::ContextualTextCodecDiagnosticV2 diagnostic;
  assert(sblr::EncodeContextualTextLiteralExecuteV2(
      execute, &encoded, &diagnostic));
  return encoded;
}

struct CanonicalGraphEvidence {
  Bytes pre_contextual_records;
  std::uint32_t pre_contextual_operand_count = 0;
  Bytes execute;
  Bytes sbos;
};

enum class CanonicalGraphMutation {
  none,
  reverse_comparison_children,
  target_missing,
  comparison_missing,
  literal_missing,
  target_duplicate,
  comparison_duplicate,
  literal_duplicate,
  target_cross_source,
  comparison_cross_source,
  literal_cross_source,
  matching_target_output_without_binding,
};

std::string HandleList(const std::vector<std::uint32_t>& values) {
  if (values.empty()) return "-";
  std::string result;
  for (const auto value : values) {
    if (!result.empty()) result.push_back(',');
    result += std::to_string(value);
  }
  return result;
}

CanonicalGraphEvidence GraphEvidence(
    const api::EngineRequestContext& context,
    const sblr::ContextualTextLiteralProfileSetV2& profiles,
    const Bytes& sbxn,
    const CanonicalGraphMutation mutation) {
  assert(profiles.mappings.size() == 1);
  const auto& profile = profiles.mappings.front().profile;
  constexpr std::uint32_t source_node_id = 10;
  constexpr std::uint32_t literal_expression_id = 20;
  constexpr std::uint32_t comparison_expression_id = 21;
  constexpr std::uint32_t target_expression_id = 22;
  constexpr std::uint32_t public_output_expression_id = 23;
  assert(profile.node_id == 101 && profile.literal_occurrence == 1 &&
         profile.comparison_occurrence == comparison_expression_id &&
         profile.target_descriptor_handle == 3 &&
         profile.literal_descriptor_handle == 9 &&
         profile.column_ordinal == 2);

  std::vector<std::uint32_t> source_binding{
      public_output_expression_id, target_expression_id,
      comparison_expression_id, literal_expression_id};
  std::vector<std::uint32_t> second_source_binding;
  std::uint32_t output_expression_id = public_output_expression_id;
  const auto erase_once = [&](const std::uint32_t expression_id) {
    const auto found = std::ranges::find(source_binding, expression_id);
    assert(found != source_binding.end());
    source_binding.erase(found);
  };
  switch (mutation) {
    case CanonicalGraphMutation::target_missing:
      erase_once(target_expression_id);
      break;
    case CanonicalGraphMutation::comparison_missing:
      erase_once(comparison_expression_id);
      break;
    case CanonicalGraphMutation::literal_missing:
      erase_once(literal_expression_id);
      break;
    case CanonicalGraphMutation::target_duplicate:
      source_binding.push_back(target_expression_id);
      break;
    case CanonicalGraphMutation::comparison_duplicate:
      source_binding.push_back(comparison_expression_id);
      break;
    case CanonicalGraphMutation::literal_duplicate:
      source_binding.push_back(literal_expression_id);
      break;
    case CanonicalGraphMutation::target_cross_source:
      erase_once(target_expression_id);
      second_source_binding.push_back(target_expression_id);
      break;
    case CanonicalGraphMutation::comparison_cross_source:
      erase_once(comparison_expression_id);
      second_source_binding.push_back(comparison_expression_id);
      break;
    case CanonicalGraphMutation::literal_cross_source:
      erase_once(literal_expression_id);
      second_source_binding.push_back(literal_expression_id);
      break;
    case CanonicalGraphMutation::matching_target_output_without_binding:
      erase_once(target_expression_id);
      output_expression_id = target_expression_id;
      break;
    case CanonicalGraphMutation::none:
    case CanonicalGraphMutation::reverse_comparison_children:
      break;
  }

  std::vector<sblr::SblrOperand> operands;
  const auto push_typed = [&](const std::string& type,
                              const std::string& name,
                              const std::string& payload) {
    operands.push_back(TypedOperand(
        static_cast<std::uint32_t>(operands.size() + 1), type, name, payload,
        profile.descriptor_uuid));
  };
  push_typed("relational_node_v1", "slot_10", "1|0|-|3|-");
  push_typed("relational_node_binding_v1", "slot_10",
             HexText("relation.source.v1") + "|" +
                 HandleList(source_binding) + "|" +
                 UuidText(profile.relation_uuid) + "|-|-");
  if (!second_source_binding.empty()) {
    push_typed("relational_node_v1", "slot_11", "1|0|-|3|-");
    push_typed("relational_node_binding_v1", "slot_11",
               HexText("relation.source.v1") + "|" +
                   HandleList(second_source_binding) + "|" +
                   UuidText(profile.relation_uuid) + "|-|-");
  }

  const auto literal_descriptor_fields = JoinRelationalDescriptorFields(
      RelationalDescriptorFields(profile, "0"));
  const auto target_descriptor_fields = JoinRelationalDescriptorFields(
      RelationalDescriptorFields(profile, "1"));
  push_typed("relational_descriptor_v2", "slot_3",
             target_descriptor_fields);
  push_typed("relational_descriptor_v2", "slot_9",
             literal_descriptor_fields);
  push_typed("relational_expression_v1", "slot_22",
             "3|-|3|-|" + UuidText(profile.column_uuid) + "|-|-|-");
  push_typed("relational_expression_v1", "slot_23",
             "3|-|3|-|" + UuidText(profile.column_uuid) + "|-|-|-");
  const std::string children =
      mutation == CanonicalGraphMutation::reverse_comparison_children
          ? "22,20"
          : "20,22";
  push_typed("relational_expression_v1", "slot_21",
             "6|" + children + "|3|-|-|-|3d|-");

  sblr::SblrOperand reference;
  reference.ordinal = static_cast<std::uint32_t>(operands.size() + 1);
  reference.type = "relational_expression_v1";
  reference.name = std::to_string(literal_expression_id);
  reference.value_kind = sblr::SblrValueKind::expression_node_ref;
  reference.value_body =
      LiteralReference(profile.node_id, sbxn, profile.descriptor_uuid);
  operands.push_back(std::move(reference));
  push_typed(
      "relational_output_v1", "slot_1",
      std::to_string(source_node_id) + "|" +
          std::to_string(output_expression_id) + "|3|1|0|636f6c");

  sblr::SblrOperand table;
  table.ordinal = static_cast<std::uint32_t>(operands.size() + 1);
  table.type = "expression.node_table.v1";
  table.name = "expression_nodes";
  table.value_kind = sblr::SblrValueKind::expression_node_table;
  table.value_body = sbxn;
  operands.push_back(std::move(table));

  CanonicalGraphEvidence evidence;
  evidence.pre_contextual_operand_count =
      static_cast<std::uint32_t>(operands.size());
  evidence.pre_contextual_records =
      sblr::EncodeSblrCanonicalOperandRecords(operands);
  assert(!evidence.pre_contextual_records.empty());
  evidence.execute =
      Execute(profiles, evidence.pre_contextual_records, sbxn,
              evidence.pre_contextual_operand_count);

  auto query = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", "contextual.selector.query");
  query.opcode_code = 4615;
  query.operation_version_minor = 1;
  query.result_shape = "query_execute_result";
  query.diagnostic_shape = "diagnostic_vector";
  query.parser_package_uuid =
      "019d0000-0000-7000-8000-00000000f201";
  query.registry_snapshot_uuid =
      context.datatype_catalog_snapshot_uuid.canonical;
  query.parser_resolved_names_to_uuids = true;
  query.operands = std::move(operands);
  sblr::SblrOperand contextual;
  contextual.ordinal =
      static_cast<std::uint32_t>(query.operands.size() + 1);
  contextual.type = "literal.contextual_text_profile_set.v2";
  contextual.name = "contextual_text_profiles";
  contextual.value_kind =
      sblr::SblrValueKind::contextual_text_literal_profile_set;
  contextual.value_body = evidence.execute;
  query.operands.push_back(std::move(contextual));

  const auto package_uuid = WireUuid(context.statement_snapshot_uuid.canonical);
  sblr::SblrOpcodeStream stream;
  stream.package_descriptor_uuid =
      context.statement_snapshot_uuid.canonical;
  stream.registry_snapshot_uuid =
      context.datatype_catalog_snapshot_uuid.canonical;
  stream.operations = {
      PackageFrame(true, query.parser_package_uuid,
                   query.registry_snapshot_uuid, package_uuid),
      std::move(query),
      PackageFrame(false, "019d0000-0000-7000-8000-00000000f201",
                   context.datatype_catalog_snapshot_uuid.canonical,
                   package_uuid)};
  evidence.sbos = sblr::EncodeSblrOpcodeStream(stream);
  assert(!evidence.sbos.empty());
  const auto decoded_sbos = sblr::DecodeSblrOpcodeStream(
      std::string_view(reinterpret_cast<const char*>(evidence.sbos.data()),
                       evidence.sbos.size()));
  if (!decoded_sbos.ok) {
    std::cerr << "canonical graph SBOS decode failed: id="
              << decoded_sbos.diagnostic_id
              << " detail=" << decoded_sbos.detail << '\n';
  }
  assert(decoded_sbos.ok && decoded_sbos.stream.operations.size() == 3);
  return evidence;
}

void RemoveAvailabilityStore(const api::EngineRequestContext& context) {
  std::error_code error;
  std::filesystem::remove(
      context.database_path +
          ".sb.sblr_executor_availability_registry.v1."
          "contextual_text_literal_equality_v2",
      error);
}

api::SblrExecutorAvailabilityRowIdentity AvailabilityIdentity() {
  return {api::kSblrContextualTextLiteralExecutorId,
          api::kSblrContextualTextLiteralOpcodeCode,
          api::kSblrContextualTextLiteralOpcodeVersion,
          api::kSblrContextualTextLiteralOperandDescriptorId,
          api::kSblrContextualTextLiteralResultDescriptorId,
          api::kSblrContextualTextLiteralResultDescriptorVersion};
}

api::EngineContextualTextPreparedResourceEstimateV2 PreparedResourceEstimate(
    const std::uint64_t ordinal,
    const std::string_view literal_body,
    const std::size_t target_name_padding) {
  auto context = Context(ordinal);
  RemoveAvailabilityStore(context);
  FakeTargetResolver resolver(context, target_name_padding);
  sblr::ContextualTextLiteralNegotiationRequestV2 decoded_request;
  auto issued = Issue(context, &resolver, &decoded_request, literal_body);
  assert(issued.ok && issued.authority.valid());
  const auto sbxn = ContextualSbxn(issued.profile_set);
  api::EngineApiDiagnostic diagnostic;
  assert(api::ValidateContextualTextComposedSbxnPartitionV2(
      context, issued.authority, sbxn, {}, &diagnostic));
  auto transferred = Transfer(
      context, &issued.authority, sbxn,
      static_cast<std::uint8_t>((ordinal % 254) + 1));
  assert(transferred.ok);
  const auto graph = GraphEvidence(context, issued.profile_set, sbxn,
                                   CanonicalGraphMutation::none);
  auto verifier =
      api::CreateEngineContextualTextGraphAuthorityVerifierForReceiptV2(
          context);
  assert(verifier != nullptr);
  api::EngineContextualTextLiteralAuthorityPrepareRequestV2 request;
  request.context = context;
  request.authority = &issued.authority;
  request.target_resolver = &resolver;
  request.graph_verifier = verifier.get();
  request.exact_sbel_v1 = Sbel(transferred.record, graph.sbos);
  request.exact_canonical_sbos = graph.sbos;
  request.composed_transfer = &transferred.record;
  request.exact_sbtlxe02 = graph.execute;
  request.exact_pre_contextual_operand_records =
      graph.pre_contextual_records;
  request.pre_contextual_operand_count = graph.pre_contextual_operand_count;
  request.exact_sbxn = sbxn;
  auto prepared = api::PrepareContextualTextLiteralAuthorityV2(request);
  assert(prepared.ok && prepared.prepared.valid());

  const auto revalidations_before = resolver.revalidation_count;
  const auto resource_snapshots_before = resolver.comparison_snapshot_count;
  const auto estimate =
      api::EstimatePreparedContextualTextLiteralResourcesV2(
          prepared.prepared);
  const auto repeated =
      api::EstimatePreparedContextualTextLiteralResourcesV2(
          prepared.prepared);
  assert(estimate.ok && repeated.ok &&
         estimate.status ==
             api::EngineContextualTextResourceEstimateStatusV2::ok &&
         estimate.prepared_retained_bytes != 0 &&
         estimate.joint_incremental_peak_bytes != 0 &&
         estimate.post_consume_lease_retained_bytes != 0 &&
         estimate.prepared_retained_bytes ==
             repeated.prepared_retained_bytes &&
         estimate.joint_incremental_peak_bytes ==
             repeated.joint_incremental_peak_bytes &&
         estimate.post_consume_lease_retained_bytes ==
             repeated.post_consume_lease_retained_bytes &&
         prepared.prepared.valid() &&
         resolver.revalidation_count == revalidations_before &&
         resolver.comparison_snapshot_count == resource_snapshots_before);
  api::EngineContextualTextLiteralAuthoritySnapshotV2 snapshot;
  assert(api::CopyContextualTextLiteralAuthoritySnapshotV2(
      issued.authority, &snapshot, &diagnostic));
  assert(snapshot.state ==
         api::EngineContextualTextLiteralAuthorityStateV2::issued);
  assert(!api::RevokeContextualTextLiteralAuthorityV2(
              &issued.authority, "test.resource_estimate_complete")
              .error);
  RemoveAvailabilityStore(context);
  return estimate;
}

void RequireProjectionKindRefusal(const std::uint64_t ordinal,
                                  const std::string_view descriptor_kind) {
  auto context = Context(ordinal);
  RemoveAvailabilityStore(context);
  FakeTargetResolver resolver(context, 0, descriptor_kind);
  sblr::ContextualTextLiteralNegotiationRequestV2 decoded_request;
  auto issued = Issue(context, &resolver, &decoded_request);
  if (!issued.ok || issued.profile_set.mappings.size() != 1) {
    std::cerr << "projection-kind authority issue failed: code="
              << issued.diagnostic.code << " key="
              << issued.diagnostic.message_key << " detail="
              << issued.diagnostic.detail << " mappings="
              << issued.profile_set.mappings.size() << '\n';
    std::exit(EXIT_FAILURE);
  }
  assert(issued.ok && issued.authority.valid());
  const auto sbxn = ContextualSbxn(issued.profile_set);
  api::EngineApiDiagnostic diagnostic;
  assert(api::ValidateContextualTextComposedSbxnPartitionV2(
      context, issued.authority, sbxn, {}, &diagnostic));
  auto transferred = Transfer(
      context, &issued.authority, sbxn,
      static_cast<std::uint8_t>((ordinal % 254) + 1));
  assert(transferred.ok);
  const auto graph = GraphEvidence(context, issued.profile_set, sbxn,
                                   CanonicalGraphMutation::none);
  auto verifier =
      api::CreateEngineContextualTextGraphAuthorityVerifierForReceiptV2(
          context);
  assert(verifier != nullptr);
  api::EngineContextualTextLiteralAuthorityPrepareRequestV2 request;
  request.context = context;
  request.authority = &issued.authority;
  request.target_resolver = &resolver;
  request.graph_verifier = verifier.get();
  request.exact_sbel_v1 = Sbel(transferred.record, graph.sbos);
  request.exact_canonical_sbos = graph.sbos;
  request.composed_transfer = &transferred.record;
  request.exact_sbtlxe02 = graph.execute;
  request.exact_pre_contextual_operand_records =
      graph.pre_contextual_records;
  request.pre_contextual_operand_count = graph.pre_contextual_operand_count;
  request.exact_sbxn = sbxn;
  const auto prepared = api::PrepareContextualTextLiteralAuthorityV2(request);
  assert(!prepared.ok &&
         prepared.diagnostic.code == "CTB.TEXT.DESCRIPTOR_INVALID" &&
         prepared.diagnostic.message_key ==
             "engine.contextual_text_literal.target_projection_column_mismatch");
  api::EngineContextualTextLiteralAuthoritySnapshotV2 snapshot;
  assert(api::CopyContextualTextLiteralAuthoritySnapshotV2(
      issued.authority, &snapshot, &diagnostic));
  assert(snapshot.state ==
         api::EngineContextualTextLiteralAuthorityStateV2::issued);
  assert(!api::RevokeContextualTextLiteralAuthorityV2(
              &issued.authority, "test.projection_kind_refusal")
              .error);
  RemoveAvailabilityStore(context);
}

}  // namespace

int main() {
  RequireProjectionKindRefusal(31, "scalar");
  RequireProjectionKindRefusal(32, "canonical_type_descriptor ");
  auto context = Context(1);
  RemoveAvailabilityStore(context);
  FakeTargetResolver resolver(context);
  sblr::ContextualTextLiteralNegotiationRequestV2 decoded_request;
  auto issued = Issue(context, &resolver, &decoded_request);
  if (!issued.ok) {
    std::cerr << "contextual authority issue failed: code="
              << issued.diagnostic.code << " key="
              << issued.diagnostic.message_key << " detail="
              << issued.diagnostic.detail << '\n';
  }
  assert(issued.ok && issued.authority.valid());

  const auto sbxn = ContextualSbxn(issued.profile_set);
  api::EngineApiDiagnostic diagnostic;
  assert(api::ValidateContextualTextComposedSbxnPartitionV2(
      context, issued.authority, sbxn, {}, &diagnostic));
  auto transferred = Transfer(context, &issued.authority, sbxn, 1);
  assert(transferred.ok);

  const auto canonical_graph =
      GraphEvidence(context, issued.profile_set, sbxn,
                    CanonicalGraphMutation::none);
  const auto reversed_graph =
      GraphEvidence(context, issued.profile_set, sbxn,
                    CanonicalGraphMutation::reverse_comparison_children);
  const std::array ownership_mutations{
      CanonicalGraphMutation::target_missing,
      CanonicalGraphMutation::comparison_missing,
      CanonicalGraphMutation::literal_missing,
      CanonicalGraphMutation::target_duplicate,
      CanonicalGraphMutation::comparison_duplicate,
      CanonicalGraphMutation::literal_duplicate,
      CanonicalGraphMutation::target_cross_source,
      CanonicalGraphMutation::comparison_cross_source,
      CanonicalGraphMutation::literal_cross_source,
      CanonicalGraphMutation::matching_target_output_without_binding};
  std::vector<CanonicalGraphEvidence> invalid_ownership_graphs;
  invalid_ownership_graphs.reserve(ownership_mutations.size());
  for (const auto mutation : ownership_mutations) {
    invalid_ownership_graphs.push_back(
        GraphEvidence(context, issued.profile_set, sbxn, mutation));
  }
  const auto sbel = Sbel(transferred.record, canonical_graph.sbos);
  bool literal_admission_consumed = false;
  FakeGraphVerifier accept_graph(true);
  FakeGraphVerifier reject_graph(false);
  FakeGraphVerifier mutated_graph_binding(true, true);
  FakeGraphVerifier mutated_target_nullability(true, false, true);

  const auto prepare = [&](api::EngineContextualTextGraphAuthorityVerifierV2*
                               graph,
                           const CanonicalGraphEvidence& evidence) {
    api::EngineContextualTextLiteralAuthorityPrepareRequestV2 request;
    request.context = context;
    request.authority = &issued.authority;
    request.target_resolver = &resolver;
    request.graph_verifier = graph;
    request.exact_sbel_v1 = Sbel(transferred.record, evidence.sbos);
    request.exact_canonical_sbos = evidence.sbos;
    request.composed_transfer = &transferred.record;
    request.exact_sbtlxe02 = evidence.execute;
    request.exact_pre_contextual_operand_records =
        evidence.pre_contextual_records;
    request.pre_contextual_operand_count =
        evidence.pre_contextual_operand_count;
    request.exact_sbxn = sbxn;
    return api::PrepareContextualTextLiteralAuthorityV2(request);
  };

  auto default_graph =
      api::CreateEngineContextualTextGraphAuthorityVerifierForReceiptV2(
          context);
  assert(default_graph != nullptr);
  assert(!prepare(default_graph.get(), reversed_graph).ok);
  for (const auto& invalid_graph : invalid_ownership_graphs) {
    assert(!prepare(default_graph.get(), invalid_graph).ok);
  }
  api::EngineContextualTextLiteralAuthoritySnapshotV2 snapshot;
  assert(api::CopyContextualTextLiteralAuthoritySnapshotV2(
      issued.authority, &snapshot, &diagnostic));
  assert(snapshot.state ==
         api::EngineContextualTextLiteralAuthorityStateV2::issued);
  auto canonical_default_prepare =
      prepare(default_graph.get(), canonical_graph);
  if (!canonical_default_prepare.ok) {
    std::cerr << "canonical default selector prepare failed: code="
              << canonical_default_prepare.diagnostic.code
              << " key="
              << canonical_default_prepare.diagnostic.message_key
              << " detail="
              << canonical_default_prepare.diagnostic.detail << '\n';
  }
  assert(canonical_default_prepare.ok);

  resolver.stale = true;
  assert(!prepare(&accept_graph, canonical_graph).ok);
  resolver.stale = false;
  assert(!prepare(&reject_graph, canonical_graph).ok);
  assert(!prepare(&mutated_graph_binding, canonical_graph).ok);
  assert(!prepare(&mutated_target_nullability, canonical_graph).ok);
  assert(api::CopyContextualTextLiteralAuthoritySnapshotV2(
      issued.authority, &snapshot, &diagnostic));
  assert(snapshot.state ==
             api::EngineContextualTextLiteralAuthorityStateV2::issued &&
         !literal_admission_consumed);

  auto prepared_winner = std::move(canonical_default_prepare);
  auto prepared_loser = prepare(default_graph.get(), canonical_graph);
  assert(prepared_winner.ok && prepared_loser.ok);
  api::PreparedContextualTextLiteralSetV2 invalid_prepared;
  const auto invalid_estimate =
      api::EstimatePreparedContextualTextLiteralResourcesV2(
          invalid_prepared);
  assert(!invalid_estimate.ok &&
         invalid_estimate.status ==
             api::EngineContextualTextResourceEstimateStatusV2::invalid_owner);
  const auto prepared_estimate =
      api::EstimatePreparedContextualTextLiteralResourcesV2(
          prepared_winner.prepared);
  const auto repeated_prepared_estimate =
      api::EstimatePreparedContextualTextLiteralResourcesV2(
          prepared_winner.prepared);
  assert(prepared_estimate.ok && repeated_prepared_estimate.ok &&
         prepared_estimate.status ==
             api::EngineContextualTextResourceEstimateStatusV2::ok &&
         prepared_estimate.prepared_retained_bytes != 0 &&
         prepared_estimate.joint_incremental_peak_bytes != 0 &&
         prepared_estimate.post_consume_lease_retained_bytes != 0 &&
         prepared_estimate.prepared_retained_bytes ==
             repeated_prepared_estimate.prepared_retained_bytes &&
         prepared_estimate.joint_incremental_peak_bytes ==
             repeated_prepared_estimate.joint_incremental_peak_bytes &&
         prepared_estimate.post_consume_lease_retained_bytes ==
             repeated_prepared_estimate.post_consume_lease_retained_bytes &&
         prepared_winner.prepared.valid() && !literal_admission_consumed);
  const auto larger_body_estimate =
      PreparedResourceEstimate(21, std::string(128, 'x'), 0);
  const auto larger_target_estimate = PreparedResourceEstimate(22, "x", 256);
  assert(larger_body_estimate.prepared_retained_bytes >
             prepared_estimate.prepared_retained_bytes &&
         larger_body_estimate.joint_incremental_peak_bytes >
             prepared_estimate.joint_incremental_peak_bytes &&
         larger_body_estimate.post_consume_lease_retained_bytes >
             prepared_estimate.post_consume_lease_retained_bytes &&
         larger_target_estimate.prepared_retained_bytes >
             prepared_estimate.prepared_retained_bytes &&
         larger_target_estimate.joint_incremental_peak_bytes >
             prepared_estimate.joint_incremental_peak_bytes &&
         larger_target_estimate.post_consume_lease_retained_bytes >
             prepared_estimate.post_consume_lease_retained_bytes);
  const auto revalidations_before_prepared_view =
      resolver.revalidation_count;
  const auto snapshots_before_prepared_view =
      resolver.comparison_snapshot_count;
  const auto prepared_values =
      api::ViewPreparedContextualTextLiteralSetV2(
          prepared_winner.prepared);
  assert(prepared_values.size() == 1);
  const auto& prepared_entry = prepared_values.front();
  const auto& prepared_runtime = prepared_entry.runtime_materialization;
  assert(prepared_entry.prepared_value.literal_occurrence == 1 &&
         prepared_entry.prepared_value.node_id == 101 &&
         prepared_entry.prepared_value.literal_descriptor_handle == 9 &&
         prepared_entry.prepared_value.canonical_body == Bytes{'x'} &&
         prepared_entry.comparison_occurrence == 21 &&
         prepared_entry.target_descriptor_handle == 3 &&
         prepared_entry.literal_argument_ordinal == 1 &&
         prepared_entry.target_argument_ordinal == 2 &&
         prepared_runtime.graph_binding.literal_expression_id == 20 &&
         prepared_runtime.graph_binding.comparison_expression_id == 21 &&
         prepared_runtime.graph_binding.target_expression_id == 22 &&
         prepared_runtime.graph_binding.source_node_id == 10 &&
         prepared_runtime.graph_binding.literal_descriptor_handle == 9 &&
         prepared_runtime.graph_binding.target_descriptor_handle == 3 &&
         prepared_runtime.value.encoded_value == "x" &&
         prepared_runtime.value.binary_value.empty() &&
         prepared_runtime.exact_profile.canonical_body == Bytes{'x'} &&
         prepared_runtime.graph_binding
                 .exact_relational_descriptor_v2_fields[7] == "0" &&
         prepared_runtime.graph_binding
                 .exact_target_relational_descriptor_v2_fields[7] == "1" &&
         prepared_runtime.target_descriptor.canonical_type_name == "text" &&
         prepared_runtime.comparison_resources.text_seed.active &&
         prepared_runtime.comparison_resources.text_seed.seed_pack_name ==
             "test.seed");
  const auto prepared_equality_uuid =
      prepared_entry.equality_operation_uuid;
  const auto prepared_equality_generation =
      prepared_entry.equality_operation_generation;
  const auto* exact_prepared_entry =
      api::FindPreparedContextualTextExecutionEntryV2(
          prepared_winner.prepared, 1, 101, 20, 21, 22, 10, 9, 3, 1, 2,
          prepared_equality_uuid, prepared_equality_generation);
  assert(exact_prepared_entry == &prepared_entry);
  assert(api::FindPreparedContextualTextExecutionEntryV2(
             prepared_winner.prepared, 1, 101, 20, 21, 22, 10, 9, 4, 1, 2,
             prepared_equality_uuid, prepared_equality_generation) ==
         nullptr);
  assert(api::CopyContextualTextLiteralAuthoritySnapshotV2(
      issued.authority, &snapshot, &diagnostic));
  assert(snapshot.state ==
             api::EngineContextualTextLiteralAuthorityStateV2::issued &&
         !literal_admission_consumed && prepared_winner.prepared.valid() &&
         resolver.revalidation_count == revalidations_before_prepared_view &&
         resolver.comparison_snapshot_count == snapshots_before_prepared_view);
  api::EngineContextualTextLiteralJointConsumeRequestV2 consume;
  consume.context = &context;
  consume.authority = &issued.authority;
  consume.prepared = &prepared_winner.prepared;
  consume.exact_sbel_v1 = &sbel;
  consume.composed_transfer = &transferred.record;
  consume.receipt_literal_admission_consumed =
      &literal_admission_consumed;

  // Comparison resources are part of the state-neutral staged value. A live
  // seed/resource mutation before the joint barrier refuses the attempt and
  // leaves both pair members reusable.
  resolver.resource_mutation = true;
  const auto snapshots_before_mutation = resolver.comparison_snapshot_count;
  const auto resource_mutated_at_cas =
      api::JointConsumeContextualTextLiteralAuthorityV2(consume);
  assert(!resource_mutated_at_cas.ok &&
         resource_mutated_at_cas.diagnostic.code ==
             "CTB.TEXT.RESOURCE_EPOCH_MISMATCH" &&
         !literal_admission_consumed && prepared_winner.prepared.valid() &&
         resolver.comparison_snapshot_count == snapshots_before_mutation + 1);
  resolver.resource_mutation = false;

  // Prepare was valid, but the retained target becomes stale before the
  // physical-route consume barrier.  The final resolver check must refuse
  // without consuming either member, and the same staged attempt remains
  // usable after authority is current again.
  resolver.stale = true;
  const auto revalidations_before_joint = resolver.revalidation_count;
  const auto stale_at_cas =
      api::JointConsumeContextualTextLiteralAuthorityV2(consume);
  assert(!stale_at_cas.ok &&
         stale_at_cas.diagnostic.code ==
             "CTB.TEXT.RESOURCE_EPOCH_MISMATCH" &&
         !literal_admission_consumed && prepared_winner.prepared.valid() &&
         resolver.revalidation_count == revalidations_before_joint + 1);
  assert(api::CopyContextualTextLiteralAuthoritySnapshotV2(
      issued.authority, &snapshot, &diagnostic));
  assert(snapshot.state ==
         api::EngineContextualTextLiteralAuthorityStateV2::issued);
  resolver.stale = false;
  const auto revalidations_before_success = resolver.revalidation_count;
  auto consumed = api::JointConsumeContextualTextLiteralAuthorityV2(consume);
  assert(consumed.ok && consumed.lease.valid() &&
         literal_admission_consumed &&
         resolver.revalidation_count == revalidations_before_success + 1);
  const auto lease_estimate =
      api::EstimateContextualTextExecutionAuthorityLeaseRetainedBytesV2(
          consumed.lease);
  assert(lease_estimate.ok &&
         lease_estimate.status ==
             api::EngineContextualTextResourceEstimateStatusV2::ok &&
         lease_estimate.post_consume_lease_retained_bytes ==
             prepared_estimate.post_consume_lease_retained_bytes);
  assert(!prepared_winner.prepared.valid() &&
         api::ViewPreparedContextualTextLiteralSetV2(
             prepared_winner.prepared).empty() &&
         !api::EstimatePreparedContextualTextLiteralResourcesV2(
              prepared_winner.prepared)
              .ok &&
         api::FindPreparedContextualTextExecutionEntryV2(
             prepared_winner.prepared, 1, 101, 20, 21, 22, 10, 9, 3, 1, 2,
             prepared_equality_uuid, prepared_equality_generation) ==
             nullptr);
  const auto values =
      api::ViewContextualTextExecutionAuthorityLeaseV2(consumed.lease);
  assert(values.size() == 1 &&
         values.front().typed_value.canonical_body == Bytes{'x'} &&
         values.front().runtime_materialization.graph_binding
                 .literal_occurrence == 1 &&
         values.front().runtime_materialization.graph_binding.node_id == 101 &&
         values.front().runtime_materialization.graph_binding
                 .literal_expression_id == 20 &&
         values.front().runtime_materialization.graph_binding
                 .comparison_expression_id == 21 &&
         values.front().runtime_materialization.graph_binding
                 .target_expression_id == 22 &&
         values.front().runtime_materialization.graph_binding.source_node_id ==
             10 &&
         values.front().runtime_materialization.graph_binding
                 .literal_descriptor_handle == 9 &&
         values.front().runtime_materialization.graph_binding
                 .target_descriptor_handle == 3);
  const auto& runtime = values.front().runtime_materialization;
  api::EnginePublicRelationProjectionV3 retained_projection;
  assert(api::DecodeEnginePublicRelationProjectionV3(
      resolver.exact_projection(), &retained_projection, &diagnostic));
  assert(retained_projection.columns.size() == 1);
  const auto& retained_column = retained_projection.columns.front();
  const auto exact_literal_relational = JoinRelationalDescriptorFields(
      RelationalDescriptorFields(runtime.exact_profile, "0"));
  const auto exact_target_relational = JoinRelationalDescriptorFields(
      RelationalDescriptorFields(runtime.exact_profile, "1"));
  assert(runtime.value.descriptor.descriptor_uuid.canonical ==
             "019d0000-0000-7000-8000-00000000d718" &&
         runtime.value.descriptor.descriptor_kind == "scalar" &&
         runtime.value.descriptor.canonical_type_name == "text" &&
         runtime.value.encoded_value == "x" &&
         runtime.value.binary_value.empty() &&
         !runtime.value.is_null &&
         runtime.value.state == api::EngineValueState::value &&
         runtime.target_descriptor.descriptor_uuid.canonical ==
             UuidText(retained_column.descriptor_uuid) &&
         retained_column.descriptor_kind == "canonical_type_descriptor" &&
         runtime.target_descriptor.descriptor_kind == "scalar" &&
         runtime.target_descriptor.canonical_type_name ==
             retained_column.canonical_type_name &&
         runtime.target_descriptor.encoded_descriptor ==
             retained_column.encoded_type_descriptor &&
         runtime.exact_literal_relational_descriptor_v2_bytes ==
             Bytes(exact_literal_relational.begin(),
                   exact_literal_relational.end()) &&
         runtime.exact_target_relational_descriptor_v2_bytes ==
             Bytes(exact_target_relational.begin(),
                   exact_target_relational.end()) &&
         runtime.exact_literal_relational_descriptor_v2_bytes !=
             runtime.exact_target_relational_descriptor_v2_bytes &&
         runtime.exact_profile.exact_bytes ==
             issued.profile_set.mappings.front().profile.exact_bytes &&
         runtime.comparison_resources.exact_public_relation_projection_v3 ==
             resolver.exact_projection() &&
         runtime.comparison_resources.exact_sbtltd02 ==
             resolver.exact_descriptor() &&
         runtime.comparison_resources.collation_uuid_canonical ==
             UuidText(runtime.exact_profile.collation_uuid) &&
         runtime.comparison_resources.text_seed.active &&
         runtime.comparison_resources.text_seed.seed_pack_name == "test.seed");
  const auto* exact_entry =
      api::FindContextualTextExecutionAuthorityEntryV2(
          consumed.lease, 20, 21, 22, 10, 9, 3,
          values.front().equality_operation_uuid,
          values.front().equality_operation_generation);
  assert(exact_entry == &values.front());
  assert(api::FindContextualTextExecutionAuthorityEntryV2(
             consumed.lease, 20, 21, 22, 10, 9, 4,
             values.front().equality_operation_uuid,
             values.front().equality_operation_generation) == nullptr);

  consume.prepared = &prepared_loser.prepared;
  assert(!api::JointConsumeContextualTextLiteralAuthorityV2(consume).ok);

  auto missing_graph =
      api::CreateEngineContextualTextGraphAuthorityVerifierForReceiptV2(
          context);
  assert(missing_graph != nullptr);
  sblr::ContextualTextLiteralExecuteV2 empty_execute;
  std::vector<api::EngineContextualTextVerifiedGraphBindingV2>
      missing_bindings;
  assert(!missing_graph->VerifyPrepareEvidence(
      context, decoded_request, empty_execute, {}, {}, transferred.record, {},
      0, {}, &missing_bindings, &diagnostic));

  auto missing_target =
      api::CreateEngineContextualTextTargetAuthorityResolverForReceiptV2(
          context);
  assert(missing_target != nullptr);
  api::EngineResolvedContextualTextTargetV2 target;
  assert(!missing_target->ResolveTarget(
      context, decoded_request.demands.front(), &target, &diagnostic));

  auto stale_context = Context(2);
  FakeTargetResolver stale_resolver(stale_context);
  sblr::ContextualTextLiteralNegotiationRequestV2 stale_request;
  auto stale_issued = Issue(stale_context, &stale_resolver, &stale_request);
  assert(stale_issued.ok);
  const auto stale_sbxn = ContextualSbxn(stale_issued.profile_set);
  const auto installed = api::LoadSblrExecutorAvailabilitySnapshot(
      stale_context, AvailabilityIdentity());
  assert(installed.ok && installed.snapshot.installed);
  api::SblrExecutorAvailabilitySetRequest revoke;
  revoke.database_uuid = stale_context.database_uuid.canonical;
  revoke.expected_snapshot_uuid = installed.snapshot.snapshot_uuid;
  revoke.expected_generation = installed.snapshot.generation;
  revoke.exact_row_identity = AvailabilityIdentity();
  revoke.requested_state = api::SblrExecutorAvailabilityState::revoked;
  revoke.reason_code = "contextual.test.revoke";
  assert(api::SetSblrExecutorAvailability(stale_context, revoke).ok);
  const auto revoked_transfer =
      Transfer(stale_context, &stale_issued.authority, stale_sbxn, 2);
  assert(!revoked_transfer.ok &&
         revoked_transfer.diagnostic.code ==
             "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING" &&
         revoked_transfer.diagnostic.message_key ==
             "sblr.opcode.executor_evidence_missing");

  (void)api::RevokeContextualTextLiteralAuthorityV2(
      &stale_issued.authority, "test_cleanup");
  (void)api::RevokeContextualTextLiteralAuthorityV2(
      &issued.authority, "test_cleanup");
  RemoveAvailabilityStore(stale_context);
  RemoveAvailabilityStore(context);
  return 0;
}
