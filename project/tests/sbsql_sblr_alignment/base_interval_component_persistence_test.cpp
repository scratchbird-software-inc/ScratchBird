// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "datatype_interval.hpp"
#include "datatype_interval_projection.hpp"
#include "datatype_physical_encoding.hpp"
#include "disk_device.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dt = scratchbird::core::datatypes;
namespace p = scratchbird::core::platform;
namespace disk = scratchbird::storage::disk;

namespace {
unsigned checks = 0;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "FAIL " << message << '\n';
  std::exit(EXIT_FAILURE);
}
void Check(bool value, std::string_view message) {
  ++checks;
  if (!value) Fail(message);
}
template <typename Result>
void ExpectRefusal(const Result& result, std::string_view code,
                   std::string_view detail, std::string_view label) {
  Check(!result.ok(), std::string(label) + " refused");
  Check(result.diagnostic.diagnostic_code == code,
        std::string(label) + " diagnostic=" +
            std::string(result.diagnostic.diagnostic_code));
  Check(result.diagnostic.detail == detail,
        std::string(label) + " detail=" + std::string(result.diagnostic.detail));
}
template <typename Result>
void ExpectDefaultValue(const Result& result, std::string_view label) {
  Check(result.value.profile == nullptr &&
            result.value.state == dt::IntervalValueStateV3::value &&
            result.value.months == 0 && result.value.civil_days == 0 &&
            result.value.fixed_nanoseconds == 0,
        std::string(label) + " atomic default value");
}
void ExpectDefaultBackup(const dt::IntervalBackupTupleViewResultV3& result,
                         std::string_view label) {
  Check(result.tuple.profile_handle == nullptr &&
            result.tuple.state == dt::IntervalValueStateV3::value &&
            result.tuple.months == 0 && result.tuple.civil_days == 0 &&
            result.tuple.fixed_nanoseconds == 0,
        std::string(label) + " atomic default tuple");
}

p::Uuid D710() {
  return p::Uuid(std::array<p::byte, 16>{
      1, 0x9d, 0, 0, 0, 0, 0x70, 0, 0x80, 0, 0, 0, 0, 0, 0xd7, 0x10});
}
std::shared_ptr<const dt::IntervalValidatedProfileHandleV3> Profile() {
  auto built = dt::BuildCurrentIntervalValidatedProfileHandleV3(D710());
  Check(built.ok(), "build d710 profile");
  return std::make_shared<const dt::IntervalValidatedProfileHandleV3>(
      std::move(built.profile));
}
std::vector<p::byte> Hex(std::string_view text) {
  auto digit = [](char c) -> unsigned {
    return c <= '9' ? static_cast<unsigned>(c - '0')
                    : 10u + static_cast<unsigned>((c | 32) - 'a');
  };
  Check((text.size() & 1u) == 0, "sealed hex has even extent");
  std::vector<p::byte> result(text.size() / 2);
  for (std::size_t i = 0; i < result.size(); ++i)
    result[i] = static_cast<p::byte>((digit(text[2 * i]) << 4) |
                                     digit(text[2 * i + 1]));
  return result;
}
bool Cancel(void*) noexcept { return true; }

dt::IntervalOwnedValueV3 Value(
    const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>& profile,
    dt::IntervalValueStateV3 state, std::int32_t months,
    std::int32_t days, std::int64_t nanoseconds) {
  if (state == dt::IntervalValueStateV3::sql_null)
    return {profile, state, 0, 0, 0};
  auto built = dt::ConstructIntervalV3(profile, months, days, nanoseconds);
  Check(built.ok(), "construct sealed interval value");
  return std::move(built.value);
}

struct FrameVector {
  std::string_view id;
  bool logical;
  dt::IntervalValueStateV3 state;
  std::int32_t months;
  std::int32_t days;
  std::int64_t nanoseconds;
  std::string_view encoded_hex;
};
constexpr std::array<FrameVector, 10> kFrames{{
    {"SBDVAL01_sql_null", true, dt::IntervalValueStateV3::sql_null, 0, 0, 0,
     "53424456414c30319301000001002000000000000000000083039d73b00f6514"},
    {"SBDVAL01_zero", true, dt::IntervalValueStateV3::value, 0, 0, 0,
     "53424456414c303193010000000020001000000000000000432cf11520271ea300000000000000000000000000000000"},
    {"SBDVAL01_mixed", true, dt::IntervalValueStateV3::value, -7, 8, -9,
     "53424456414c303193010000000020001000000000000000592856d2369dd73ef9ffffff08000000f7ffffffffffffff"},
    {"SBDVAL01_cartesian_min", true, dt::IntervalValueStateV3::value,
     std::numeric_limits<std::int32_t>::min(),
     std::numeric_limits<std::int32_t>::min(),
     std::numeric_limits<std::int64_t>::min(),
     "53424456414c303193010000000020001000000000000000c39a4c932fbb46f500000080000000800000000000000080"},
    {"SBDVAL01_cartesian_max", true, dt::IntervalValueStateV3::value,
     std::numeric_limits<std::int32_t>::max(),
     std::numeric_limits<std::int32_t>::max(),
     std::numeric_limits<std::int64_t>::max(),
     "53424456414c303193010000000020001000000000000000b3ec7116c0e410fcffffff7fffffff7fffffffffffffff7f"},
    {"SBDPV001_sql_null", false, dt::IntervalValueStateV3::sql_null, 0, 0, 0,
     "5342445056303031930100000000000000000000461be993"},
    {"SBDPV001_zero", false, dt::IntervalValueStateV3::value, 0, 0, 0,
     "53424450563030319301000001000000100000001985f98400000000000000000000000000000000"},
    {"SBDPV001_mixed", false, dt::IntervalValueStateV3::value, -7, 8, -9,
     "5342445056303031930100000100000010000000f3e20a21f9ffffff08000000f7ffffffffffffff"},
    {"SBDPV001_cartesian_min", false, dt::IntervalValueStateV3::value,
     std::numeric_limits<std::int32_t>::min(),
     std::numeric_limits<std::int32_t>::min(),
     std::numeric_limits<std::int64_t>::min(),
     "534244505630303193010000010000001000000099360c1f00000080000000800000000000000080"},
    {"SBDPV001_cartesian_max", false, dt::IntervalValueStateV3::value,
     std::numeric_limits<std::int32_t>::max(),
     std::numeric_limits<std::int32_t>::max(),
     std::numeric_limits<std::int64_t>::max(),
     "534244505630303193010000010000001000000089c86cd2ffffff7fffffff7fffffffffffffff7f"},
}};

dt::IntervalViewResultV3 DecodeFrame(
    const dt::IntervalValidatedProfileHandleV3& profile, bool logical,
    std::span<const p::byte> encoded,
    const dt::IntervalExecutionControlV3& control = {}) {
  return logical ? dt::DecodeIntervalSbdvalComposedNoAllocV3(
                       profile, true, encoded, control)
                 : dt::DecodeIntervalSbdpvComposedNoAllocV3(
                       profile, true, encoded, control);
}
std::string_view ByteDiagnostic(const FrameVector& frame, std::size_t offset) {
  if (frame.logical) {
    if (offset < 8 || (offset >= 13 && offset < 24))
      return "CTB.BINARY.FRAME_INVALID";
    if (offset < 12) return "CTI.INTERVAL.DESCRIPTOR_INVALID";
    if (offset == 12)
      return frame.state == dt::IntervalValueStateV3::sql_null
                 ? "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID"
                 : "DATATYPE.NULL_STATE.INVALID";
    return "CTB.BINARY.INTEGRITY_FAILED";
  }
  if (offset < 8) return "SB-DATATYPE-PHYSICAL-BAD-MAGIC";
  if (offset < 14 || offset >= 20)
    return "SB-DATATYPE-PHYSICAL-CHECKSUM-MISMATCH";
  if (offset < 16) return "SB-DATATYPE-PHYSICAL-BAD-FLAGS";
  return "SB-DATATYPE-PHYSICAL-LENGTH-MISMATCH";
}
std::string_view MutationDetail(const FrameVector& frame,
                                std::size_t offset) {
  if (!frame.logical) return "sbdpv_structure";
  if (offset >= 8 && offset < 12) return "sbdval_type";
  if (offset == 12 &&
      frame.state == dt::IntervalValueStateV3::sql_null)
    return "component_extent_invalid";
  return "sbdval_structure";
}

void ExactComponentExtentCorpus(
    const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>& profile) {
  std::array<p::byte, 33> bytes{};
  unsigned corpus = 0;
  for (std::size_t extent = 0; extent <= 32; ++extent) {
    auto decoded = dt::DecodeCanonicalIntervalComponentNoAllocV3(
        *profile, dt::IntervalValueStateV3::value, true,
        std::span<const p::byte>(bytes.data(), extent));
    ++corpus;
    if (extent == 16) {
      Check(decoded.ok() && decoded.value.profile == profile.get() &&
                decoded.value.state == dt::IntervalValueStateV3::value &&
                decoded.value.months == 0 && decoded.value.civil_days == 0 &&
                decoded.value.fixed_nanoseconds == 0,
            "component PRESENT exact extent");
    } else {
      ExpectRefusal(decoded, "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                    "component_extent_invalid", "component PRESENT extent");
      ExpectDefaultValue(decoded, "component PRESENT extent");
    }
  }
  for (std::size_t extent = 0; extent <= 32; ++extent) {
    auto decoded = dt::DecodeCanonicalIntervalComponentNoAllocV3(
        *profile, dt::IntervalValueStateV3::sql_null, true,
        std::span<const p::byte>(bytes.data(), extent));
    ++corpus;
    if (extent == 0) {
      Check(decoded.ok() && decoded.value.profile == profile.get() &&
                decoded.value.state == dt::IntervalValueStateV3::sql_null &&
                decoded.value.months == 0 && decoded.value.civil_days == 0 &&
                decoded.value.fixed_nanoseconds == 0,
            "component SQL_NULL zero extent");
    } else {
      ExpectRefusal(decoded, "DATATYPE.NULL_STATE.INVALID",
                    "sql_null_component_nonempty", "component SQL_NULL extent");
      ExpectDefaultValue(decoded, "component SQL_NULL extent");
    }
  }
  Check(corpus == 66, "exact component extent corpus count");
}

void ExactComposedCorpus(
    const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>& profile) {
  unsigned corpus = 0;
  for (const auto& frame : kFrames) {
    const auto sealed = Hex(frame.encoded_hex);
    auto value = Value(profile, frame.state, frame.months, frame.days,
                       frame.nanoseconds);
    const auto encoded = frame.logical
        ? dt::EncodeIntervalSbdvalComposedV3(value, true)
        : dt::EncodeIntervalSbdpvComposedV3(value, true);
    Check(encoded.ok() && encoded.bytes == sealed,
          std::string(frame.id) + " exact sealed encoding");
    auto decoded = DecodeFrame(*profile, frame.logical, sealed);
    Check(decoded.ok() && decoded.value.profile == profile.get() &&
              decoded.value.state == frame.state &&
              decoded.value.months == frame.months &&
              decoded.value.civil_days == frame.days &&
              decoded.value.fixed_nanoseconds == frame.nanoseconds,
          std::string(frame.id) + " exact decode");
    const auto reencoded = frame.logical
        ? dt::EncodeIntervalSbdvalComposedV3(
              {profile, decoded.value.state, decoded.value.months,
               decoded.value.civil_days, decoded.value.fixed_nanoseconds}, true)
        : dt::EncodeIntervalSbdpvComposedV3(
              {profile, decoded.value.state, decoded.value.months,
               decoded.value.civil_days, decoded.value.fixed_nanoseconds}, true);
    Check(reencoded.ok() && reencoded.bytes == sealed,
          std::string(frame.id) + " decode reencode identity");

    for (std::size_t offset = 0; offset < sealed.size(); ++offset) {
      auto mutated = sealed;
      mutated[offset] ^= 1;
      auto refused = DecodeFrame(*profile, frame.logical, mutated);
      ExpectRefusal(refused, ByteDiagnostic(frame, offset),
                    MutationDetail(frame, offset),
                    std::string(frame.id) + " byte " +
                        std::to_string(offset));
      ExpectDefaultValue(refused, frame.id);
      ++corpus;
    }
    for (std::size_t extent = 0; extent < sealed.size(); ++extent) {
      auto refused = DecodeFrame(
          *profile, frame.logical,
          std::span<const p::byte>(sealed.data(), extent));
      const auto code = frame.logical
          ? "CTB.BINARY.FRAME_INVALID"
          : (extent < 24 ? "SB-DATATYPE-PHYSICAL-TRUNCATED"
                         : "SB-DATATYPE-PHYSICAL-LENGTH-MISMATCH");
      ExpectRefusal(refused, code,
                    frame.logical ? "sbdval_structure" : "sbdpv_structure",
                    std::string(frame.id) + " truncate " +
                        std::to_string(extent));
      ExpectDefaultValue(refused, frame.id);
      ++corpus;
    }
    auto trailing = sealed;
    trailing.push_back(0);
    auto refused = DecodeFrame(*profile, frame.logical, trailing);
    ExpectRefusal(refused,
                  frame.logical ? "CTB.BINARY.FRAME_INVALID"
                                : "SB-DATATYPE-PHYSICAL-LENGTH-MISMATCH",
                  frame.logical ? "sbdval_structure" : "sbdpv_structure",
                  std::string(frame.id) + " append zero");
    ExpectDefaultValue(refused, frame.id);
    ++corpus;

    auto bad = *profile;
    bad.receipt.catalog_generation ^= 1;
    refused = DecodeFrame(bad, frame.logical, sealed);
    ExpectRefusal(refused, "CTI.INTERVAL.DESCRIPTOR_INVALID",
                  frame.logical ? "sbdval_profile" : "sbdpv_profile",
                  std::string(frame.id) + " wrong receipt");
    ExpectDefaultValue(refused, frame.id); ++corpus;
    bad = *profile; ++bad.identity.legacy_fields.descriptor_generation;
    refused = DecodeFrame(bad, frame.logical, sealed);
    ExpectRefusal(refused, "CTI.INTERVAL.DESCRIPTOR_INVALID",
                  frame.logical ? "sbdval_profile" : "sbdpv_profile",
                  std::string(frame.id) + " wrong descriptor");
    ExpectDefaultValue(refused, frame.id); ++corpus;
    bad = *profile; ++bad.identity.legacy_fields.codec_version;
    refused = DecodeFrame(bad, frame.logical, sealed);
    ExpectRefusal(refused, "CTI.INTERVAL.DESCRIPTOR_INVALID",
                  frame.logical ? "sbdval_profile" : "sbdpv_profile",
                  std::string(frame.id) + " wrong codec");
    ExpectDefaultValue(refused, frame.id); ++corpus;
    bad = *profile; bad.profile_fingerprint[0] ^= 1;
    refused = DecodeFrame(bad, frame.logical, sealed);
    ExpectRefusal(refused, "CTI.INTERVAL.DESCRIPTOR_INVALID",
                  frame.logical ? "sbdval_profile" : "sbdpv_profile",
                  std::string(frame.id) + " wrong profile");
    ExpectDefaultValue(refused, frame.id); ++corpus;
    bad = *profile; ++bad.subtype_policy.generation;
    refused = DecodeFrame(bad, frame.logical, sealed);
    ExpectRefusal(refused, "CTI.INTERVAL.DESCRIPTOR_INVALID",
                  frame.logical ? "sbdval_profile" : "sbdpv_profile",
                  std::string(frame.id) + " wrong policy");
    ExpectDefaultValue(refused, frame.id); ++corpus;

    dt::IntervalExecutionControlV3 control;
    control.force_reencode_mismatch_for_conformance = true;
    refused = DecodeFrame(*profile, frame.logical, sealed, control);
    ExpectRefusal(refused, "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                  frame.logical ? "sbdval_reencode" : "sbdpv_reencode",
                  std::string(frame.id) + " forced reencode mismatch");
    ExpectDefaultValue(refused, frame.id); ++corpus;
    control = {}; control.maximum_allocation_bytes = sealed.size() - 1;
    refused = DecodeFrame(*profile, frame.logical, sealed, control);
    ExpectRefusal(refused, "RESOURCE.BUDGET_EXCEEDED",
                  frame.logical ? "sbdval_decode_budget"
                                : "sbdpv_decode_budget",
                  std::string(frame.id) + " one-byte-short resource");
    ExpectDefaultValue(refused, frame.id); ++corpus;
    control = {}; control.cancelled = Cancel;
    refused = DecodeFrame(*profile, frame.logical, sealed, control);
    ExpectRefusal(refused, "PROCESS.CANCELLED",
                  "before_publication",
                  std::string(frame.id) + " cancel before publication");
    ExpectDefaultValue(refused, frame.id); ++corpus;
  }
  Check(corpus == 906, "exact composed mutation corpus count");
}

struct BackupSemanticVector {
  std::string_view id, encoded_hex, diagnostic, detail;
};
constexpr std::array<BackupSemanticVector, 6> kBackupSemantic{{
    {"wrong_receipt_recomputed_integrity", "5342494e424b30310100680178010000009d000000007000800000000000d7100a000000000000000a0000000000000093010000696e7465b276616c000000000100000000000000019d000000007000800000000000d8220100000000000000019d000000007000800000000000d82301000000000000000100000000000000db1e0236a8dd8a04561a57868ccd1ab8bad508f3c1c751aa5d6ba8b5b42823eb01a10510696e70008000000000000008010000000000000001a10510696e7000800000000000000a010000000000000001a10510696e7000800000000000000b010000000000000001a10510696e7000800000000000000c010000000000000001a10510696e7000800000000000000f010000000000000001a10510696e7000800000000000001001000000000000000110000000000000000000000000000000000000000000006a331f86543195ca0108b5ffe75fa5ac7896358ac6420981efeb74fa249007dff9ffffff08000000f7ffffffffffffff", "CTI.INTERVAL.DESCRIPTOR_INVALID", "backup_identity"},
    {"wrong_descriptor_recomputed_integrity", "5342494e424b30310100680178010000019d000000007000800000000000d7100a000000000000000a0000000000000092010000696e7465b276616c000000000100000000000000019d000000007000800000000000d8220100000000000000019d000000007000800000000000d82301000000000000000100000000000000db1e0236a8dd8a04561a57868ccd1ab8bad508f3c1c751aa5d6ba8b5b42823eb01a10510696e70008000000000000008010000000000000001a10510696e7000800000000000000a010000000000000001a10510696e7000800000000000000b010000000000000001a10510696e7000800000000000000c010000000000000001a10510696e7000800000000000000f010000000000000001a10510696e7000800000000000001001000000000000000110000000000000000000000000000000000000000000009d509ba11615d17ab13a2b81266bd48ab28e7ef4c70633f30706b5565b2eecc1f9ffffff08000000f7ffffffffffffff", "CTI.INTERVAL.DESCRIPTOR_INVALID", "backup_identity"},
    {"wrong_policy_recomputed_integrity", "5342494e424b30310100680178010000019d000000007000800000000000d7100a000000000000000a0000000000000093010000696e7465b276616c000000000100000000000000019d000000007000800000000000d8220100000000000000019d000000007000800000000000d82301000000000000000100000000000000db1e0236a8dd8a04561a57868ccd1ab8bad508f3c1c751aa5d6ba8b5b42823eb00a10510696e70008000000000000008010000000000000001a10510696e7000800000000000000a010000000000000001a10510696e7000800000000000000b010000000000000001a10510696e7000800000000000000c010000000000000001a10510696e7000800000000000000f010000000000000001a10510696e7000800000000000001001000000000000000110000000000000000000000000000000000000000000002dbd0b8798284b6199d04d89d52b5b9761819b4e0908e42ba4da82f8648c5038f9ffffff08000000f7ffffffffffffff", "CTI.INTERVAL.DESCRIPTOR_INVALID", "backup_identity"},
    {"unknown_state_recomputed_integrity", "5342494e424b30310100680178010000019d000000007000800000000000d7100a000000000000000a0000000000000093010000696e7465b276616c000000000100000000000000019d000000007000800000000000d8220100000000000000019d000000007000800000000000d82301000000000000000100000000000000db1e0236a8dd8a04561a57868ccd1ab8bad508f3c1c751aa5d6ba8b5b42823eb01a10510696e70008000000000000008010000000000000001a10510696e7000800000000000000a010000000000000001a10510696e7000800000000000000b010000000000000001a10510696e7000800000000000000c010000000000000001a10510696e7000800000000000000f010000000000000001a10510696e70008000000000000010010000000000000002100000000000000000000000000000000000000000000003d11462f04929fd06a36c10573f5491cfb9f2097988956b92d75dc124d636a7f9ffffff08000000f7ffffffffffffff", "DATATYPE.NULL_STATE.INVALID", "backup_state"},
    {"wrong_component_extent_recomputed_integrity", "5342494e424b30310100680178010000019d000000007000800000000000d7100a000000000000000a0000000000000093010000696e7465b276616c000000000100000000000000019d000000007000800000000000d8220100000000000000019d000000007000800000000000d82301000000000000000100000000000000db1e0236a8dd8a04561a57868ccd1ab8bad508f3c1c751aa5d6ba8b5b42823eb01a10510696e70008000000000000008010000000000000001a10510696e7000800000000000000a010000000000000001a10510696e7000800000000000000b010000000000000001a10510696e7000800000000000000c010000000000000001a10510696e7000800000000000000f010000000000000001a10510696e700080000000000000100100000000000000010f00000000000000000000000000000000000000000000c2aa9d891808b4043b53eb6dcbe9407deb1f65a257524d1015fb92cdbd3465a9f9ffffff08000000f7ffffffffffffff", "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "backup_value_pair"},
    {"reserved_nonzero_recomputed_integrity", "5342494e424b30310100680178010000019d000000007000800000000000d7100a000000000000000a0000000000000093010000696e7465b276616c000000000100000000000000019d000000007000800000000000d8220100000000000000019d000000007000800000000000d82301000000000000000100000000000000db1e0236a8dd8a04561a57868ccd1ab8bad508f3c1c751aa5d6ba8b5b42823eb01a10510696e70008000000000000008010000000000000001a10510696e7000800000000000000a010000000000000001a10510696e7000800000000000000b010000000000000001a10510696e7000800000000000000c010000000000000001a10510696e7000800000000000000f010000000000000001a10510696e7000800000000000001001000000000000000110010000000000000000000000000000000000000000001cc0b6a64ecbf1d45b46021a609f898c8bbe90f66dc4154405213a199e7f9870f9ffffff08000000f7ffffffffffffff", "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "backup_reserved"},
}};

void ExactBackupCorpus(
    const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>& profile) {
  auto present_value = Value(profile, dt::IntervalValueStateV3::value, -7, 8, -9);
  auto null_value = Value(profile, dt::IntervalValueStateV3::sql_null, 0, 0, 0);
  const auto encoded_present = dt::EncodeIntervalBackupTupleV3(present_value);
  const auto encoded_null = dt::EncodeIntervalBackupTupleV3(null_value);
  Check(encoded_present.ok() && encoded_null.ok(), "backup sealed encode");
  constexpr std::array<std::string_view, 2> sealed_hex{{
      "5342494e424b30310100680178010000019d000000007000800000000000d7100a000000000000000a0000000000000093010000696e7465b276616c000000000100000000000000019d000000007000800000000000d8220100000000000000019d000000007000800000000000d82301000000000000000100000000000000db1e0236a8dd8a04561a57868ccd1ab8bad508f3c1c751aa5d6ba8b5b42823eb01a10510696e70008000000000000008010000000000000001a10510696e7000800000000000000a010000000000000001a10510696e7000800000000000000b010000000000000001a10510696e7000800000000000000c010000000000000001a10510696e7000800000000000000f010000000000000001a10510696e700080000000000000100100000000000000011000000000000000000000000000000000000000000000e7590bbed7bf5f559a63db531a2bafa6f25bc39689865dd6bbd1662b57b8098df9ffffff08000000f7ffffffffffffff", "5342494e424b30310100680168010000019d000000007000800000000000d7100a000000000000000a0000000000000093010000696e7465b276616c000000000100000000000000019d000000007000800000000000d8220100000000000000019d000000007000800000000000d82301000000000000000100000000000000db1e0236a8dd8a04561a57868ccd1ab8bad508f3c1c751aa5d6ba8b5b42823eb01a10510696e70008000000000000008010000000000000001a10510696e7000800000000000000a010000000000000001a10510696e7000800000000000000b010000000000000001a10510696e7000800000000000000c010000000000000001a10510696e7000800000000000000f010000000000000001a10510696e700080000000000000100100000000000000000000000000000000000000000000000000000000000000f0678d83d703e2bedafe38d6e32a44766ae09aa0f8cd2ba2777303a8d7af9a13"}};
  std::array<std::vector<p::byte>, 2> sealed{{Hex(sealed_hex[0]), Hex(sealed_hex[1])}};
  Check(encoded_present.bytes == sealed[0] && encoded_null.bytes == sealed[1],
        "backup exact sealed baselines");

  unsigned byte_cases = 0, extent_cases = 0;
  unsigned canonical_bytes = 0, checksum_bytes = 0;
  for (std::size_t baseline = 0; baseline < sealed.size(); ++baseline) {
    const bool is_null = baseline == 1;
    const auto& bytes = sealed[baseline];
    auto decoded = dt::DecodeIntervalBackupTupleNoAllocV3(*profile, true, bytes);
    Check(decoded.ok() && decoded.tuple.profile_handle == profile.get() &&
              decoded.tuple.state == (is_null ? dt::IntervalValueStateV3::sql_null
                                              : dt::IntervalValueStateV3::value) &&
              decoded.tuple.months == (is_null ? 0 : -7) &&
              decoded.tuple.civil_days == (is_null ? 0 : 8) &&
              decoded.tuple.fixed_nanoseconds == (is_null ? 0 : -9),
          "backup exact baseline decode");
    auto reencoded = dt::EncodeIntervalBackupTupleV3(
        {profile, decoded.tuple.state, decoded.tuple.months,
         decoded.tuple.civil_days, decoded.tuple.fixed_nanoseconds});
    Check(reencoded.ok() && reencoded.bytes == bytes,
          "backup decode reencode identity");

    for (std::size_t offset = 0; offset < bytes.size(); ++offset) {
      auto mutated = bytes; mutated[offset] ^= 1;
      const bool canonical = offset < 16 || (offset >= 116 && offset < 120) ||
                             (offset >= 306 && offset < 328);
      const auto code = canonical
          ? "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID"
          : "SBLR.ENVELOPE.CHECKSUM_INVALID";
      const auto detail = offset < 16 ? "backup_structure"
                          : (offset >= 116 && offset < 120)
                                ? "backup_reserved"
                          : (offset >= 306 && offset < 328)
                                ? "backup_reserved" : "backup_integrity";
      auto refused = dt::DecodeIntervalBackupTupleNoAllocV3(
          *profile, true, mutated);
      ExpectRefusal(refused, code, detail,
                    std::string("backup byte ") + std::to_string(offset));
      ExpectDefaultBackup(refused, "backup byte mutation");
      ++byte_cases;
      canonical ? ++canonical_bytes : ++checksum_bytes;
    }
    for (std::size_t extent = 0; extent < bytes.size(); ++extent) {
      auto refused = dt::DecodeIntervalBackupTupleNoAllocV3(
          *profile, true,
          std::span<const p::byte>(bytes.data(), extent));
      ExpectRefusal(refused, "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                    "backup_structure", "backup truncation");
      ExpectDefaultBackup(refused, "backup truncation");
      ++extent_cases;
    }
    auto trailing = bytes; trailing.push_back(0);
    auto refused = dt::DecodeIntervalBackupTupleNoAllocV3(
        *profile, true, trailing);
    ExpectRefusal(refused, "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                  "backup_structure", "backup trailing byte");
    ExpectDefaultBackup(refused, "backup trailing byte");
    ++extent_cases;
  }
  Check(byte_cases == 736 && canonical_bytes == 84 && checksum_bytes == 652,
        "backup exact 736 byte diagnostic split 84/652");
  Check(extent_cases == 738, "backup exact 738 extent mutations");

  for (const auto& vector : kBackupSemantic) {
    auto refused = dt::DecodeIntervalBackupTupleNoAllocV3(
        *profile, true, Hex(vector.encoded_hex));
    ExpectRefusal(refused, vector.diagnostic, vector.detail, vector.id);
    ExpectDefaultBackup(refused, vector.id);
  }

  dt::IntervalExecutionControlV3 control;
  control.maximum_allocation_bytes = sealed[0].size() - 1;
  auto refused = dt::DecodeIntervalBackupTupleNoAllocV3(
      *profile, true, sealed[0], control);
  ExpectRefusal(refused, "RESOURCE.BUDGET_EXCEEDED", "backup_extent_budget",
                "backup resource precedence");
  ExpectDefaultBackup(refused, "backup resource precedence");
  control = {}; control.cancelled = Cancel;
  refused = dt::DecodeIntervalBackupTupleNoAllocV3(
      *profile, true, sealed[0], control);
  ExpectRefusal(refused, "PROCESS.CANCELLED", "backup_before_publication",
                "backup cancellation precedence");
  ExpectDefaultBackup(refused, "backup cancellation precedence");
  control = {}; control.force_reencode_mismatch_for_conformance = true;
  refused = dt::DecodeIntervalBackupTupleNoAllocV3(
      *profile, true, sealed[0], control);
  ExpectRefusal(refused, "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                "component_reencode", "backup forced reencode mismatch");
  ExpectDefaultBackup(refused, "backup forced reencode mismatch");
  refused = dt::DecodeIntervalBackupTupleNoAllocV3(
      *profile, false, sealed[1]);
  ExpectRefusal(refused, "DATATYPE.NULL_NOT_ADMITTED",
                "backup_null_not_admitted", "backup NULL not admitted");
  ExpectDefaultBackup(refused, "backup NULL not admitted");

  control = {}; control.maximum_allocation_bytes = sealed[0].size() - 1;
  auto encode_refused = dt::EncodeIntervalBackupTupleV3(present_value, control);
  ExpectRefusal(encode_refused, "RESOURCE.BUDGET_EXCEEDED",
                "backup_allocation", "backup encode resource");
  Check(encode_refused.bytes.empty(), "backup encode resource atomic bytes");
  control = {}; control.cancelled = Cancel;
  encode_refused = dt::EncodeIntervalBackupTupleV3(present_value, control);
  ExpectRefusal(encode_refused, "PROCESS.CANCELLED",
                "backup_before_allocation", "backup encode cancellation");
  Check(encode_refused.bytes.empty(), "backup encode cancellation atomic bytes");
}

struct PersistenceVector {
  std::string_view id;
  bool is_null;
  std::int32_t months, days;
  std::int64_t nanoseconds;
  std::string_view persisted_hex;
};
constexpr std::array<PersistenceVector, 16> kPersistence{{
    {"zero", false, 0, 0, 0, "53424450563030319301000001000000100000001985f98400000000000000000000000000000000"},
    {"months_min", false, -2147483648, 0, 0, "534244505630303193010000010000001000000099f3aafa00000080000000000000000000000000"},
    {"months_max", false, 2147483647, 0, 0, "53424450563030319301000001000000100000007542c04effffff7f000000000000000000000000"},
    {"days_min", false, 0, -2147483648, 0, "5342445056303031930100000100000010000000990b99e000000000000000800000000000000000"},
    {"days_max", false, 0, 2147483647, 0, "5342445056303031930100000100000010000000b5e4516c00000000ffffff7f0000000000000000"},
    {"nanoseconds_min", false, 0, 0,
     std::numeric_limits<std::int64_t>::min(), "5342445056303031930100000100000010000000994efa0400000000000000000000000000000080"},
    {"nanoseconds_max", false, 0, 0, 9223372036854775807, "534244505630303193010000010000001000000091bddb170000000000000000ffffffffffffff7f"},
    {"cartesian_min", false, std::numeric_limits<std::int32_t>::min(),
     std::numeric_limits<std::int32_t>::min(),
     std::numeric_limits<std::int64_t>::min(), "534244505630303193010000010000001000000099360c1f00000080000000800000000000000080"},
    {"cartesian_max", false, 2147483647, 2147483647, 9223372036854775807, "534244505630303193010000010000001000000089c86cd2ffffff7fffffff7fffffffffffffff7f"},
    {"mixed_pos_neg_pos", false, 17, -23, 86400000000000, "5342445056303031930100000100000010000000dcbeada111000000e9ffffff00004f91944e0000"},
    {"mixed_neg_pos_neg", false, -17, 23, -86400000000000, "5342445056303031930100000100000010000000af0ce2f2efffffff170000000000b16e6bb1ffff"},
    {"no_ns_day_carry", false, 0, 0, 86400000000000, "5342445056303031930100000100000010000000eb3a3812000000000000000000004f91944e0000"},
    {"no_day_month_carry", false, 0, 31, 0, "534244505630303193010000010000001000000096371b43000000001f0000000000000000000000"},
    {"subtype_not_inferred_zero_months", false, 0, 9, 10, "5342445056303031930100000100000010000000aa4ad7ba00000000090000000a00000000000000"},
    {"subtype_not_inferred_zero_days_ns", false, 7, 0, 0, "53424450563030319301000001000000100000005e10c38a07000000000000000000000000000000"},
    {"sql_null", true, 0, 0, 0, "5342445056303031930100000000000000000000461be993"},
}};

struct ArtifactGuard {
  std::filesystem::path path;
  ~ArtifactGuard() { std::error_code ignored; std::filesystem::remove(path, ignored); }
};

std::uint32_t PhysicalChecksum(dt::CanonicalTypeId type,
                               dt::DatatypePhysicalValueState state,
                               std::span<const p::byte> payload) {
  std::uint32_t value = 2166136261u;
  auto mix = [&value](std::uint32_t next) {
    value ^= next; value *= 16777619u;
  };
  mix(static_cast<std::uint32_t>(type));
  mix(static_cast<std::uint32_t>(state));
  for (const auto byte : payload) mix(byte);
  return value;
}
void StoreLittle32(p::byte* out, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    out[i] = static_cast<p::byte>(value >> (8 * i));
}
std::vector<p::byte> WrongTypeFrame(std::span<const p::byte> component) {
  std::vector<p::byte> frame(24 + component.size());
  const dt::DatatypePhysicalValueView view{
      dt::CanonicalTypeId::date, dt::DatatypePhysicalValueState::value,
      component.data(), component.size()};
  auto encoded = dt::EncodeDatatypePhysicalStructuralValueIntoNoAlloc(
      view, frame.data(), frame.size());
  Check(encoded.ok() && encoded.bytes_written == frame.size(),
        "build wrong-type physical frame");
  return frame;
}

void ExactPersistenceCorpus(
    const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>& profile) {
  const auto path = std::filesystem::temp_directory_path() /
      "scratchbird_base_interval_d710_component_exact.bin";
  ArtifactGuard cleanup{path};
  std::error_code ignored; std::filesystem::remove(path, ignored);

  for (const auto& vector : kPersistence) {
    const auto state = vector.is_null ? dt::IntervalValueStateV3::sql_null
                                     : dt::IntervalValueStateV3::value;
    auto value = Value(profile, state, vector.months, vector.days,
                       vector.nanoseconds);
    auto encoded = dt::EncodeIntervalSbdpvComposedV3(value, true);
    const auto sealed = Hex(vector.persisted_hex);
    Check(encoded.ok() && encoded.bytes == sealed,
          std::string(vector.id) + " persistence sealed encode");
    if (!vector.is_null) {
      Check(encoded.bytes.size() == 40,
            std::string(vector.id) + " PRESENT physical extent");
    } else {
      Check(encoded.bytes.size() == 24,
            std::string(vector.id) + " NULL physical extent");
    }

    std::filesystem::remove(path, ignored);
    disk::FileDevice writer;
    Check(writer.Open(path.string(), disk::FileOpenMode::create_new).ok(),
          std::string(vector.id) + " storage open writer");
    auto wrote = writer.WriteAt(0, encoded.bytes.data(), encoded.bytes.size());
    Check(wrote.ok() && wrote.bytes_transferred == encoded.bytes.size() &&
              writer.Sync().ok() && writer.Close().ok(),
          std::string(vector.id) + " storage write sync close");
    disk::FileDevice reader;
    Check(reader.Open(path.string(),
                      disk::FileOpenMode::open_existing_read_only).ok(),
          std::string(vector.id) + " storage reopen independent handle");
    std::vector<p::byte> reopened(encoded.bytes.size(), 0xa5);
    auto read = reader.ReadAt(0, reopened.data(), reopened.size());
    Check(read.ok() && read.bytes_transferred == reopened.size() &&
              reader.Close().ok() && reopened == sealed,
          std::string(vector.id) + " storage exact read");
    auto decoded = dt::DecodeIntervalSbdpvComposedNoAllocV3(
        *profile, true, reopened);
    Check(decoded.ok() && decoded.value.profile == profile.get() &&
              decoded.value.state == state &&
              decoded.value.months == vector.months &&
              decoded.value.civil_days == vector.days &&
              decoded.value.fixed_nanoseconds == vector.nanoseconds,
          std::string(vector.id) + " storage exact decode");
    auto reencoded = dt::EncodeIntervalSbdpvComposedV3(
        {profile, decoded.value.state, decoded.value.months,
         decoded.value.civil_days, decoded.value.fixed_nanoseconds}, true);
    Check(reencoded.ok() && reencoded.bytes == reopened,
          std::string(vector.id) + " storage byte-identical reencode");
  }

  const auto present = Hex(kPersistence[0].persisted_hex);
  auto expect_storage_refusal = [&](const dt::IntervalValidatedProfileHandleV3& p,
                                    std::span<const p::byte> bytes,
                                    std::string_view code,
                                    std::string_view detail,
                                    std::string_view id,
                                    const dt::IntervalExecutionControlV3& control = {}) {
    auto refused = dt::DecodeIntervalSbdpvComposedNoAllocV3(
        p, true, bytes, control);
    ExpectRefusal(refused, code, detail, id);
    ExpectDefaultValue(refused, id);
  };
  auto bad = *profile; bad.receipt.registry_generation ^= 1;
  expect_storage_refusal(bad, present, "CTI.INTERVAL.DESCRIPTOR_INVALID",
                         "sbdpv_profile", "storage wrong receipt");
  bad = *profile; ++bad.identity.legacy_fields.descriptor_generation;
  expect_storage_refusal(bad, present, "CTI.INTERVAL.DESCRIPTOR_INVALID",
                         "sbdpv_profile", "storage wrong descriptor");
  auto wrong_type = WrongTypeFrame(
      std::span<const p::byte>(present.data() + 24, 16));
  expect_storage_refusal(*profile, wrong_type,
                         "CTI.INTERVAL.DESCRIPTOR_INVALID", "sbdpv_type",
                         "storage wrong type");
  bad = *profile; ++bad.identity.legacy_fields.codec_generation;
  expect_storage_refusal(bad, present, "CTI.INTERVAL.DESCRIPTOR_INVALID",
                         "sbdpv_profile", "storage wrong codec");
  expect_storage_refusal(*profile,
                         std::span<const p::byte>(present.data(), present.size() - 1),
                         "SB-DATATYPE-PHYSICAL-LENGTH-MISMATCH",
                         "sbdpv_structure", "storage short component");
  auto trailing = present; trailing.push_back(0);
  expect_storage_refusal(*profile, trailing,
                         "SB-DATATYPE-PHYSICAL-LENGTH-MISMATCH",
                         "sbdpv_structure", "storage trailing component");
  auto dirty_null = present;
  dirty_null[12] = 0; dirty_null[13] = 0;
  const auto checksum = PhysicalChecksum(
      dt::CanonicalTypeId::interval, dt::DatatypePhysicalValueState::sql_null,
      std::span<const p::byte>(dirty_null.data() + 24, 16));
  StoreLittle32(dirty_null.data() + 20, checksum);
  expect_storage_refusal(*profile, dirty_null, "DATATYPE.NULL_STATE.INVALID",
                         "sbdpv_structure", "storage dirty NULL");
  bad = *profile; bad.profile_material[0] ^= 1;
  expect_storage_refusal(bad, present, "CTI.INTERVAL.DESCRIPTOR_INVALID",
                         "sbdpv_profile", "storage profile mismatch");
  auto corrupt = present; corrupt[0] ^= 1;
  expect_storage_refusal(*profile, corrupt,
                         "SB-DATATYPE-PHYSICAL-BAD-MAGIC",
                         "sbdpv_structure", "storage corrupt magic");
  corrupt = present; corrupt[24] ^= 1;
  expect_storage_refusal(*profile, corrupt,
                         "SB-DATATYPE-PHYSICAL-CHECKSUM-MISMATCH",
                         "sbdpv_structure", "storage corrupt checksum");

  const auto sentinel = Hex("a55aa55aa55aa55a");
  std::filesystem::remove(path, ignored);
  disk::FileDevice writer;
  Check(writer.Open(path.string(), disk::FileOpenMode::create_new).ok(),
        "storage cancellation sentinel open");
  auto wrote = writer.WriteAt(0, sentinel.data(), sentinel.size());
  Check(wrote.ok() && wrote.bytes_transferred == sentinel.size() &&
            writer.Sync().ok() && writer.Close().ok(),
        "storage cancellation sentinel write");
  dt::IntervalExecutionControlV3 control; control.cancelled = Cancel;
  Check(control.cancelled(control.cancellation_context),
        "storage cancel before write gate");
  disk::FileDevice reader;
  Check(reader.Open(path.string(), disk::FileOpenMode::open_existing_read_only).ok(),
        "storage cancel before write reopen");
  std::vector<p::byte> unchanged(sentinel.size());
  auto read = reader.ReadAt(0, unchanged.data(), unchanged.size());
  Check(read.ok() && read.bytes_transferred == unchanged.size() &&
            reader.Close().ok() && unchanged == sentinel,
        "storage cancel before write destination unchanged");
  expect_storage_refusal(*profile, present, "PROCESS.CANCELLED",
                         "before_publication",
                         "storage cancel before publication", control);
}

void BatchBoundaries(
    const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>& profile) {
  for (p::u64 rows : {0ull, 1ull, 7ull, 8ull, 9ull, 1024ull, 65536ull}) {
    const p::u64 exact = rows * 16 + (rows + 7) / 8;
    auto extent = dt::ComputeIntervalBatchExtentsV3(rows, exact);
    Check(extent.ok() && extent.combined_bytes == exact,
          "batch exact extent formula");
    if (exact)
      Check(!dt::ComputeIntervalBatchExtentsV3(rows, exact - 1).ok(),
            "batch one byte short");
    std::vector<std::int32_t> months(rows, 1), days(rows, -2);
    std::vector<std::int64_t> nanoseconds(rows, 3);
    std::vector<p::byte> bitmap((rows + 7) / 8, 0);
    dt::IntervalExecutionControlV3 control;
    control.maximum_allocation_bytes = exact;
    auto materialized = dt::MaterializeIntervalBatchV3(
        profile, months, days, nanoseconds, bitmap, control);
    Check(materialized.ok() && materialized.batch.months.size() == rows &&
              materialized.batch.profile == profile,
          "batch exact materialization");
    if (exact) {
      control.maximum_allocation_bytes = exact - 1;
      auto refused = dt::MaterializeIntervalBatchV3(
          profile, months, days, nanoseconds, bitmap, control);
      Check(!refused.ok() && refused.batch.months.empty() &&
                refused.batch.profile == nullptr,
            "batch one-short atomic refusal");
    }
  }
}
}  // namespace

int main() {
  const auto profile = Profile();
  ExactComponentExtentCorpus(profile);
  ExactComposedCorpus(profile);
  ExactBackupCorpus(profile);
  ExactPersistenceCorpus(profile);
  BatchBoundaries(profile);
  std::cout << "PASS checks=" << checks
            << " component_extent_cases=66 composed_mutations=906"
               " backup_byte_mutations=736 backup_split=84/652"
               " backup_extent_mutations=738"
               " backup_semantic_mutations=6 persistence_positive=16"
               " persistence_negative=12\n";
}
