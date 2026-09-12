// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Compile the actual private codec in this translation unit. Section GC omits
// unrelated executor entrypoints; no substitute executor or mock is linked.
// This is a spill-codec component test, not query execution or recovery proof.
#include "../../src/engine/executor/aggregate_executor.cpp"

#include <iostream>
#include <stdexcept>

namespace codec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {
void Require(bool value, const char* detail) {
  if (!value) throw std::runtime_error(detail);
}

void GoldenU64(std::vector<std::uint8_t>* bytes, std::uint64_t value) {
  for (unsigned index = 0; index < 8; ++index) {
    bytes->push_back(static_cast<std::uint8_t>(value % 256));
    value /= 256;
  }
}
void GoldenText(std::vector<std::uint8_t>* bytes, const std::string_view text) {
  GoldenU64(bytes, text.size());
  bytes->insert(bytes->end(), text.begin(), text.end());
}
} // namespace

int main() {
  try {
    const api::EngineUuid descriptor{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x14}};
    const api::EngineUuid type{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x15}};
    api::EngineTypedValue value;
    value.descriptor.descriptor_uuid = descriptor;
    value.descriptor.type_uuid = type;
    value.descriptor.descriptor_kind = "scalar";
    value.descriptor.canonical_type_name = "int128";
    value.descriptor.encoded_descriptor = "nullability=non_null";
    value.state = api::EngineValueState::value;
    value.binary_value = {0,1,2,3,4,5,0x17,7,0x89,9,10,11,12,13,14,0xff};

    // Independent Core boolean v1 tuple; identity bytes never come from
    // metadata text. Exercise the actual join role-domain validator.
    const api::EngineUuid boolean{{0x01,0,0,0,0x62,0x6f,0x7f,0x6c,0xa5,0x61,0x6e,0,0,0,0,0}};
    auto boolean_descriptor = value.descriptor;
    boolean_descriptor.descriptor_uuid = boolean;
    boolean_descriptor.type_uuid = boolean;
    boolean_descriptor.canonical_type_name = "boolean";
    const std::string boolean_fields = "datatype_descriptor_generation=1;type_generation=1;codec_id=datatype.boolean.u8.v1;codec_version=1;codec_generation=1;null_encoding=1;nullability=";
    boolean_descriptor.encoded_descriptor = boolean_fields + "nullable";
    codec::DescriptorBatch ordinary_batch, boolean_batch;
    ordinary_batch.columns.push_back({"ordinary", value.descriptor, false, 801});
    boolean_batch.columns.push_back({"boolean", boolean_descriptor, true, 802});
    Require(codec::ValidateCanonicalJoinDescriptorRoleDomains(boolean_batch, ordinary_batch).ok,
            "join rejected the exact binary boolean alias tuple");
    auto altered = boolean_batch;
    altered.columns[0].nullable = false;
    altered.columns[0].descriptor.encoded_descriptor = boolean_fields + "non_null";
    Require(codec::ValidateCanonicalJoinDescriptorRoleDomains(altered, ordinary_batch).ok,
            "join rejected exact non-null binary boolean alias");
    for (const auto fields : {
        "datatype_descriptor_generation=2;type_generation=1;codec_id=datatype.boolean.u8.v1;codec_version=1;codec_generation=1;null_encoding=1;nullability=nullable",
        "datatype_descriptor_generation=1;type_generation=2;codec_id=datatype.boolean.u8.v1;codec_version=1;codec_generation=1;null_encoding=1;nullability=nullable",
        "datatype_descriptor_generation=1;type_generation=1;codec_id=datatype.boolean.lookalike.v1;codec_version=1;codec_generation=1;null_encoding=1;nullability=nullable",
        "datatype_descriptor_generation=1;type_generation=1;codec_id=datatype.boolean.u8.v1;codec_version=2;codec_generation=1;null_encoding=1;nullability=nullable",
        "datatype_descriptor_generation=1;type_generation=1;codec_id=datatype.boolean.u8.v1;codec_version=1;codec_generation=2;null_encoding=1;nullability=nullable",
        "datatype_descriptor_generation=1;type_generation=1;codec_id=datatype.boolean.u8.v1;codec_version=1;codec_generation=1;null_encoding=0;nullability=nullable"}) {
      altered = boolean_batch;
      altered.columns[0].descriptor.encoded_descriptor = fields;
      Require(!codec::ValidateCanonicalJoinDescriptorRoleDomains(altered, ordinary_batch).ok,
              "join boolean alias accepted stale generation or substituted codec/null encoding");
    }
    altered = boolean_batch;
    altered.columns[0].nullable = false;
    Require(!codec::ValidateCanonicalJoinDescriptorRoleDomains(altered, ordinary_batch).ok,
            "join alias ignored containing slot nullability");
    altered = boolean_batch;
    altered.columns[0].descriptor.collation_uuid = descriptor;
    Require(!codec::ValidateCanonicalJoinDescriptorRoleDomains(altered, ordinary_batch).ok,
            "join alias accepted collation on the canonical boolean tuple");
    altered = ordinary_batch;
    altered.columns[0].descriptor.type_uuid = descriptor;
    Require(!codec::ValidateCanonicalJoinDescriptorRoleDomains(altered, ordinary_batch).ok,
            "join accepted a nonboolean descriptor/type domain collision");
    for (unsigned bit = 0; bit != 128; ++bit) {
      altered = boolean_batch;
      altered.columns[0].descriptor.descriptor_uuid.bytes[bit / 8] ^= (1u << (bit % 8));
      altered.columns[0].descriptor.type_uuid = altered.columns[0].descriptor.descriptor_uuid;
      Require(!codec::ValidateCanonicalJoinDescriptorRoleDomains(altered, ordinary_batch).ok,
              "join granted the canonical alias exception to mutated UUID bytes");
    }
    for (const auto field : {"descriptor_uuid", "datatype_descriptor_uuid", "type_uuid", "collation_uuid"}) {
      altered = ordinary_batch;
      altered.columns[0].descriptor.encoded_descriptor += ";" + std::string(field) + "=01000000-626f-7f6c-a561-6e0000000000";
      Require(!codec::ValidateCanonicalJoinDescriptorRoleDomains(altered, boolean_batch).ok,
              "join accepted competing textual identity metadata");
    }

    codec::CanonicalDescriptorOrderTerm digest_term;
    digest_term.expression_descriptor_id = 1;
    std::uint64_t digest_workspace = 0;
    std::uint64_t digest_actual = 0;
    const codec::CanonicalOrderTermBindingDigest expected_digest{
        0x3e,0xae,0xc4,0x68,0xc0,0x00,0x3a,0x4b,
        0x2b,0xb8,0xb0,0x13,0x7f,0xf9,0x11,0xd3,
        0x6a,0x12,0x64,0xfd,0x1f,0x61,0x58,0x7e,
        0x87,0xa4,0xbc,0x59,0x7c,0x6f,0xe8,0x81};
    static_assert(sizeof(codec::CanonicalOrderTermBindingDigest) == 32);
    // Independent BE framing plus SHA-256 oracle: 228 encoded bytes and
    // 32 digest bytes. This hashes content only; it issues no receipt UUID.
    Require(codec::PlanCanonicalDescriptorOrderTermBindingDigestWorkspace(
                digest_term, descriptor, &digest_workspace) && digest_workspace == 260,
            "order binding digest planning differs from independent byte oracle");
    const auto digest = codec::ComputeCanonicalDescriptorOrderTermBindingDigest(
        digest_term, descriptor, digest_workspace, &digest_actual);
    Require(digest.has_value() && *digest == expected_digest && digest_actual == digest_workspace,
            "order binding changed or truncated the full SHA-256 digest");
    Require(!codec::ComputeCanonicalDescriptorOrderTermBindingDigest(
                digest_term, descriptor, digest_workspace - 1, &digest_actual).has_value() &&
                digest_actual == 0,
            "order binding digest exceeded its workspace ceiling");
    for (unsigned byte = 0; byte < 16; ++byte) {
      auto changed = descriptor;
      changed.bytes[byte] ^= 1;
      const auto changed_digest = codec::ComputeCanonicalDescriptorOrderTermBindingDigest(
          digest_term, changed, digest_workspace, &digest_actual);
      Require(changed_digest.has_value() && *changed_digest != expected_digest,
              "order binding digest ignored a changed UUID byte");
    }
    auto invalid_property = descriptor;
    invalid_property.bytes[6] = 0x80;
    Require(!codec::ComputeCanonicalDescriptorOrderTermBindingDigest(
                digest_term, invalid_property, digest_workspace, &digest_actual).has_value(),
            "order binding digest admitted UUIDv8 as system property identity");

    Require(codec::IsCanonicalInt128DescriptorV1(value.descriptor) &&
            codec::DescriptorMatches(value.descriptor, value.descriptor),
            "binary int128 descriptor authority was not retained");
    auto changed_descriptor = value.descriptor;
    changed_descriptor.type_uuid = {};
    changed_descriptor.encoded_descriptor +=
        ";type_uuid=019d0000-0000-7000-8000-00000000d715";
    Require(!codec::IsCanonicalInt128DescriptorV1(changed_descriptor) &&
            !codec::DescriptorMatches(value.descriptor, changed_descriptor),
            "text UUID metadata substituted for bound binary type authority");
    changed_descriptor = value.descriptor;
    changed_descriptor.descriptor_uuid.bytes[15] ^= 0x40;
    changed_descriptor.encoded_descriptor = "nullability=nullable";
    Require(codec::CanonicalDerivedDescriptorTypeMatches(
                value.descriptor, false, changed_descriptor, true),
            "nullable derivation lost a valid common binary type identity");
    changed_descriptor.type_uuid.bytes[15] ^= 0x40;
    Require(!codec::CanonicalDerivedDescriptorTypeMatches(
                value.descriptor, false, changed_descriptor, true),
            "nullable derivation admitted a different binary type identity");

    const codec::ExecutorColumnDescriptor column{"v", value.descriptor, false, 1};
    const auto fingerprint = codec::DescriptorFingerprint({column});
    std::vector<std::uint8_t> fingerprint_bytes;
    const std::string_view fingerprint_version = "scratchbird.descriptor-fingerprint.v3";
    fingerprint_bytes.insert(fingerprint_bytes.end(), fingerprint_version.begin(), fingerprint_version.end());
    GoldenU64(&fingerprint_bytes, 1);
    GoldenText(&fingerprint_bytes, "v");
    fingerprint_bytes.insert(fingerprint_bytes.end(), descriptor.bytes.begin(), descriptor.bytes.end());
    fingerprint_bytes.insert(fingerprint_bytes.end(), type.bytes.begin(), type.bytes.end());
    fingerprint_bytes.insert(fingerprint_bytes.end(), 16, 0);
    GoldenText(&fingerprint_bytes, "scalar");
    GoldenText(&fingerprint_bytes, "int128");
    GoldenText(&fingerprint_bytes, "nullability=non_null");
    fingerprint_bytes.push_back(0);
    Require(std::vector<std::uint8_t>(fingerprint.begin(), fingerprint.end()) == fingerprint_bytes,
            "descriptor fingerprint differs from independent binary16 oracle");
    for (unsigned identity = 0; identity < 2; ++identity) {
      for (unsigned byte = 0; byte < 16; ++byte) {
        auto altered = column;
        auto& uuid = identity == 0 ? altered.descriptor.descriptor_uuid : altered.descriptor.type_uuid;
        uuid.bytes[byte] ^= 1;
        Require(codec::DescriptorFingerprint({altered}) != fingerprint &&
                !codec::DescriptorMatches(column.descriptor, altered.descriptor),
                "descriptor key or comparison ignored a changed UUID byte");
      }
    }
    auto framed_left = column;
    auto framed_right = column;
    framed_left.stable_name = "v:scalar";
    framed_left.descriptor.descriptor_kind = "scalar";
    framed_right.stable_name = "v";
    framed_right.descriptor.descriptor_kind = "scalar:scalar";
    Require(codec::DescriptorFingerprint({framed_left}) != codec::DescriptorFingerprint({framed_right}),
            "descriptor fingerprint aliases delimiter-bearing fields");

    std::vector<std::uint8_t> golden;
    golden.insert(golden.end(), descriptor.bytes.begin(), descriptor.bytes.end());
    golden.insert(golden.end(), type.bytes.begin(), type.bytes.end());
    golden.insert(golden.end(), 16, 0);
    GoldenText(&golden, "scalar");
    GoldenText(&golden, "int128");
    GoldenText(&golden, "nullability=non_null");
    GoldenText(&golden, "");
    GoldenU64(&golden, 16);
    golden.insert(golden.end(), value.binary_value.begin(), value.binary_value.end());
    golden.push_back(0);
    golden.push_back(0);
    std::vector<std::uint8_t> actual;
    codec::AppendStateValue(&actual, value);
    Require(actual == golden, "spill value encoding differs from independent binary16 oracle");
    std::size_t planned = 0;
    Require(codec::AddSerializedStateValueSize(value, &planned) && planned == actual.size(),
            "spill value byte planning differs from emitted bytes");
    codec::AggregateStateReader reader(actual, actual.size());
    api::EngineTypedValue restored;
    Require(reader.ReadValue(&restored) && reader.Remaining() == 0 &&
            restored.descriptor == value.descriptor &&
            restored.binary_value == value.binary_value &&
            restored.encoded_value.empty() && restored.state == value.state,
            "spill value decode changed typed identity or payload");
    for (std::size_t length = 0; length < actual.size(); ++length) {
      const std::vector<std::uint8_t> truncated(actual.begin(), actual.begin() + length);
      codec::AggregateStateReader short_reader(truncated, actual.size());
      Require(!short_reader.ReadValue(&restored), "truncated spill value was accepted");
    }
    for (const unsigned identity_offset : {0u, 16u}) {
      for (unsigned version = 0; version < 16; ++version) {
        auto mutated = actual;
        mutated[identity_offset + 6] = static_cast<std::uint8_t>(version << 4);
        codec::AggregateStateReader changed(mutated, mutated.size());
        Require(changed.ReadValue(&restored) == (version == 7),
                "spill value accepted a non-v7 system identity");
      }
      for (unsigned variant = 0; variant < 4; ++variant) {
        auto mutated = actual;
        mutated[identity_offset + 8] = static_cast<std::uint8_t>(variant << 6);
        codec::AggregateStateReader changed(mutated, mutated.size());
        Require(changed.ReadValue(&restored) == (variant == 2),
                "spill value accepted a malformed system identity variant");
      }
      auto mutated = actual;
      std::fill_n(mutated.begin() + identity_offset, 16, 0);
      codec::AggregateStateReader changed(mutated, mutated.size());
      Require(!changed.ReadValue(&restored), "spill value accepted a nil system identity");
    }
    // Collation is optional, but a present identity obeys the same binary
    // UUIDv7 policy and participates in exact descriptor comparisons.
    auto collated_value = value;
    collated_value.descriptor.collation_uuid = descriptor;
    std::vector<std::uint8_t> collated_bytes;
    codec::AppendStateValue(&collated_bytes, collated_value);
    auto collated_golden = golden;
    std::copy(descriptor.bytes.begin(), descriptor.bytes.end(), collated_golden.begin() + 32);
    Require(collated_bytes == collated_golden,
            "spill codec did not retain the exact binary collation slot");
    codec::AggregateStateReader collation_reader(collated_bytes, collated_bytes.size());
    Require(collation_reader.ReadValue(&restored) &&
            restored.descriptor == collated_value.descriptor &&
            !codec::DescriptorMatches(value.descriptor, restored.descriptor),
            "spill codec or descriptor comparison lost collation identity");
    for (unsigned version = 0; version < 16; ++version) {
      auto mutated = collated_bytes;
      mutated[38] = static_cast<std::uint8_t>(version << 4);
      codec::AggregateStateReader changed(mutated, mutated.size());
      Require(changed.ReadValue(&restored) == (version == 7),
              "spill codec accepted non-v7 collation authority");
    }
    for (unsigned variant = 0; variant < 4; ++variant) {
      auto mutated = collated_bytes;
      mutated[40] = static_cast<std::uint8_t>(variant << 6);
      codec::AggregateStateReader changed(mutated, mutated.size());
      Require(changed.ReadValue(&restored) == (variant == 2),
              "spill codec accepted malformed collation authority");
    }
    codec::CanonicalAggregateRuntimeRequest request;
    const auto* aggregate = codec::LookupCanonicalAggregateByFunctionV1(
        codec::CanonicalAggregateFunction::array_agg);
    Require(aggregate != nullptr, "array aggregate registry row is absent");
    request.descriptor = {aggregate->abi_version, aggregate->function,
                           aggregate->builtin_id, aggregate->function_uuid, false};
    request.value_columns = {0};
    codec::CanonicalAggregateCoreState state;
    state.transition_count = 1;
    state.non_null_count = 1;
    state.collection_values.push_back(value);
    std::vector<std::uint8_t> encoded;
    std::size_t state_bytes = 0;
    Require(codec::SerializeCanonicalAggregateCoreState(request, state, 65536, &encoded) &&
            codec::PlanCanonicalAggregateCoreStateDeserialization(request, encoded, 65536, &state_bytes),
            "binary aggregate state failed serializer/preflight roundtrip");
    codec::CanonicalAggregateCoreState restored_state;
    std::size_t key_generations = 0;
    std::size_t comparisons = 0;
    codec::DescriptorBatch equality_authority;
    equality_authority.columns.push_back(column);
    const auto deserialize = [&](const std::vector<std::uint8_t>& bytes) {
      return codec::DeserializeCanonicalAggregateCoreState(
          request, equality_authority, bytes, 65536, 65536, 65536, 65536,
          &restored_state, &key_generations, &comparisons);
    };
    Require(deserialize(encoded) && restored_state.transition_count == 1 &&
            restored_state.non_null_count == 1 && restored_state.collection_values.size() == 1 &&
            restored_state.collection_values.front().descriptor == value.descriptor &&
            restored_state.collection_values.front().binary_value == value.binary_value &&
            key_generations == 0 && comparisons == 0,
            "full aggregate-state deserializer changed identities or collection contents");
    for (unsigned identity = 0; identity < 2; ++identity) {
      auto forged_state = state;
      auto& forged = forged_state.collection_values.front().descriptor;
      (identity == 0 ? forged.descriptor_uuid : forged.type_uuid).bytes[15] ^= 0x40;
      std::vector<std::uint8_t> forged_bytes;
      Require(codec::SerializeCanonicalAggregateCoreState(request, forged_state, 65536, &forged_bytes) &&
              !deserialize(forged_bytes),
              "full aggregate-state deserializer admitted substituted descriptor/type authority");
    }
    auto forged_state = state;
    forged_state.collection_values.front().descriptor.collation_uuid = descriptor;
    std::vector<std::uint8_t> forged_bytes;
    Require(codec::SerializeCanonicalAggregateCoreState(request, forged_state, 65536, &forged_bytes) &&
            !deserialize(forged_bytes),
            "full aggregate-state deserializer admitted substituted collation authority");
    forged_state = state;
    forged_state.non_null_count = 0;
    Require(codec::SerializeCanonicalAggregateCoreState(request, forged_state, 65536, &forged_bytes) &&
            !deserialize(forged_bytes),
            "full aggregate-state deserializer trusted a false non-null count");
    forged_state = state;
    forged_state.transition_count = 2;
    Require(codec::SerializeCanonicalAggregateCoreState(request, forged_state, 65536, &forged_bytes) &&
            !deserialize(forged_bytes),
            "full aggregate-state deserializer trusted a false collection count");
    const auto identity = std::search(encoded.begin(), encoded.end(),
                                      aggregate->function_uuid.bytes.begin(),
                                      aggregate->function_uuid.bytes.end());
    Require(identity != encoded.end(), "aggregate state omitted its binary function UUID");
    std::vector<std::uint8_t> prefix;
    GoldenText(&prefix, "scratchbird.aggregate-state.v5");
    GoldenU64(&prefix, aggregate->abi_version);
    GoldenU64(&prefix, static_cast<std::uint8_t>(aggregate->function));
    GoldenText(&prefix, aggregate->builtin_id);
    prefix.insert(prefix.end(), aggregate->function_uuid.bytes.begin(), aggregate->function_uuid.bytes.end());
    Require(encoded.size() >= prefix.size() &&
            std::equal(prefix.begin(), prefix.end(), encoded.begin()),
            "aggregate state prefix differs from binary v5 oracle");
    auto wrong = encoded;
    for (const char legacy_version : {'3', '4'}) {
      wrong[8 + std::string_view("scratchbird.aggregate-state.v").size()] = legacy_version;
      Require(!codec::PlanCanonicalAggregateCoreStateDeserialization(request, wrong, 65536, &state_bytes),
              "old aggregate state version was reinterpreted as binary v5");
      Require(!deserialize(wrong), "full deserializer admitted a legacy state version");
    }
    for (std::size_t length = 0; length < encoded.size(); ++length) {
      wrong.assign(encoded.begin(), encoded.begin() + length);
      Require(!codec::PlanCanonicalAggregateCoreStateDeserialization(request, wrong, 65536, &state_bytes),
              "truncated aggregate state passed preflight");
      Require(!deserialize(wrong), "full deserializer admitted a truncated state");
    }
    wrong = encoded;
    wrong.push_back(0);
    Require(!codec::PlanCanonicalAggregateCoreStateDeserialization(request, wrong, 65536, &state_bytes),
            "aggregate state accepted trailing bytes");
    Require(!deserialize(wrong), "full deserializer admitted trailing bytes");
    Require(!codec::SerializeCanonicalAggregateCoreState(request, state, encoded.size() - 1, &wrong),
            "aggregate state exceeded its serialized byte ceiling");
    std::cout << "PASS actual descriptor identity checks and private aggregate v5 codec/preflight; not execution/recovery\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
