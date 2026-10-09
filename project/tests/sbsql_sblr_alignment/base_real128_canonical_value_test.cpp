// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "admitted_datatype_cohort.hpp"
#include "datatype_binary.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_exchange.hpp"
#include "datatype_operations.hpp"
#include "datatype_physical_encoding.hpp"
#include "datatype_storage_identity.hpp"
#include "disk_device.hpp"
#include "runtime_platform.hpp"
#include "sbl_numeric.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace dt = scratchbird::core::datatypes;
namespace numeric = scratchbird::libraries::sbl_numeric;
namespace disk = scratchbird::storage::disk;
namespace platform = scratchbird::core::platform;
namespace fs = std::filesystem;

namespace {

unsigned checks = 0;
unsigned failures = 0;

void Check(bool condition, const std::string& reason) {
  ++checks;
  if (!condition) {
    ++failures;
    if (failures <= 40) std::cerr << "FAIL: " << reason << '\n';
  }
}

template <typename LeftUuid, typename RightUuid>
bool SameUuidBytes(const LeftUuid& left, const RightUuid& right) {
  return std::equal(std::begin(left.bytes), std::end(left.bytes),
                    std::begin(right.bytes), std::end(right.bytes));
}

std::string DiagnosticDetail(const platform::DiagnosticRecord& diagnostic) {
  for (const auto& argument : diagnostic.arguments) {
    if (argument.key == "detail") {
      const auto* value = argument.text();
      return value == nullptr ? std::string{} : *value;
    }
  }
  return {};
}

scratchbird::engine::Uuid FixtureV7Uuid(std::uint8_t seed) {
  scratchbird::engine::Uuid uuid{};
  for (std::size_t index = 0; index < 16; ++index) {
    uuid.bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  uuid.bytes[6] = static_cast<std::uint8_t>((uuid.bytes[6] & 0x0fu) | 0x70u);
  uuid.bytes[8] = static_cast<std::uint8_t>((uuid.bytes[8] & 0x3fu) | 0x80u);
  return uuid;
}

scratchbird::engine::ExecutionTypeDescriptor DescriptorFor(
    dt::CanonicalTypeId type_id) {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return {};
  const auto row = dt::LookupDatatypeCatalogRow(manifest.manifest, type_id);
  if (!row.ok() || row.manifest.descriptor_rows.size() != 1) return {};
  dt::CatalogExecutionTypeMetadata metadata;
  metadata.descriptor_uuid = row.manifest.descriptor_rows.front().descriptor_uuid;
  metadata.descriptor_epoch =
      row.manifest.descriptor_rows.front().descriptor_epoch;
  const auto result =
      dt::LookupExecutionTypeDescriptorFromCatalog(type_id, metadata);
  return result.ok() ? result.descriptor
                     : scratchbird::engine::ExecutionTypeDescriptor{};
}

const scratchbird::engine::ExecutionTypeDescriptor& RealDescriptor() {
  static const auto descriptor = DescriptorFor(dt::CanonicalTypeId::real128);
  return descriptor;
}

std::string Bytes(const numeric::Real128Bytes& bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::vector<platform::byte> Payload(std::string_view bytes) {
  return {bytes.begin(), bytes.end()};
}

std::vector<platform::byte> UuidPayload(const platform::Uuid& uuid) {
  return {uuid.bytes.begin(), uuid.bytes.end()};
}

numeric::NumericContext BackendContext(
    dt::DatatypeRoundingMode rounding = dt::DatatypeRoundingMode::half_even,
    bool special = false) {
  numeric::NumericContext context;
  context.allow_special_values = special;
  switch (rounding) {
    case dt::DatatypeRoundingMode::half_even:
      context.rounding = numeric::RoundingMode::half_even;
      break;
    case dt::DatatypeRoundingMode::half_up:
      context.rounding = numeric::RoundingMode::half_up;
      break;
    case dt::DatatypeRoundingMode::truncate:
      context.rounding = numeric::RoundingMode::truncate;
      break;
    default:
      context.rounding = static_cast<numeric::RoundingMode>(99);
      break;
  }
  return context;
}

std::string EncodeText(
    std::string_view text,
    dt::DatatypeRoundingMode rounding = dt::DatatypeRoundingMode::half_even,
    bool special = false) {
  const auto encoded =
      numeric::EncodeReal128LittleEndian(text, BackendContext(rounding, special));
  Check(encoded.numeric.status == numeric::NumericStatusCode::ok &&
            encoded.bytes.has_value(),
        "test fixture text encodes through the public real128 boundary");
  return encoded.bytes ? Bytes(*encoded.bytes) : std::string{};
}

dt::DatatypeOperationValue PresentBytes(std::string bytes) {
  dt::DatatypeOperationValue value{
      dt::CanonicalTypeId::real128, std::move(bytes), false};
  value.descriptor = RealDescriptor();
  return value;
}

dt::DatatypeOperationValue PresentText(std::string_view text,
                                       bool special = false) {
  return PresentBytes(EncodeText(text, dt::DatatypeRoundingMode::half_even,
                                 special));
}

dt::DatatypeOperationValue TypedNull() {
  dt::DatatypeOperationValue value{dt::CanonicalTypeId::real128, {}, true};
  value.descriptor = RealDescriptor();
  return value;
}

numeric::Real128Bytes Pattern(std::initializer_list<std::pair<std::size_t,
                                                              std::uint8_t>> set) {
  numeric::Real128Bytes bytes{};
  for (const auto& [offset, value] : set) bytes[offset] = value;
  return bytes;
}

const numeric::Real128Bytes kPositiveZero{};
const numeric::Real128Bytes kNegativeZero = Pattern({{15, 0x80}});
const numeric::Real128Bytes kMinSubnormal = Pattern({{0, 0x01}});
const numeric::Real128Bytes kOne = Pattern({{14, 0xff}, {15, 0x3f}});
const numeric::Real128Bytes kOnePointFive =
    Pattern({{13, 0x80}, {14, 0xff}, {15, 0x3f}});
const numeric::Real128Bytes kTwo = Pattern({{15, 0x40}});
const numeric::Real128Bytes kThree =
    Pattern({{13, 0x80}, {15, 0x40}});
const numeric::Real128Bytes kPositiveInfinity =
    Pattern({{14, 0xff}, {15, 0x7f}});
const numeric::Real128Bytes kNegativeInfinity =
    Pattern({{14, 0xff}, {15, 0xff}});
const numeric::Real128Bytes kQuietNan =
    Pattern({{13, 0x80}, {14, 0xff}, {15, 0x7f}});
const numeric::Real128Bytes kSignalingNan =
    Pattern({{0, 0x01}, {14, 0xff}, {15, 0x7f}});

numeric::Real128Bytes MaximumFinite() {
  numeric::Real128Bytes value{};
  std::fill(value.begin(), value.begin() + 14, 0xff);
  value[14] = 0xfe;
  value[15] = 0x7f;
  return value;
}

bool SameFacts(const dt::DatatypeNumericFacts& left,
               const numeric::NumericResult& right) {
  return left.inexact == right.inexact &&
      left.underflow == right.underflow &&
      left.overflow == right.overflow && left.invalid == right.invalid &&
      left.divide_by_zero == right.divide_by_zero &&
      left.subnormal == right.subnormal &&
      left.unordered ==
          (right.status == numeric::NumericStatusCode::unordered);
}

constexpr platform::Uuid kDescriptorUuid{{
    0x8e,0,0,0,0x72,0x65,0x71,0x6c,0xb1,0x32,0x38,0,0,0,0,0}};
constexpr platform::Uuid kTypeUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x12}};
constexpr platform::Uuid kCodecUuid{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x13}};
constexpr std::string_view kCodecId = "datatype.real128.ieee754.le.v1";

void ExactIdentityAndVectors() {
  const auto descriptor = RealDescriptor();
  Check(SameUuidBytes(descriptor.descriptor_uuid, kDescriptorUuid) &&
            descriptor.descriptor_epoch == 1 &&
            descriptor.canonical_type_id ==
                static_cast<std::uint32_t>(dt::CanonicalTypeId::real128) &&
            descriptor.bit_width == 128 && descriptor.precision == 113,
        "real128 has the exact UUID-bound descriptor identity");

  for (const auto cohort : std::array{
           std::tuple{dt::kDatatypeCohortV4, 4ULL, 4ULL},
           std::tuple{dt::kDatatypeCohortV5, 5ULL, 5ULL}}) {
    const auto admitted = dt::LookupDatatypeTypeCodecIdentityV1(
        std::get<0>(cohort), std::get<1>(cohort), std::get<2>(cohort),
        kDescriptorUuid, 1);
    Check(admitted.ok && admitted.row.type_uuid == kTypeUuid &&
              admitted.row.codec_uuid == kCodecUuid &&
              admitted.row.codec_id == kCodecId &&
              admitted.row.descriptor_generation == 1 &&
              admitted.row.type_generation == 1 &&
              admitted.row.codec_generation == 1 &&
              admitted.row.codec_version == 1 &&
              admitted.row.canonical_value_minimum_bytes == 16 &&
              admitted.row.canonical_value_maximum_bytes == 16 &&
              admitted.row.canonical_value_exact_bytes == 16 &&
              admitted.row.canonical_byte_order == "little_endian" &&
              admitted.row.canonical_representation == "IEEE754_binary128",
          "real128 exact identity is present in V4 and inherited by V5");
  }
  Check(!dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV1, 1, 1, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV2, 2, 2, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV3, 3, 3, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 4, 5, kDescriptorUuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, kTypeUuid, 1).ok,
        "real128 is absent from V1-V3 and crossed identities refuse");

  dt::DatatypeStorageIdentityV1 storage;
  const auto layout =
      dt::LookupDatatypeStorageLayout(dt::CanonicalTypeId::real128);
  Check(dt::LookupDatatypeStorageIdentityV1(
            dt::kDatatypeCohortV5, 5, 5, kDescriptorUuid, 1, &storage) &&
            storage.type_uuid == kTypeUuid && storage.codec.has_value() &&
            storage.codec->codec_uuid == kCodecUuid &&
            storage.codec->codec_id == kCodecId && layout.ok() &&
            layout.layout.storage_class ==
                dt::DatatypeStorageClass::inline_fixed &&
            layout.layout.encoding ==
                dt::DatatypeBinaryEncoding::ieee754_binary128_little_endian &&
            layout.layout.inline_bytes == 16,
        "real128 storage identity and layout retain exact LE16");

  struct Vector {
    const char* text;
    numeric::Real128Bytes bytes;
    bool special;
  };
  for (const auto& vector : std::array{
           Vector{"0", kPositiveZero, false},
           Vector{"-0", kNegativeZero, false},
           Vector{"0x1p-16494", kMinSubnormal, false},
           Vector{"1", kOne, false},
           Vector{"1.5", kOnePointFive, false},
           Vector{"2", kTwo, false},
           Vector{"3", kThree, false},
           Vector{"Infinity", kPositiveInfinity, true},
           Vector{"-Infinity", kNegativeInfinity, true},
           Vector{"NaN", kQuietNan, true},
           Vector{"sNaN", kSignalingNan, true}}) {
    const auto encoded = numeric::EncodeReal128LittleEndian(
        vector.text, BackendContext(dt::DatatypeRoundingMode::half_even,
                                    vector.special));
    const auto decoded = numeric::DecodeReal128LittleEndian(
        vector.bytes.data(), vector.bytes.size(),
        BackendContext(dt::DatatypeRoundingMode::half_even, vector.special),
        true);
    Check(encoded.numeric.status == numeric::NumericStatusCode::ok &&
              encoded.bytes && *encoded.bytes == vector.bytes &&
              decoded.numeric.status == numeric::NumericStatusCode::ok &&
              decoded.bytes && *decoded.bytes == vector.bytes,
          std::string("hard-coded independent binary128 vector: ") +
              vector.text);
  }
}

void LowerCodecs() {
  for (const auto& bytes : {kPositiveZero, kNegativeZero, kMinSubnormal, kOne,
                            kPositiveInfinity, kQuietNan, kSignalingNan}) {
    const auto value = Bytes(bytes);
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::real128, false, false, Payload(value)});
    const auto binary_back = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded)
        : dt::DatatypeBinaryResult{};
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::real128,
         dt::DatatypePhysicalValueState::value, Payload(value)});
    const auto physical_back = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(),
                                          physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};
    Check(binary.ok() && binary_back.ok() &&
              binary_back.value.payload == Payload(value) && physical.ok() &&
              physical_back.ok() &&
              physical_back.value.type_id == dt::CanonicalTypeId::real128 &&
              physical_back.value.state ==
                  dt::DatatypePhysicalValueState::value &&
              physical_back.value.payload == Payload(value),
          "lower binary and physical codecs preserve exact real128 LE16 bits");
  }
  for (const auto width : {0u, 1u, 2u, 8u, 15u, 17u, 32u}) {
    const std::string malformed(width, static_cast<char>(0x5a));
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::real128, false, false, Payload(malformed)});
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::real128,
         dt::DatatypePhysicalValueState::value, Payload(malformed)});
    Check(!binary.ok() && binary.encoded.empty() && !physical.ok() &&
              physical.bytes.empty(),
          "lower codecs reject malformed real128 width atomically");
  }
  const auto binary_null = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::real128, true, false, {}});
  const auto physical_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::real128,
       dt::DatatypePhysicalValueState::sql_null, {}});
  Check(binary_null.ok() && physical_null.ok() &&
            !dt::EncodeDatatypeBinaryValue(
                 {dt::CanonicalTypeId::real128, true, false, {0}}).ok() &&
            !dt::EncodeDatatypePhysicalValue(
                 {dt::CanonicalTypeId::real128,
                  dt::DatatypePhysicalValueState::sql_null, {0}}).ok(),
        "lower codecs preserve clean NULL and reject dirty NULL");
}

void DescriptorEnvelope() {
  dt::DatatypeDescriptorEnvelope envelope;
  envelope.kind = dt::DatatypeDescriptorEnvelopeKind::datatype_transport;
  envelope.integrity_profile = dt::DatatypeDescriptorIntegrityProfile::strong;
  envelope.records = {{"descriptor_uuid", UuidPayload(kDescriptorUuid)},
                      {"type_uuid", UuidPayload(kTypeUuid)},
                      {"codec_uuid", UuidPayload(kCodecUuid)},
                      {"codec_id", Payload(kCodecId)}};
  const auto encoded = dt::EncodeDatatypeDescriptorEnvelope(envelope);
  const auto decoded = encoded.ok()
      ? dt::DecodeDatatypeDescriptorEnvelope(encoded.encoded)
      : dt::DatatypeDescriptorEnvelopeResult{};
  Check(encoded.ok() && decoded.ok() &&
            decoded.envelope.records.size() == envelope.records.size() &&
            decoded.envelope.records[0].payload == UuidPayload(kDescriptorUuid) &&
            decoded.envelope.records[1].payload == UuidPayload(kTypeUuid) &&
            decoded.envelope.records[2].payload == UuidPayload(kCodecUuid),
        "descriptor envelope preserves exact real128 identities");
  if (encoded.ok()) {
    auto corrupt = encoded.encoded;
    corrupt.back() ^= 1u;
    auto truncated = encoded.encoded;
    truncated.pop_back();
    Check(!dt::DecodeDatatypeDescriptorEnvelope(corrupt).ok() &&
              !dt::DecodeDatatypeDescriptorEnvelope(truncated).ok(),
          "corrupt or truncated real128 descriptor envelopes refuse");
  }
}

void IdentityAndNulls() {
  const auto present = PresentBytes(Bytes(kOne));
  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    dt::DatatypeCastRequest identity;
    identity.value = present;
    identity.value.descriptor.stable_name = "binary128-source-label";
    identity.target_type_id = dt::CanonicalTypeId::real128;
    identity.target_descriptor = RealDescriptor();
    identity.target_descriptor.stable_name = "binary128-target-label";
    identity.context = context;
    identity.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    identity.numeric_context.allow_special_values = false;
    const auto result = dt::CastDatatypeValue(identity);
    Check(result.ok() &&
              result.category == dt::DatatypeCastCategory::identity &&
              result.value.encoded_value == Bytes(kOne) &&
              SameUuidBytes(result.value.descriptor.descriptor_uuid,
                            kDescriptorUuid),
          "exact-equal-descriptor PRESENT identity byte-preserves LE16 and ignores labels");

    dt::DatatypeCastRequest contextual;
    contextual.value = {dt::CanonicalTypeId::null_type, {}, true};
    contextual.target_type_id = dt::CanonicalTypeId::real128;
    contextual.target_descriptor = RealDescriptor();
    contextual.context = context;
    contextual.explicit_cast =
        context == dt::DatatypeCastContext::explicit_cast;
    const auto bound = dt::CastDatatypeValue(contextual);
    Check(bound.ok() && bound.value.type_id == dt::CanonicalTypeId::real128 &&
              bound.value.is_null && bound.value.encoded_value.empty() &&
              SameUuidBytes(bound.value.descriptor.descriptor_uuid,
                            kDescriptorUuid),
          "contextual NULL binds exact real128 descriptor");

    dt::DatatypeCastRequest typed_identity;
    typed_identity.value = TypedNull();
    typed_identity.target_type_id = dt::CanonicalTypeId::real128;
    typed_identity.target_descriptor = RealDescriptor();
    typed_identity.context = context;
    typed_identity.explicit_cast =
        context == dt::DatatypeCastContext::explicit_cast;
    const auto typed = dt::CastDatatypeValue(typed_identity);
    Check(typed.ok() && typed.category == dt::DatatypeCastCategory::identity &&
              typed.value.is_null && typed.value.encoded_value.empty(),
          "exact same-descriptor typed real128 NULL identity succeeds");
  }

  dt::DatatypeCastRequest special_identity;
  special_identity.value = PresentBytes(Bytes(kQuietNan));
  special_identity.target_type_id = dt::CanonicalTypeId::real128;
  special_identity.target_descriptor = RealDescriptor();
  const auto denied_special = dt::CastDatatypeValue(special_identity);
  special_identity.numeric_context.allow_special_values = true;
  const auto admitted_special = dt::CastDatatypeValue(special_identity);
  Check(!denied_special.ok() &&
            denied_special.category == dt::DatatypeCastCategory::identity &&
            denied_special.numeric_facts.invalid &&
            denied_special.diagnostic.diagnostic_code ==
                "NUMERIC.REAL128.INVALID" &&
            denied_special.value.type_id == dt::CanonicalTypeId::unknown &&
            admitted_special.ok() &&
            admitted_special.value.encoded_value == Bytes(kQuietNan),
        "PRESENT identity validates special-value admission before byte preservation");

  for (const auto state : {0, 1, 2}) {
    dt::DatatypeCastRequest invalid_context;
    if (state == 0) {
      invalid_context.value = PresentBytes(Bytes(kOne));
    } else if (state == 1) {
      invalid_context.value = {dt::CanonicalTypeId::null_type, {}, true};
    } else {
      invalid_context.value = TypedNull();
    }
    invalid_context.target_type_id = dt::CanonicalTypeId::real128;
    invalid_context.target_descriptor = RealDescriptor();
    invalid_context.numeric_context.rounding =
        static_cast<dt::DatatypeRoundingMode>(99);
    const auto result = dt::CastDatatypeValue(invalid_context);
    Check(!result.ok() && result.numeric_facts.invalid &&
              result.diagnostic.diagnostic_code ==
                  "NUMERIC.REAL128.INVALID" &&
              result.value.type_id == dt::CanonicalTypeId::unknown,
          "unknown numeric context rejects PRESENT and NULL real128 identities");
  }

  auto policy_descriptor = RealDescriptor();
  policy_descriptor.security_policy_uuid = FixtureV7Uuid(0xc0);
  policy_descriptor.modifier_flags |=
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          scratchbird::engine::ExecutionTypeModifierFlag::security_policy_uuid);
  for (const auto source_descriptor :
       {scratchbird::engine::ExecutionTypeDescriptor{}, policy_descriptor}) {
    auto value = present;
    value.descriptor = source_descriptor;
    dt::DatatypeCastRequest identity;
    identity.value = value;
    identity.target_type_id = dt::CanonicalTypeId::real128;
    identity.target_descriptor = RealDescriptor();
    const auto result = dt::CastDatatypeValue(identity);
    Check(!result.ok() &&
              result.diagnostic.diagnostic_code == "DATATYPE.DESCRIPTOR.INVALID" &&
              result.value.type_id == dt::CanonicalTypeId::unknown &&
              result.value.encoded_value.empty(),
          "missing or non-exact PRESENT descriptor refuses before semantics");
  }
  auto label_only = present;
  label_only.descriptor = {};
  label_only.descriptor.stable_name = "real128";
  label_only.encoded_value.assign(15, '\0');
  dt::DatatypeCastRequest invalid_identity;
  invalid_identity.value = label_only;
  invalid_identity.target_type_id = dt::CanonicalTypeId::real128;
  invalid_identity.target_descriptor = RealDescriptor();
  const auto label_result = dt::CastDatatypeValue(invalid_identity);
  Check(!label_result.ok() &&
            label_result.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID",
        "label-only descriptor wins over malformed carrier");

  auto dirty_null = TypedNull();
  dirty_null.encoded_value = Bytes(kPositiveZero);
  invalid_identity.value = dirty_null;
  const auto dirty = dt::CastDatatypeValue(invalid_identity);
  Check(!dirty.ok() &&
            dirty.diagnostic.diagnostic_code == "DATATYPE.NULL_STATE.INVALID" &&
            dirty.value.encoded_value.empty(),
        "dirty typed real128 NULL refuses atomically");

  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    for (const bool compatibility : {false, true}) {
      dt::DatatypeCastRequest cross_in;
      cross_in.value = {dt::CanonicalTypeId::int128, {}, true};
      cross_in.value.descriptor = DescriptorFor(dt::CanonicalTypeId::int128);
      cross_in.target_type_id = dt::CanonicalTypeId::real128;
      cross_in.target_descriptor = RealDescriptor();
      cross_in.context = context;
      cross_in.explicit_cast =
          context == dt::DatatypeCastContext::explicit_cast;
      cross_in.reference_compatibility_profile = compatibility;
      dt::DatatypeCastRequest cross_out;
      cross_out.value = TypedNull();
      cross_out.target_type_id = dt::CanonicalTypeId::uint128;
      cross_out.target_descriptor =
          DescriptorFor(dt::CanonicalTypeId::uint128);
      cross_out.context = context;
      cross_out.explicit_cast =
          context == dt::DatatypeCastContext::explicit_cast;
      cross_out.reference_compatibility_profile = compatibility;
      const auto incoming = dt::CastDatatypeValue(cross_in);
      const auto outgoing = dt::CastDatatypeValue(cross_out);
      Check(!incoming.ok() && !outgoing.ok() &&
                incoming.value.type_id == dt::CanonicalTypeId::unknown &&
                outgoing.value.type_id == dt::CanonicalTypeId::unknown,
            "cross-type typed NULL casts remain blocked in every context/profile");
    }
  }
}

dt::DatatypeNumericOperationRequest NumericRequest(
    dt::DatatypeNumericOperationKind operation,
    const numeric::Real128Bytes& left,
    const numeric::Real128Bytes& right = kPositiveZero) {
  dt::DatatypeNumericOperationRequest request;
  request.operation = operation;
  request.type_id = dt::CanonicalTypeId::real128;
  request.left = PresentBytes(Bytes(left));
  request.right = PresentBytes(Bytes(right));
  request.result_descriptor = RealDescriptor();
  return request;
}

numeric::Real128BinaryResult BinaryOracle(
    dt::DatatypeNumericOperationKind operation,
    const numeric::Real128Bytes& left,
    const numeric::Real128Bytes& right,
    const dt::DatatypeNumericContext& context) {
  numeric::Real128BinaryRequest request;
  request.operation = static_cast<numeric::NumericOperation>(operation);
  request.left = left;
  if (operation != dt::DatatypeNumericOperationKind::canonicalize) {
    request.right = right;
  }
  request.context = BackendContext(context.rounding,
                                   context.allow_special_values);
  return numeric::ApplyReal128BinaryOperation(request);
}

void NumericOperationsAndComparison() {
  struct ExactCase {
    dt::DatatypeNumericOperationKind operation;
    numeric::Real128Bytes left;
    numeric::Real128Bytes right;
    numeric::Real128Bytes expected;
  };
  for (const auto& entry : std::array{
           ExactCase{dt::DatatypeNumericOperationKind::canonicalize, kOne,
                     kPositiveZero, kOne},
           ExactCase{dt::DatatypeNumericOperationKind::add, kOne, kTwo,
                     kThree},
           ExactCase{dt::DatatypeNumericOperationKind::subtract, kThree, kOne,
                     kTwo},
           ExactCase{dt::DatatypeNumericOperationKind::multiply, kOnePointFive,
                     kTwo, kThree},
           ExactCase{dt::DatatypeNumericOperationKind::divide, kThree, kTwo,
                     kOnePointFive}}) {
    const auto request = NumericRequest(entry.operation, entry.left, entry.right);
    const auto result = dt::ApplyNumericOperation(request);
    const auto oracle =
        BinaryOracle(entry.operation, entry.left, entry.right, request.context);
    Check(result.ok() && result.value.type_id == dt::CanonicalTypeId::real128 &&
              result.value.encoded_value == Bytes(entry.expected) &&
              result.value.encoded_value.size() == 16 &&
              SameUuidBytes(result.value.descriptor.descriptor_uuid,
                            kDescriptorUuid) &&
              oracle.bytes && result.value.encoded_value == Bytes(*oracle.bytes) &&
              SameFacts(result.numeric_facts, oracle.numeric),
          "request-scoped real128 operation publishes exact LE16, descriptor and facts");
  }

  auto result_descriptor_request = NumericRequest(
      dt::DatatypeNumericOperationKind::add, kOne, kTwo);
  result_descriptor_request.result_descriptor = {};
  const auto missing_result_descriptor =
      dt::ApplyNumericOperation(result_descriptor_request);
  result_descriptor_request.result_descriptor =
      DescriptorFor(dt::CanonicalTypeId::boolean);
  const auto wrong_result_descriptor =
      dt::ApplyNumericOperation(result_descriptor_request);
  result_descriptor_request.result_descriptor = RealDescriptor();
  result_descriptor_request.result_descriptor.security_policy_uuid =
      FixtureV7Uuid(0xb0);
  result_descriptor_request.result_descriptor.modifier_flags |=
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          scratchbird::engine::ExecutionTypeModifierFlag::security_policy_uuid);
  const auto non_exact_result_descriptor =
      dt::ApplyNumericOperation(result_descriptor_request);
  Check(!missing_result_descriptor.ok() &&
            !wrong_result_descriptor.ok() &&
            !non_exact_result_descriptor.ok() &&
            missing_result_descriptor.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID" &&
            wrong_result_descriptor.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID" &&
            non_exact_result_descriptor.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID" &&
            missing_result_descriptor.value.type_id ==
                dt::CanonicalTypeId::unknown &&
            wrong_result_descriptor.value.type_id ==
                dt::CanonicalTypeId::unknown &&
            non_exact_result_descriptor.value.type_id ==
                dt::CanonicalTypeId::unknown,
        "real128 numeric results require the exact bound result descriptor");

  auto compare_descriptor_request = NumericRequest(
      dt::DatatypeNumericOperationKind::compare, kOne, kTwo);
  compare_descriptor_request.result_descriptor = RealDescriptor();
  const auto wrong_compare_descriptor =
      dt::ApplyNumericOperation(compare_descriptor_request);
  Check(!wrong_compare_descriptor.ok() &&
            wrong_compare_descriptor.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID" &&
            wrong_compare_descriptor.value.type_id ==
                dt::CanonicalTypeId::unknown,
        "real128 numeric comparison refuses a non-boolean result descriptor");

  auto inexact = NumericRequest(dt::DatatypeNumericOperationKind::divide,
                                kOne, kThree);
  const auto inexact_result = dt::ApplyNumericOperation(inexact);
  Check(inexact_result.ok() && inexact_result.numeric_facts.inexact &&
            inexact_result.value.encoded_value.size() == 16,
        "real128 inexact arithmetic remains a successful binary value");

  auto underflow = NumericRequest(dt::DatatypeNumericOperationKind::divide,
                                  kMinSubnormal, kTwo);
  const auto underflow_result = dt::ApplyNumericOperation(underflow);
  Check(underflow_result.ok() && underflow_result.numeric_facts.inexact &&
            underflow_result.numeric_facts.underflow &&
            underflow_result.value.encoded_value == Bytes(kPositiveZero),
        "real128 tiny inexact result preserves underflow fact");

  const auto maximum = MaximumFinite();
  auto overflow = NumericRequest(dt::DatatypeNumericOperationKind::multiply,
                                 maximum, kTwo);
  const auto overflow_result = dt::ApplyNumericOperation(overflow);
  auto divide_zero = NumericRequest(dt::DatatypeNumericOperationKind::divide,
                                    kOne, kPositiveZero);
  const auto divide_zero_result = dt::ApplyNumericOperation(divide_zero);
  Check(!overflow_result.ok() && overflow_result.numeric_facts.overflow &&
            overflow_result.diagnostic.diagnostic_code ==
                "NUMERIC.REAL128.OVERFLOW" &&
            overflow_result.value.type_id == dt::CanonicalTypeId::unknown &&
            overflow_result.value.encoded_value.empty() &&
            !divide_zero_result.ok() &&
            divide_zero_result.numeric_facts.divide_by_zero &&
            divide_zero_result.diagnostic.diagnostic_code ==
                "NUMERIC.REAL128.DIVIDE_BY_ZERO" &&
            divide_zero_result.value.type_id == dt::CanonicalTypeId::unknown,
        "overflow and divide-by-zero publish facts and no stale value");

  auto special = NumericRequest(dt::DatatypeNumericOperationKind::canonicalize,
                                kQuietNan);
  const auto denied_special = dt::ApplyNumericOperation(special);
  special.context.allow_special_values = true;
  const auto admitted_special = dt::ApplyNumericOperation(special);
  Check(!denied_special.ok() && denied_special.numeric_facts.invalid &&
            admitted_special.ok() &&
            admitted_special.value.encoded_value == Bytes(kQuietNan),
        "request context controls special-value admission");

  dt::DatatypeComparisonRequest compare;
  compare.left = PresentBytes(Bytes(kOne));
  compare.right = PresentBytes(Bytes(kTwo));
  const auto less = dt::CompareDatatypeValues(compare);
  compare.left = PresentBytes(Bytes(kQuietNan));
  compare.numeric_context.allow_special_values = true;
  const auto unordered = dt::CompareDatatypeValues(compare);
  compare.left = PresentBytes(Bytes(kSignalingNan));
  const auto signaling = dt::CompareDatatypeValues(compare);
  Check(less.ok() && less.comparison == -1 && !unordered.ok() &&
            unordered.numeric_facts.unordered &&
            !unordered.numeric_facts.invalid && !signaling.ok() &&
            signaling.numeric_facts.invalid,
        "binary numeric compare distinguishes finite, qNaN unordered and sNaN invalid");

  compare.left = TypedNull();
  compare.right = PresentBytes(Bytes(kOne));
  compare.numeric_context = {};
  for (const auto ordering : {dt::DatatypeNullOrdering::nulls_first,
                              dt::DatatypeNullOrdering::nulls_last}) {
    compare.null_ordering = ordering;
    const auto result = dt::CompareDatatypeValues(compare);
    Check(result.ok() &&
              result.comparison ==
                  (ordering == dt::DatatypeNullOrdering::nulls_first ? -1 : 1),
          "real128 NULL placement remains caller-declared");
  }
  compare.null_ordering = static_cast<dt::DatatypeNullOrdering>(99);
  Check(!dt::CompareDatatypeValues(compare).ok(),
        "unknown NULL-ordering tag rejects before NULL placement");

  auto invalid_rounding = NumericRequest(dt::DatatypeNumericOperationKind::add,
                                         kOne, kTwo);
  invalid_rounding.left = TypedNull();
  invalid_rounding.context.rounding =
      static_cast<dt::DatatypeRoundingMode>(99);
  const auto invalid_rounding_result =
      dt::ApplyNumericOperation(invalid_rounding);
  Check(!invalid_rounding_result.ok() &&
            invalid_rounding_result.numeric_facts.invalid &&
            invalid_rounding_result.value.type_id ==
                dt::CanonicalTypeId::unknown,
        "unknown rounding rejects before strict NULL propagation");

  auto null_request = NumericRequest(dt::DatatypeNumericOperationKind::add,
                                     kOne, kTwo);
  null_request.left = TypedNull();
  const auto null_result = dt::ApplyNumericOperation(null_request);
  Check(null_result.ok() && null_result.value.is_null &&
            null_result.value.encoded_value.empty() &&
            SameUuidBytes(null_result.value.descriptor.descriptor_uuid,
                          kDescriptorUuid),
        "valid real128 arithmetic strictly propagates typed NULL with result descriptor");

  null_request.result_descriptor.nullable_allowed = false;
  const auto nonnullable_null_result =
      dt::ApplyNumericOperation(null_request);
  Check(!nonnullable_null_result.ok() &&
            nonnullable_null_result.diagnostic.diagnostic_code ==
                "DATATYPE.NULL_NOT_ADMITTED" &&
            nonnullable_null_result.value.type_id ==
                dt::CanonicalTypeId::unknown,
        "strict real128 NULL propagation requires a nullable result descriptor");
}

dt::DatatypeOperationValue FixedZero(dt::CanonicalTypeId type) {
  std::size_t width = 1;
  switch (type) {
    case dt::CanonicalTypeId::int16:
    case dt::CanonicalTypeId::uint16:
      width = 2;
      break;
    case dt::CanonicalTypeId::int32:
    case dt::CanonicalTypeId::uint32:
      width = 4;
      break;
    case dt::CanonicalTypeId::int64:
    case dt::CanonicalTypeId::uint64:
      width = 8;
      break;
    case dt::CanonicalTypeId::int128:
    case dt::CanonicalTypeId::uint128:
      width = 16;
      break;
    default:
      break;
  }
  dt::DatatypeOperationValue value{type, std::string(width, '\0'), false};
  value.descriptor = DescriptorFor(type);
  return value;
}

void PresentCastAndExchangeRefusal() {
  std::vector<dt::DatatypeOperationValue> incident{
      FixedZero(dt::CanonicalTypeId::int8),
      FixedZero(dt::CanonicalTypeId::int16),
      FixedZero(dt::CanonicalTypeId::int32),
      FixedZero(dt::CanonicalTypeId::int64),
      FixedZero(dt::CanonicalTypeId::int128),
      FixedZero(dt::CanonicalTypeId::uint8),
      FixedZero(dt::CanonicalTypeId::uint16),
      FixedZero(dt::CanonicalTypeId::uint32),
      FixedZero(dt::CanonicalTypeId::uint64),
      FixedZero(dt::CanonicalTypeId::uint128)};
  for (const auto& candidate : incident) {
    for (const auto context : {dt::DatatypeCastContext::implicit,
                               dt::DatatypeCastContext::assignment,
                               dt::DatatypeCastContext::explicit_cast}) {
      for (const bool compatibility : {false, true}) {
        dt::DatatypeCastRequest incoming;
        incoming.value = candidate;
        incoming.target_type_id = dt::CanonicalTypeId::real128;
        incoming.target_descriptor = RealDescriptor();
        incoming.context = context;
        incoming.explicit_cast =
            context == dt::DatatypeCastContext::explicit_cast;
        incoming.reference_compatibility_profile = compatibility;
        const auto incoming_result = dt::CastDatatypeValue(incoming);

        dt::DatatypeCastRequest outgoing;
        outgoing.value = PresentBytes(Bytes(kOne));
        outgoing.target_type_id = candidate.type_id;
        outgoing.target_descriptor = candidate.descriptor;
        outgoing.context = context;
        outgoing.explicit_cast =
            context == dt::DatatypeCastContext::explicit_cast;
        outgoing.reference_compatibility_profile = compatibility;
        const auto outgoing_result = dt::CastDatatypeValue(outgoing);
        Check(!incoming_result.ok() && !outgoing_result.ok() &&
                  incoming_result.value.type_id ==
                      dt::CanonicalTypeId::unknown &&
                  incoming_result.value.encoded_value.empty() &&
                  outgoing_result.value.type_id ==
                      dt::CanonicalTypeId::unknown &&
                  outgoing_result.value.encoded_value.empty(),
              "unadmitted PRESENT real128 integer incident casts refuse atomically");
      }
    }
  }

  const auto decfloat = dt::ResolveReferenceTypeLabelPlaceholder(
      dt::ReferenceDialectId::firebird, "DECFLOAT");
  Check(!decfloat.ok() &&
            decfloat.descriptor.type_id != dt::CanonicalTypeId::real128 &&
            decfloat.diagnostic.diagnostic_code ==
                "SB-DATATYPE-REFERENCE-LABEL-UNMAPPED",
        "Firebird DECFLOAT is not substituted with binary real128");
}

void CharacterCasts() {
  dt::DatatypeCastRequest parse;
  parse.value = {dt::CanonicalTypeId::character, "1.25", false};
  parse.value.descriptor = DescriptorFor(dt::CanonicalTypeId::character);
  parse.target_type_id = dt::CanonicalTypeId::real128;
  parse.target_descriptor = RealDescriptor();
  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    parse.context = context;
    parse.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    const auto parsed = dt::CastDatatypeValue(parse);
    Check(dt::ClassifyDatatypeCast(parse.value.type_id, parse.target_type_id) ==
              dt::DatatypeCastCategory::lossy_explicit,
          "character REAL128 conversion has checked potentially inexact policy");
    if (context == dt::DatatypeCastContext::implicit) {
      Check(!parsed.ok() && parsed.value.encoded_value.empty(),
            "implicit character REAL128 conversion refuses without a value");
    } else {
      const auto expected = numeric::EncodeReal128LittleEndian("1.25");
      Check(parsed.ok() && expected.bytes &&
                parsed.value.encoded_value == Bytes(*expected.bytes) &&
                !parsed.numeric_facts.inexact,
            "checked character REAL128 conversion publishes exact native bytes");
    }
    dt::DatatypeCastRequest render;
    render.value = PresentBytes(Bytes(kOnePointFive));
    render.target_type_id = dt::CanonicalTypeId::character;
    render.target_descriptor = parse.value.descriptor;
    render.context = context;
    render.explicit_cast = parse.explicit_cast;
    const auto rendered = dt::CastDatatypeValue(render);
    Check(context == dt::DatatypeCastContext::implicit
              ? !rendered.ok() && rendered.value.encoded_value.empty()
              : rendered.ok() && rendered.value.encoded_value == "1.5",
          "REAL128 character rendering observes cast context and canonical text");
  }
  parse.context = dt::DatatypeCastContext::explicit_cast;
  parse.explicit_cast = true;
  for (const auto invalid : {"", "1 trailing", "1e", "0x", "nan(payload)", "1e99999"}) {
    parse.value.encoded_value = invalid;
    const auto refused = dt::CastDatatypeValue(parse);
    Check(!refused.ok() && refused.value.encoded_value.empty() &&
              refused.diagnostic.diagnostic_code ==
                  (std::string_view(invalid) == "1e99999"
                       ? "NUMERIC.REAL128.OVERFLOW" : "NUMERIC.REAL128.INVALID"),
          "invalid or overflowing character REAL128 input preserves reference refusal");
  }
  parse.value.encoded_value = "1.1";
  const auto rounded = dt::CastDatatypeValue(parse);
  Check(rounded.ok() && rounded.value.encoded_value.size() == 16 &&
            rounded.numeric_facts.inexact,
        "character REAL128 conversion retains inexact fact");
  parse.value.encoded_value = "Infinity";
  Check(!dt::CastDatatypeValue(parse).ok(), "special input requires explicit numeric policy");
  parse.numeric_context.allow_special_values = true;
  const auto infinity = dt::CastDatatypeValue(parse);
  Check(infinity.ok() && infinity.value.encoded_value == Bytes(kPositiveInfinity),
        "admitted special character conversion keeps exact binary128 class");
  parse.value.is_null = true;
  parse.value.encoded_value.clear();
  const auto null_value = dt::CastDatatypeValue(parse);
  Check(null_value.ok() && null_value.value.is_null &&
            null_value.value.encoded_value.empty() &&
            null_value.value.type_id == dt::CanonicalTypeId::real128,
        "bound NULL character REAL128 conversion publishes target NULL only");
  parse.numeric_context.rounding = static_cast<dt::DatatypeRoundingMode>(99);
  Check(!dt::CastDatatypeValue(parse).ok(), "NULL conversion still validates numeric context");
  parse.numeric_context.rounding = dt::DatatypeRoundingMode::half_even;
  parse.value.is_null = false;
  parse.value.encoded_value = "1";
  parse.value.descriptor = {};
  const auto unbound = dt::CastDatatypeValue(parse);
  Check(!unbound.ok() && unbound.value.encoded_value.empty() &&
            unbound.diagnostic.diagnostic_code == "DATATYPE.DESCRIPTOR.INVALID",
        "character REAL128 conversion cannot infer missing source descriptor");
}

void AppendSetU16(std::string* bytes, std::uint16_t value) {
  bytes->push_back(static_cast<char>((value >> 8u) & 0xffu));
  bytes->push_back(static_cast<char>(value & 0xffu));
}
void AppendSetU32(std::string* bytes, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    bytes->push_back(static_cast<char>((value >> shift) & 0xffu));
  }
}
void AppendSetU64(std::string* bytes, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    bytes->push_back(static_cast<char>((value >> shift) & 0xffu));
  }
}
void AppendSetUuid(std::string* bytes,
                   const scratchbird::engine::Uuid& uuid) {
  bytes->append(reinterpret_cast<const char*>(uuid.bytes), sizeof(uuid.bytes));
}
void AppendSetString(std::string* bytes, std::string_view value) {
  AppendSetU32(bytes, static_cast<std::uint32_t>(value.size()));
  bytes->append(value);
}
std::string LowerHex(std::string_view bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (const unsigned char byte : bytes) {
    result.push_back(digits[byte >> 4u]);
    result.push_back(digits[byte & 0x0fu]);
  }
  return result;
}
std::string SetDescriptorFingerprint(
    const scratchbird::engine::ExecutionTypeDescriptor& descriptor) {
  std::string bytes = "SBTD1";
  AppendSetUuid(&bytes, descriptor.descriptor_uuid);
  AppendSetU64(&bytes, descriptor.descriptor_epoch);
  AppendSetU32(&bytes, descriptor.canonical_type_id);
  AppendSetU16(&bytes, static_cast<std::uint16_t>(descriptor.family));
  AppendSetU16(&bytes, static_cast<std::uint16_t>(descriptor.width_class));
  AppendSetString(&bytes, descriptor.stable_name);
  AppendSetU32(&bytes, descriptor.bit_width);
  AppendSetU32(&bytes, descriptor.precision);
  AppendSetU32(&bytes, descriptor.scale);
  AppendSetU32(&bytes, descriptor.length);
  AppendSetU32(&bytes, descriptor.vector_dimensions);
  AppendSetU32(&bytes, descriptor.container_rank);
  AppendSetU64(&bytes, descriptor.modifier_flags);
  AppendSetUuid(&bytes, descriptor.domain_uuid);
  AppendSetU32(&bytes,
               static_cast<std::uint32_t>(descriptor.domain_stack.size()));
  for (const auto& domain : descriptor.domain_stack) AppendSetUuid(&bytes, domain);
  AppendSetUuid(&bytes, descriptor.charset_uuid);
  AppendSetUuid(&bytes, descriptor.collation_uuid);
  AppendSetUuid(&bytes, descriptor.timezone_uuid);
  AppendSetUuid(&bytes, descriptor.element_descriptor_uuid);
  AppendSetUuid(&bytes, descriptor.security_policy_uuid);
  bytes.push_back(descriptor.nullable_allowed ? '\1' : '\0');
  bytes.push_back(descriptor.descriptor_authoritative ? '\1' : '\0');
  bytes.push_back(descriptor.parser_independent ? '\1' : '\0');
  return LowerHex(bytes);
}

std::string RealSetFrame(
    const scratchbird::engine::ExecutionTypeDescriptor& descriptor,
    std::string_view items, bool allow_nulls = false) {
  return "SBSET2;element=real128;descriptor=" +
      SetDescriptorFingerprint(descriptor) +
      ";ordered=0;nulls=" + (allow_nulls ? "1" : "0") +
      ";duplicates=0;items=" + std::string(items);
}

void PolicyAndSerializationSurfaces() {
  const auto present = PresentBytes(Bytes(kOne));
  const auto null_value = TypedNull();

  dt::DatatypeExtractRequest extract;
  extract.value = present;
  const auto extracted = dt::ExtractDatatypeField(extract);
  const auto hash = dt::HashDatatypeValue({present});
  const auto null_hash = dt::HashDatatypeValue({null_value});
  const auto key = dt::MakeDatatypeSortKey({present});
  const auto null_key = dt::MakeDatatypeSortKey({null_value});
  const auto display = dt::RenderDatatypeValueForDisplay({present});
  const auto null_display = dt::RenderDatatypeValueForDisplay({null_value});
  Check(!extracted.ok() && extracted.value.type_id == dt::CanonicalTypeId::unknown &&
            extracted.diagnostic.diagnostic_code ==
                "SB_DATATYPE_EXTRACT_REJECTED" &&
            DiagnosticDetail(extracted.diagnostic) ==
                "real128_extract_policy_unresolved" &&
            !hash.ok() && hash.stable_hash_hex.empty() && !null_hash.ok() &&
            hash.diagnostic.diagnostic_code == "SB_DATATYPE_HASH_REJECTED" &&
            DiagnosticDetail(hash.diagnostic) ==
                "real128_hash_policy_unresolved" &&
            DiagnosticDetail(null_hash.diagnostic) ==
                "real128_hash_policy_unresolved" &&
            !key.ok() && key.sort_key.empty() && !null_key.ok() &&
            key.diagnostic.diagnostic_code ==
                "SB_DATATYPE_SORT_KEY_REJECTED" &&
            DiagnosticDetail(key.diagnostic) ==
                "real128_sort_key_policy_unresolved" &&
            DiagnosticDetail(null_key.diagnostic) ==
                "real128_sort_key_policy_unresolved" &&
            !display.ok() && display.display_value.empty() &&
            display.diagnostic.diagnostic_code ==
                "SB_DATATYPE_DISPLAY_RENDER_REJECTED" &&
            DiagnosticDetail(display.diagnostic) ==
                "real128_display_policy_unresolved" &&
            !null_display.ok() && null_display.display_value.empty() &&
            DiagnosticDetail(null_display.diagnostic) ==
                "real128_display_policy_unresolved",
        "extract/hash/key/display surfaces fail closed without policy");

  dt::DatatypeSetDescriptor set_descriptor;
  set_descriptor.element_type_id = dt::CanonicalTypeId::real128;
  set_descriptor.element_descriptor = RealDescriptor();
  const auto encoded = dt::EncodeSetValue(set_descriptor, {present});
  const auto empty = dt::EncodeSetValue(set_descriptor, {});
  auto nullable_descriptor = set_descriptor;
  nullable_descriptor.allow_null_elements = true;
  const auto null_set = dt::EncodeSetValue(nullable_descriptor, {null_value});
  Check(!encoded.ok() && !empty.ok() && !null_set.ok() &&
            encoded.encoded_set.empty() && empty.encoded_set.empty() &&
            null_set.encoded_set.empty() &&
            DiagnosticDetail(encoded.diagnostic) ==
                "real128_set_semantics_policy_unresolved" &&
            DiagnosticDetail(empty.diagnostic) ==
                "real128_set_semantics_policy_unresolved" &&
            DiagnosticDetail(null_set.diagnostic) ==
                "real128_set_semantics_policy_unresolved",
        "real128 set construction including empty and typed-NULL-only refuses");

  const std::string token = "V" + LowerHex(present.encoded_value);
  const std::string frame = RealSetFrame(RealDescriptor(), token);
  for (const auto operation : {dt::DatatypeSetOperationKind::membership,
                               dt::DatatypeSetOperationKind::equals,
                               dt::DatatypeSetOperationKind::subset,
                               dt::DatatypeSetOperationKind::superset,
                               dt::DatatypeSetOperationKind::cardinality}) {
    dt::DatatypeSetOperationRequest request;
    request.operation = operation;
    request.descriptor = set_descriptor;
    request.left_encoded_set = frame;
    request.right_encoded_set = frame;
    request.right_value = present;
    const auto result = dt::ApplySetOperation(request);
    Check(!result.ok() && result.encoded_set.empty() &&
              result.value.type_id == dt::CanonicalTypeId::unknown &&
              result.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_SET_OPERATION_REJECTED" &&
              DiagnosticDetail(result.diagnostic) ==
                  "real128_set_semantics_policy_unresolved",
          "every real128 set operation including cardinality refuses");
  }

  dt::DatatypeSetOperationRequest frame_validation;
  frame_validation.operation = dt::DatatypeSetOperationKind::equals;
  frame_validation.descriptor = set_descriptor;
  frame_validation.left_encoded_set = frame;
  frame_validation.right_encoded_set = frame + "00";
  const auto malformed_right = dt::ApplySetOperation(frame_validation);
  auto alternate_real_descriptor = RealDescriptor();
  alternate_real_descriptor.security_policy_uuid = FixtureV7Uuid(0xd0);
  alternate_real_descriptor.modifier_flags |=
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          scratchbird::engine::ExecutionTypeModifierFlag::security_policy_uuid);
  frame_validation.right_encoded_set =
      RealSetFrame(alternate_real_descriptor, token);
  const auto mismatched_right = dt::ApplySetOperation(frame_validation);
  Check(!malformed_right.ok() &&
            DiagnosticDetail(malformed_right.diagnostic) ==
                "right_set_encoding_invalid" &&
            !mismatched_right.ok() &&
            mismatched_right.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID" &&
            DiagnosticDetail(mismatched_right.diagnostic) ==
                "right_set_descriptor_mismatch",
        "set frame and descriptor errors precede real128 policy refusal");

  auto malformed_member = present;
  malformed_member.encoded_value.assign(15, '\0');
  dt::DatatypeSetOperationRequest membership;
  membership.operation = dt::DatatypeSetOperationKind::membership;
  membership.descriptor = set_descriptor;
  membership.left_encoded_set = frame;
  membership.right_value = malformed_member;
  const auto malformed_membership = dt::ApplySetOperation(membership);
  Check(!malformed_membership.ok() &&
            DiagnosticDetail(malformed_membership.diagnostic) ==
                "set_membership_value_invalid",
        "membership validates LE16 before policy refusal");

  const auto serialized = dt::SerializeDatatypeValue({present});
  dt::DatatypeDeserializationRequest restore;
  restore.expected_type_id = dt::CanonicalTypeId::real128;
  restore.expected_descriptor = RealDescriptor();
  restore.serialized_value = serialized.serialized_value;
  const auto restored = serialized.ok()
      ? dt::DeserializeDatatypeValue(restore)
      : dt::DatatypeDeserializationResult{};
  const std::string expected_present_frame =
      "SBDV1;type=real128;state=value;payload=" + LowerHex(Bytes(kOne));
  Check(serialized.ok() &&
            serialized.serialized_value == expected_present_frame &&
            serialized.descriptor.canonical_type_id ==
                static_cast<std::uint32_t>(dt::CanonicalTypeId::real128) &&
            !restored.ok() &&
            restored.diagnostic.diagnostic_code ==
                "SB_DATATYPE_DESERIALIZATION_REJECTED" &&
            DiagnosticDetail(restored.diagnostic) ==
                "real128_deserialization_policy_unresolved" &&
            restored.value.type_id == dt::CanonicalTypeId::unknown &&
            restored.value.encoded_value.empty(),
        "exact LE16 PRESENT serializes but generic PRESENT deserialize refuses");

  const auto null_serialized = dt::SerializeDatatypeValue({null_value});
  restore.serialized_value = null_serialized.serialized_value;
  const auto null_restored = null_serialized.ok()
      ? dt::DeserializeDatatypeValue(restore)
      : dt::DatatypeDeserializationResult{};
  Check(null_serialized.ok() && null_restored.ok() &&
            null_restored.value.is_null &&
            null_restored.value.encoded_value.empty() &&
            SameUuidBytes(null_restored.value.descriptor.descriptor_uuid,
                          kDescriptorUuid),
        "descriptor-bearing generic framing preserves typed real128 NULL");

  restore.serialized_value =
      "SBDV1;type=real128;state=value;payload=zz";
  const auto malformed_hex = dt::DeserializeDatatypeValue(restore);
  restore.serialized_value =
      "SBDV1;type=real128;state=value;payload=" +
      LowerHex(std::string(15, '\0'));
  const auto wrong_width = dt::DeserializeDatatypeValue(restore);
  Check(!malformed_hex.ok() &&
            malformed_hex.diagnostic.diagnostic_code ==
                "SB_DATATYPE_DESERIALIZATION_REJECTED" &&
            DiagnosticDetail(malformed_hex.diagnostic) ==
                "payload_hex_invalid" &&
            !wrong_width.ok() &&
            wrong_width.diagnostic.diagnostic_code ==
                "NUMERIC.ENCODING.NONCANONICAL" &&
            DiagnosticDetail(wrong_width.diagnostic) ==
                "real128_payload_width_invalid",
        "real128 PRESENT deserialization distinguishes malformed hex and width");

  restore.expected_descriptor = {};
  restore.serialized_value = "SBDV1;type=real128;state=value;payload=zz";
  const auto missing_descriptor = dt::DeserializeDatatypeValue(restore);
  restore.expected_descriptor = RealDescriptor();
  restore.expected_descriptor.stable_name = "alias-only-name-change";
  restore.expected_descriptor.security_policy_uuid = FixtureV7Uuid(0xe0);
  restore.expected_descriptor.modifier_flags |=
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          scratchbird::engine::ExecutionTypeModifierFlag::security_policy_uuid);
  const auto mismatched_descriptor = dt::DeserializeDatatypeValue(restore);
  Check(!missing_descriptor.ok() &&
            missing_descriptor.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID" &&
            !mismatched_descriptor.ok() &&
            mismatched_descriptor.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID",
        "deserialization validates exact descriptor before malformed payload");
}

void DescriptorAndWidthPrecedence() {
  auto malformed = PresentBytes(std::string(15, '\0'));
  auto missing = malformed;
  missing.descriptor = {};
  auto label_only = missing;
  label_only.descriptor.stable_name = "real128";
  auto mismatch = malformed;
  mismatch.descriptor.security_policy_uuid = FixtureV7Uuid(0xa0);
  mismatch.descriptor.modifier_flags |=
      scratchbird::engine::ExecutionTypeModifierFlagBit(
          scratchbird::engine::ExecutionTypeModifierFlag::security_policy_uuid);

  for (const auto& value : {missing, label_only, mismatch}) {
    dt::DatatypeNumericOperationRequest numeric_request;
    numeric_request.operation = dt::DatatypeNumericOperationKind::canonicalize;
    numeric_request.type_id = dt::CanonicalTypeId::real128;
    numeric_request.left = value;
    numeric_request.result_descriptor = RealDescriptor();
    const auto numeric_result = dt::ApplyNumericOperation(numeric_request);
    const auto compare_result =
        dt::CompareDatatypeValues({value, PresentBytes(Bytes(kOne))});
    const auto serialized = dt::SerializeDatatypeValue({value});
    Check(!numeric_result.ok() &&
              numeric_result.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              !compare_result.ok() &&
              compare_result.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID" &&
              !serialized.ok() &&
              serialized.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID",
          "descriptor invalidity precedes real128 carrier diagnostics");
  }
  dt::DatatypeNumericOperationRequest numeric_request;
  numeric_request.operation = dt::DatatypeNumericOperationKind::canonicalize;
  numeric_request.type_id = dt::CanonicalTypeId::real128;
  numeric_request.left = malformed;
  numeric_request.result_descriptor = RealDescriptor();
  const auto numeric_result = dt::ApplyNumericOperation(numeric_request);
  const auto serialized = dt::SerializeDatatypeValue({malformed});
  Check(!numeric_result.ok() &&
            numeric_result.diagnostic.diagnostic_code ==
                "NUMERIC.ENCODING.NONCANONICAL" &&
            numeric_result.value.type_id == dt::CanonicalTypeId::unknown &&
            !serialized.ok() &&
            serialized.diagnostic.diagnostic_code ==
                "NUMERIC.ENCODING.NONCANONICAL" &&
            serialized.serialized_value.empty(),
        "exact descriptor exposes malformed LE16 and all failures are atomic");

  auto right_invalid = NumericRequest(dt::DatatypeNumericOperationKind::add,
                                      kOne, kTwo);
  right_invalid.right.descriptor = {};
  right_invalid.right.encoded_value.assign(15, '\0');
  const auto right_descriptor_failure =
      dt::ApplyNumericOperation(right_invalid);
  const auto right_compare_descriptor_failure =
      dt::CompareDatatypeValues({PresentBytes(Bytes(kOne)),
                                 right_invalid.right});
  right_invalid.right.descriptor = RealDescriptor();
  const auto right_width_failure = dt::ApplyNumericOperation(right_invalid);
  const auto right_compare_width_failure =
      dt::CompareDatatypeValues({PresentBytes(Bytes(kOne)),
                                 right_invalid.right});
  Check(!right_descriptor_failure.ok() &&
            right_descriptor_failure.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID" &&
            right_descriptor_failure.value.type_id ==
                dt::CanonicalTypeId::unknown &&
            !right_compare_descriptor_failure.ok() &&
            right_compare_descriptor_failure.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID" &&
            !right_width_failure.ok() &&
            right_width_failure.diagnostic.diagnostic_code ==
                "NUMERIC.ENCODING.NONCANONICAL" &&
            right_width_failure.value.type_id ==
                dt::CanonicalTypeId::unknown &&
            !right_compare_width_failure.ok() &&
            right_compare_width_failure.diagnostic.diagnostic_code ==
                "NUMERIC.ENCODING.NONCANONICAL",
        "right operand descriptor precedence and LE16 width fail atomically");
}

void Append(std::vector<platform::byte>* output,
            const std::vector<platform::byte>& bytes) {
  output->insert(output->end(), bytes.begin(), bytes.end());
}

void Persistence() {
  const std::array<numeric::Real128Bytes, 4> patterns{
      kPositiveZero, kNegativeZero, kOne, kQuietNan};
  std::vector<std::vector<platform::byte>> frames;
  for (const auto& bits : patterns) {
    const auto encoded = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::real128,
         dt::DatatypePhysicalValueState::value, Payload(Bytes(bits))});
    Check(encoded.ok(), "encode real128 physical persistence frame");
    frames.push_back(encoded.bytes);
  }
  const auto null_frame = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::real128,
       dt::DatatypePhysicalValueState::sql_null, {}});
  Check(null_frame.ok(), "encode real128 NULL physical persistence frame");
  frames.push_back(null_frame.bytes);

  constexpr std::size_t header_bytes = 128;
  std::vector<platform::byte> expected(header_bytes, 0);
  const std::array<platform::byte, 8> magic{{'S','B','R','1','2','8','0','1'}};
  std::copy(magic.begin(), magic.end(), expected.begin());
  std::copy(kDescriptorUuid.bytes.begin(), kDescriptorUuid.bytes.end(),
            expected.begin() + 8);
  std::copy(kTypeUuid.bytes.begin(), kTypeUuid.bytes.end(),
            expected.begin() + 24);
  std::copy(kCodecUuid.bytes.begin(), kCodecUuid.bytes.end(),
            expected.begin() + 40);
  platform::StoreLittle32(expected.data() + 56,
                          static_cast<std::uint32_t>(frames.size()));
  std::uint32_t offset = header_bytes;
  for (std::size_t index = 0; index < frames.size(); ++index) {
    platform::StoreLittle32(expected.data() + 64 + index * 8, offset);
    platform::StoreLittle32(expected.data() + 68 + index * 8,
                            static_cast<std::uint32_t>(frames[index].size()));
    offset += static_cast<std::uint32_t>(frames[index].size());
  }
  for (const auto& frame : frames) Append(&expected, frame);

#ifdef _WIN32
  const auto pid = ::_getpid();
#else
  const auto pid = ::getpid();
#endif
  const fs::path path = fs::temp_directory_path() /
      ("sb-base-real128-" + std::to_string(pid) + ".carrier");
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code error;
      fs::remove(path, error);
    }
  } cleanup{path};

  disk::FileDevice writer;
  Check(writer.Open(path.string(), disk::FileOpenMode::create_new).ok(),
        "create real128 FileDevice fixture");
  const auto write = writer.WriteAt(0, expected.data(), expected.size());
  Check(write.ok() && write.bytes_transferred == expected.size() &&
            writer.Sync().ok() && writer.Close().ok(),
        "write, sync and close real128 fixture");

  disk::FileDevice reader;
  Check(reader.Open(path.string(),
                    disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen real128 fixture read-only");
  std::vector<platform::byte> actual(expected.size());
  const auto read = reader.ReadAt(0, actual.data(), actual.size());
  bool decoded_all = read.ok() && actual == expected;
  for (std::size_t index = 0; index < frames.size() && decoded_all; ++index) {
    const auto frame_offset =
        platform::LoadLittle32(actual.data() + 64 + index * 8);
    const auto frame_size =
        platform::LoadLittle32(actual.data() + 68 + index * 8);
    const auto decoded = dt::DecodeDatatypePhysicalValue(
        actual.data() + frame_offset, frame_size);
    decoded_all = decoded.ok() &&
        decoded.value.type_id == dt::CanonicalTypeId::real128 &&
        (index == patterns.size()
             ? decoded.value.state ==
                       dt::DatatypePhysicalValueState::sql_null &&
                   decoded.value.payload.empty()
             : decoded.value.state ==
                       dt::DatatypePhysicalValueState::value &&
                   decoded.value.payload == Payload(Bytes(patterns[index])));
  }
  Check(read.ok() && read.bytes_transferred == expected.size() && decoded_all,
        "FileDevice reopen preserves exact real128 identity and LE16 frames");
  std::array<platform::byte, 2> short_buffer{};
  const auto short_read = reader.ReadAt(expected.size() - 1,
                                        short_buffer.data(),
                                        short_buffer.size());
  const auto rejected_write = reader.WriteAt(0, magic.data(), 1);
  Check(!short_read.ok() && short_read.bytes_transferred < short_buffer.size() &&
            !rejected_write.ok() && rejected_write.bytes_transferred == 0 &&
            reader.Close().ok(),
        "FileDevice short-read and read-only protections are observable");

  disk::FileDevice corrupter;
  Check(corrupter.Open(path.string(), disk::FileOpenMode::open_existing).ok(),
        "reopen real128 fixture for corruption");
  const platform::byte corrupt_checksum =
      static_cast<platform::byte>(expected[header_bytes + 20] ^ 1u);
  const auto corrupt_write = corrupter.WriteAt(
      header_bytes + 20, &corrupt_checksum, sizeof(corrupt_checksum));
  Check(corrupt_write.ok() && corrupter.Sync().ok() && corrupter.Close().ok(),
        "persist corrupt real128 physical checksum");
  disk::FileDevice corrupt_reader;
  Check(corrupt_reader.Open(
            path.string(), disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen corrupt real128 fixture");
  std::vector<platform::byte> corrupt_actual(expected.size());
  const auto corrupt_read = corrupt_reader.ReadAt(
      0, corrupt_actual.data(), corrupt_actual.size());
  const auto corrupt_decoded = corrupt_read.ok()
      ? dt::DecodeDatatypePhysicalValue(
            corrupt_actual.data() + header_bytes, frames.front().size())
      : dt::DatatypePhysicalEncodingResult{};
  Check(corrupt_read.ok() && !corrupt_decoded.ok() &&
            corrupt_reader.Close().ok(),
        "FileDevice reopen exposes corruption to physical decoder");

  disk::FileDevice truncator;
  Check(truncator.Open(path.string(),
                       disk::FileOpenMode::create_or_truncate).ok(),
        "open real128 fixture through truncate mode");
  const auto truncated_write = truncator.WriteAt(
      0, expected.data(), expected.size() - 1);
  Check(truncated_write.ok() && truncator.Sync().ok() && truncator.Close().ok(),
        "persist one-byte-truncated real128 fixture");
  disk::FileDevice truncated_reader;
  Check(truncated_reader.Open(
            path.string(), disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen truncated real128 fixture");
  std::vector<platform::byte> truncated_actual(expected.size());
  const auto truncated_read = truncated_reader.ReadAt(
      0, truncated_actual.data(), truncated_actual.size());
  Check(!truncated_read.ok() &&
            truncated_read.bytes_transferred < truncated_actual.size() &&
            truncated_reader.Close().ok(),
        "FileDevice refuses full read from truncated real128 fixture");
}

}  // namespace

int main() {
  ExactIdentityAndVectors();
  LowerCodecs();
  DescriptorEnvelope();
  IdentityAndNulls();
  NumericOperationsAndComparison();
  PresentCastAndExchangeRefusal();
  CharacterCasts();
  PolicyAndSerializationSurfaces();
  DescriptorAndWidthPrecedence();
  Persistence();
  numeric::ReleaseReal128ThreadCache();
  std::cout << "base real128 canonical-value checks=" << checks
            << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
