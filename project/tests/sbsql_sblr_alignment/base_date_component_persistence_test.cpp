// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "datatype_date.hpp"
#include "datatype_physical_encoding.hpp"
#include "disk_device.hpp"
#include "hash_digest_parts.hpp"
#include "../support/owned_temp_directory.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace dt = scratchbird::core::datatypes;
namespace platform = scratchbird::core::platform;
namespace core_hash = scratchbird::core::hash;
namespace disk = scratchbird::storage::disk;
namespace fs = std::filesystem;

namespace {

constexpr std::size_t kOracleHeaderBytes = 200;
constexpr std::size_t kOracleMaximumFrameBytes = 36;
constexpr std::size_t kOracleMaximumBytes = 236;
constexpr std::string_view kOracleEvidenceDomain =
    "ScratchBird.BaseDate.TestOracle.V1";

void Require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void TestFixtureIsolationAndFailureCleanup() {
  fs::path failed_path;
  scratchbird::tests::OwnedTempDirectory sibling;
  try {
    scratchbird::tests::OwnedTempDirectory directory;
    failed_path = directory.path();
    Require(failed_path != sibling.path(), "fixture paths must be unique");
    disk::FileDevice writer;
    Require(writer.Open((failed_path / "failure-image").string(),
                        disk::FileOpenMode::create_new).ok(),
            "failure cleanup fixture open");
    throw 7;  // Device destructor must release its lease before directory cleanup.
  } catch (int marker) {
    Require(marker == 7, "unexpected cleanup injection");
  }
  Require(!failed_path.empty() && !fs::exists(failed_path),
          "exception cleanup removes image and ownership sidecars");
  Require(fs::is_directory(sibling.path()), "cleanup preserves sibling fixture");
  const auto sibling_path = sibling.path();
  sibling.Cleanup();
  Require(!fs::exists(sibling_path), "explicit cleanup removes owned directory");
}

platform::Uuid StatementReceipt() {
  platform::Uuid value{};
  value.bytes[0] = 1;
  value.bytes[1] = 0x9d;
  value.bytes[6] = 0x70;
  value.bytes[8] = 0x80;
  value.bytes[14] = 0xd7;
  value.bytes[15] = 0x10;
  return value;
}

dt::DateValidatedProfileHandleV3 Profile() {
  auto result = dt::BuildCurrentDateValidatedProfileHandleV3(StatementReceipt());
  Require(result.ok(), "current date profile");
  return result.profile;
}

std::vector<platform::byte> Hex(std::string_view text) {
  Require((text.size() & 1U) == 0, "odd hexadecimal fixture extent");
  std::vector<platform::byte> bytes;
  bytes.reserve(text.size() / 2);
  for (std::size_t offset = 0; offset < text.size(); offset += 2) {
    const auto nibble = [](char value) -> platform::byte {
      if (value >= '0' && value <= '9')
        return static_cast<platform::byte>(value - '0');
      if (value >= 'a' && value <= 'f')
        return static_cast<platform::byte>(value - 'a' + 10);
      throw std::runtime_error("invalid hexadecimal fixture");
    };
    bytes.push_back(static_cast<platform::byte>(
        (nibble(text[offset]) << 4) | nibble(text[offset + 1])));
  }
  return bytes;
}

struct ComponentVector {
  dt::DateValueStateV3 state;
  std::int32_t day;
  std::string_view sbdval;
  std::string_view sbdpv;
};

constexpr std::array<ComponentVector, 4> kComponentVectors{{
    {dt::DateValueStateV3::value, 0,
     "53424456414c3031900100000000200004000000000000003331a286a046543100000000",
     "5342445056303031900100000100000004000000eac93b7d00000000"},
    {dt::DateValueStateV3::value, std::numeric_limits<std::int32_t>::min(),
     "53424456414c303190010000000020000400000000000000b357a186a0c6533100000080",
     "53424450563030319001000001000000040000006a003bfd00000080"},
    {dt::DateValueStateV3::value, std::numeric_limits<std::int32_t>::max(),
     "53424456414c3031900100000000200004000000000000000f2d562864985844ffffff7f",
     "534244505630303190010000010000000400000006a24d95ffffff7f"},
    {dt::DateValueStateV3::sql_null, 0,
     "53424456414c30319001000001002000000000000000000083039d73b00f6514",
     "5342445056303031900100000000000000000000dda0e66d"},
}};

struct OraclePositiveVector {
  std::string_view name;
  dt::DateValueStateV3 state;
  std::int32_t day;
  std::string_view frame_hex;
  std::string_view record_hex;
};

constexpr std::array<OraclePositiveVector, 4> kOraclePositiveVectors{{
    {"epoch", dt::DateValueStateV3::value, 0,
     "53424456414c3031900100000000200004000000000000003331a286a046543100000000",
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae55240000000000000073423ef305d7e90ce03099f6182ce6a1bbfa0947880ad223654f5aa58c3dea4053424456414c3031900100000000200004000000000000003331a286a046543100000000"},
    {"maximum", dt::DateValueStateV3::value, 2147483647,
     "53424456414c3031900100000000200004000000000000000f2d562864985844ffffff7f",
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae55240000000000000065de4ea249f740b79a03114ed93250f808e995a9f9c16700f810a19c74ffa3d753424456414c3031900100000000200004000000000000000f2d562864985844ffffff7f"},
    {"minimum", dt::DateValueStateV3::value, -2147483648,
     "53424456414c303190010000000020000400000000000000b357a186a0c6533100000080",
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae5524000000000000003b2de9df0ae07514eb2bd0c24ba98a322dbb62f7292ca128398a9f8da49c62c353424456414c303190010000000020000400000000000000b357a186a0c6533100000080"},
    {"sql_null", dt::DateValueStateV3::sql_null, 0,
     "53424456414c30319001000001002000000000000000000083039d73b00f6514",
     "534244544f5230310100c80001000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae5520000000000000008aa076a5fcfd7cf78e7c39bc6b70dd06c0fab26917c15d8a9f1e286c63d07a2e53424456414c30319001000001002000000000000000000083039d73b00f6514"},
}};

constexpr std::array<std::string_view, 4> kOraclePositiveNames{{
    "epoch", "maximum", "minimum", "sql_null"}};

enum class OutcomeScope { harness_only, nested_public };

struct OracleMutation {
  std::string_view name;
  std::string_view gate;
  std::string_view expected;
  OutcomeScope scope;
  std::string_view public_diagnostic_code;
  std::string_view public_diagnostic_uuid;
  bool null_allowed;
  std::string_view record_hex;
};

constexpr std::array<OracleMutation, 35> kOracleMutations{{
    {"bad_header_extent", "T01", "oracle_harness_structure_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c70000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae552400000000000000d5732007dbf35fbb1efbbde28f4fb42c5e290b4001f06fc60027a2fef437c3e653424456414c3031900100000000200004000000000000003331a286a046543100000000"},
    {"bad_magic", "T01", "oracle_harness_structure_invalid",
     OutcomeScope::harness_only, "", "", true,
     "584244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae552400000000000000eea16c014e0c09f5af3b83a66acbb1a49918e6dc2ddc6ae99b1308bcb26fd00453424456414c3031900100000000200004000000000000003331a286a046543100000000"},
    {"bad_version", "T01", "oracle_harness_structure_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310200c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae5524000000000000006abe314ba7e907e5aa9e27df1269652462d03a997e332c8cca215eb9c687fdc753424456414c3031900100000000200004000000000000003331a286a046543100000000"},
    {"catalog_generation_mismatch", "T03", "oracle_harness_authority_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70806000000000000000800000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae552400000000000000d684e770ae6992e26e7b59ae1f324c6be8cfca18f33a2d25e9c73712b06fa64253424456414c3031900100000000200004000000000000003331a286a046543100000000"},
    {"codec_generation_mismatch", "T03", "oracle_harness_authority_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000200000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae55240000000000000025d3fb4ed5cfd31f4a05b195fce75eeafcc2f4031cb7a883a8d749506a6482ae53424456414c3031900100000000200004000000000000003331a286a046543100000000"},
    {"codec_identity_mismatch", "T03", "oracle_harness_authority_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c01000000000000000000000000000000000000000000000001000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae552400000000000000c2193084019d71209dbf4ef2652dd419bfdc5a87f80211e9efc47e68a98b538d53424456414c3031900100000000200004000000000000003331a286a046543100000000"},
    {"codec_version_mismatch", "T03", "oracle_harness_authority_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d02000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae552400000000000000f8911742c873b571536c740ae3b88b9e9433a0699f0ca2a15b25131e2502a64953424456414c3031900100000000200004000000000000003331a286a046543100000000"},
    {"descriptor_generation_mismatch", "T03", "oracle_harness_authority_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000200000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae55240000000000000039b2a7718a8ec682a8b46a951dff854433fe3d75b30d7259fbc0b98e04b8d35b53424456414c3031900100000000200004000000000000003331a286a046543100000000"},
    {"descriptor_identity_mismatch", "T03", "oracle_harness_authority_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000000000000000000000000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae552400000000000000f652202fddc8b777d1c8287f2ebaed1d153c0f16cf5eaa3d2594a327910f2cbd53424456414c3031900100000000200004000000000000003331a286a046543100000000"},
    {"evidence_hash_flip", "T02", "oracle_harness_structure_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae552400000000000000bf1e64ba22d2958575ce3d9e4182ea4dbac669cf0489179f7149e39a1770f4d953424456414c3031900100000000200004000000000000003331a286a046543100000000"},
    {"evidence_integrity_plus_inner_state", "T02", "oracle_evidence_precedes_inner_state",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae5524000000000000008e7934f3a6f0cca7ee7a188585ae56a4b2f481af04f54f47cae1f1cab75fe5f653424456414c3031900100000100200004000000000000003331a286a046543100000000"},
    {"frame_length_mismatch", "T01", "oracle_harness_structure_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae5523000000000000007dd84c03ed09ba87c5eff5bb54651a9097d9165975b5ee106822cc105b52514853424456414c3031900100000000200004000000000000003331a286a046543100000000"},
    {"inner_NULL_not_admitted", "T05", "oracle_nested_sbdval_null_not_admitted",
     OutcomeScope::nested_public, "DATATYPE.NULL_NOT_ADMITTED", "aba0828d-77c9-50de-9842-7f2047100214", false,
     "534244544f5230310100c80001000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae5520000000000000008aa076a5fcfd7cf78e7c39bc6b70dd06c0fab26917c15d8a9f1e286c63d07a2e53424456414c30319001000001002000000000000000000083039d73b00f6514"},
    {"inner_VALUE_payload_missing", "T05", "oracle_nested_sbdval_structure_invalid",
     OutcomeScope::nested_public, "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "01a1008e-c200-7003-8000-000000000003", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae55200000000000000036cfbd3db39113d4274e18411bec9d13171741671f4a447912fb5bd1a60e53fd53424456414c30319001000000002000000000000000000083039d73b00f6514"},
    {"inner_bad_header_extent", "T05", "oracle_nested_sbdval_structure_invalid",
     OutcomeScope::nested_public, "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "01a1008e-c200-7003-8000-000000000003", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae5524000000000000002a99dff7634868614ca26c00eb7f287a9f91886e99344a9815c6529feb3632b653424456414c30319001000000001f0004000000000000003331a286a046543100000000"},
    {"inner_bad_magic", "T05", "oracle_nested_sbdval_structure_invalid",
     OutcomeScope::nested_public, "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "01a1008e-c200-7003-8000-000000000003", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae552400000000000000a57855b05262b85fd78d7e1c16d44c1ed6eae573c1598b5c992bb4708f1e360558424456414c3031900100000000200004000000000000003331a286a046543100000000"},
    {"inner_bad_type", "T05", "oracle_nested_sbdval_authority_mismatch",
     OutcomeScope::nested_public, "CTI.TEMPORAL.DESCRIPTOR_INVALID", "01a1008e-c200-7001-8000-000000000001", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae552400000000000000a445561a11204e4360aa7ccc0cac64b5c3e7c3a2e70980acdb1a0638154003d753424456414c30312d0100000000200004000000000000003331a286a046543100000000"},
    {"inner_checksum_corrupt", "T05", "oracle_nested_sbdval_structure_invalid",
     OutcomeScope::nested_public, "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "01a1008e-c200-7003-8000-000000000003", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae55240000000000000099822733c17d870201d3914cb17ffa7c19ca62fb7c28357ad380b91024382bf053424456414c3031900100000000200004000000000000003231a286a046543100000000"},
    {"inner_payload_bearing_NULL", "T05", "oracle_nested_sbdval_null_state_invalid",
     OutcomeScope::nested_public, "DATATYPE.NULL_STATE.INVALID", "588fafd0-6386-55cf-844e-f5b0dc22c700", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae5524000000000000008c9efd426df5b0827c152c2a88053f7cf2d4318fe8f22615980ec44a6f6e925a53424456414c3031900100000100200004000000000000003331a286a046543100000000"},
    {"inner_payload_extent_mismatch", "T05", "oracle_nested_sbdval_structure_invalid",
     OutcomeScope::nested_public, "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "01a1008e-c200-7003-8000-000000000003", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae552400000000000000badf321b1592ee20f91bc5dc19cc01fbc6d0f871c9eb5bd325eac7255e508cf853424456414c3031900100000000200005000000000000003331a286a046543100000000"},
    {"inner_reserved_nonzero", "T05", "oracle_nested_sbdval_structure_invalid",
     OutcomeScope::nested_public, "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "01a1008e-c200-7003-8000-000000000003", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae55240000000000000071f95d6b366535e71e5166aa2daf0f55a2a8cee90ca391bdfb715440daed91aa53424456414c3031900100000000200004000000010000003331a286a046543100000000"},
    {"inner_trailing_byte", "T01", "oracle_nested_sbdval_structure_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae552500000000000000abebee2c9ddf708befb198746b38f433f7ec6d2eb644e2ab738b4d12b86caef353424456414c3031900100000000200004000000000000003331a286a04654310000000000"},
    {"inner_truncated_31", "T05", "oracle_nested_sbdval_structure_invalid",
     OutcomeScope::nested_public, "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "01a1008e-c200-7003-8000-000000000003", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae551f000000000000000f31e81aaf189be07c9de18ed12892cf3740f902c4e512318edda41a428c121553424456414c3031900100000000200004000000000000003331a286a04654"},
    {"inner_unknown_flags", "T05", "oracle_nested_sbdval_structure_invalid",
     OutcomeScope::nested_public, "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "01a1008e-c200-7003-8000-000000000003", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae55240000000000000084d9844e860331a6b2a54fd9caa5be55cdbb6e724fb46cb1accae17fc3065f7653424456414c3031900100000200200004000000000000003331a286a046543100000000"},
    {"outer_inner_state_mismatch", "T06", "oracle_harness_state_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80001000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae5524000000000000004f997947aeada204691772c7e1af727cf1f88bf3f24c14b450ab050420be298253424456414c3031900100000000200004000000000000003331a286a046543100000000"},
    {"outer_state_invalid", "T04", "oracle_harness_state_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80002000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae5524000000000000005abdca8907c053accd7e888c6f5e8d112ab2d35f3ce843dacf0c569c4d51a21853424456414c3031900100000000200004000000000000003331a286a046543100000000"},
    {"outer_structure_plus_authority", "T01", "oracle_outer_structure_precedes_authority",
     OutcomeScope::harness_only, "", "", true,
     "584244544f5230310100c800000000000000000000000000000000000000000008000000000000000800000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae5524000000000000000b1181763a07611baa4bd6edf1917f77bb85fba75717b5842633dd51c2a3bc7553424456414c3031900100000000200004000000000000003331a286a046543100000000"},
    {"profile_mismatch", "T03", "oracle_harness_authority_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000000000000000000000000000000000000000000000000000000000000000000000240000000000000077711fe914441f8935e44917aa4f1678c496053bf036bc13a69f93bbcdb1033e53424456414c3031900100000000200004000000000000003331a286a046543100000000"},
    {"receipt_mismatch", "T03", "oracle_harness_authority_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70608000000000000000800000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae552400000000000000afca4d8799c73e33fd4824981829790cad7d35c487165c62c97f9e899d5bb08e53424456414c3031900100000000200004000000000000003331a286a046543100000000"},
    {"registry_generation_mismatch", "T03", "oracle_harness_authority_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d02000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae552400000000000000f8911742c873b571536c740ae3b88b9e9433a0699f0ca2a15b25131e2502a64953424456414c3031900100000000200004000000000000003331a286a046543100000000"},
    {"reserved3_nonzero", "T01", "oracle_harness_structure_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000010000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae5524000000000000005fd341e7ef7f1e437f5a46ec67111e0c79bb2245f4d2ae87657f9c6a13d34ab853424456414c3031900100000000200004000000000000003331a286a046543100000000"},
    {"reserved_u32_nonzero", "T01", "oracle_harness_structure_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000010000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae552400000000000000a665ed2c4e3965645309c2d0d37fbca1e323973e13ec5e32cb350bd3c7623bca53424456414c3031900100000000200004000000000000003331a286a046543100000000"},
    {"trailing_byte", "T01", "oracle_harness_structure_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae5524000000000000009f8dbd8ee8a2c1402b3a72b6a7f5ca91107f05ac6191cec43cb9d6c1393717cf53424456414c3031900100000000200004000000000000003331a286a04654310000000000"},
    {"type_generation_mismatch", "T03", "oracle_harness_authority_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0200000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae55240000000000000080b7a13848c429b66df98ecf72c9b7c4b7c65d5e62fd085989ecbad22766d1b353424456414c3031900100000000200004000000000000003331a286a046543100000000"},
    {"type_identity_mismatch", "T03", "oracle_harness_authority_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000900100006461746580000000000000000100000000000000000000000000000000000000000000000100000000000000019d000000007000800000000000d81d01000000000000000100000000000000577595923169d577e4cee18e114d9a09e31609823c53cbd1d608fa50641aae552400000000000000eb02859dece86f21ff234ecc2b9f3e863873a5be75b89e64133eb54c4a8ef75a53424456414c3031900100000000200004000000000000003331a286a046543100000000"},
}};

constexpr std::array<std::string_view, 35> kOracleMutationNames{{
    "bad_header_extent",
    "bad_magic",
    "bad_version",
    "catalog_generation_mismatch",
    "codec_generation_mismatch",
    "codec_identity_mismatch",
    "codec_version_mismatch",
    "descriptor_generation_mismatch",
    "descriptor_identity_mismatch",
    "evidence_hash_flip",
    "evidence_integrity_plus_inner_state",
    "frame_length_mismatch",
    "inner_NULL_not_admitted",
    "inner_VALUE_payload_missing",
    "inner_bad_header_extent",
    "inner_bad_magic",
    "inner_bad_type",
    "inner_checksum_corrupt",
    "inner_payload_bearing_NULL",
    "inner_payload_extent_mismatch",
    "inner_reserved_nonzero",
    "inner_trailing_byte",
    "inner_truncated_31",
    "inner_unknown_flags",
    "outer_inner_state_mismatch",
    "outer_state_invalid",
    "outer_structure_plus_authority",
    "profile_mismatch",
    "receipt_mismatch",
    "registry_generation_mismatch",
    "reserved3_nonzero",
    "reserved_u32_nonzero",
    "trailing_byte",
    "type_generation_mismatch",
    "type_identity_mismatch",
}};

std::array<platform::byte, 32> OracleEvidence(
    std::span<const platform::byte> header_prefix,
    std::span<const platform::byte> frame) {
  const std::array<core_hash::HashDigestSegment, 3> segments{{
      {reinterpret_cast<const platform::byte*>(kOracleEvidenceDomain.data()),
       kOracleEvidenceDomain.size()},
      {header_prefix.data(), header_prefix.size()},
      {frame.data(), frame.size()},
  }};
  const auto result =
      core_hash::ComputeSha256DigestPartsNative(segments.data(), segments.size());
  Require(result.ok(), "SBDTOR01 evidence SHA-256");
  return result.digest;
}

void PutUuid(platform::byte* output, const platform::Uuid& value) {
  std::memcpy(output, value.bytes.data(), value.bytes.size());
}

bool ExactUuid(const platform::byte* input, const platform::Uuid& value) {
  return std::memcmp(input, value.bytes.data(), value.bytes.size()) == 0;
}

std::vector<platform::byte> EncodeOracleRecord(
    const dt::DateValidatedProfileHandleV3& profile,
    dt::DateValueStateV3 state,
    std::span<const platform::byte> frame) {
  Require(frame.size() <= kOracleMaximumFrameBytes,
          "SBDTOR01 frame exceeds bounded test layout");
  std::vector<platform::byte> encoded(kOracleHeaderBytes + frame.size());
  std::memcpy(encoded.data(), "SBDTOR01", 8);
  platform::StoreLittle16(encoded.data() + 8, 1);
  platform::StoreLittle16(encoded.data() + 10, kOracleHeaderBytes);
  encoded[12] = static_cast<platform::byte>(state);
  PutUuid(encoded.data() + 16, profile.receipt.catalog_snapshot_uuid);
  platform::StoreLittle64(encoded.data() + 32,
                          profile.receipt.catalog_generation);
  platform::StoreLittle64(encoded.data() + 40,
                          profile.receipt.registry_generation);
  const auto& identity = profile.identity.legacy_fields;
  PutUuid(encoded.data() + 48, identity.descriptor_uuid);
  platform::StoreLittle64(encoded.data() + 64,
                          identity.descriptor_generation);
  PutUuid(encoded.data() + 72, identity.type_uuid);
  platform::StoreLittle64(encoded.data() + 88, identity.type_generation);
  PutUuid(encoded.data() + 96, identity.codec_uuid);
  platform::StoreLittle32(encoded.data() + 112, identity.codec_version);
  platform::StoreLittle64(encoded.data() + 120, identity.codec_generation);
  std::copy(profile.profile_fingerprint.begin(),
            profile.profile_fingerprint.end(), encoded.begin() + 128);
  platform::StoreLittle64(encoded.data() + 160, frame.size());
  std::copy(frame.begin(), frame.end(), encoded.begin() + kOracleHeaderBytes);
  const auto evidence = OracleEvidence(
      std::span<const platform::byte>(encoded.data(), 168), frame);
  std::copy(evidence.begin(), evidence.end(), encoded.begin() + 168);
  return encoded;
}

struct OracleDecodeResult {
  bool ok = false;
  std::string_view gate;
  std::uint8_t passed_gate_mask = 0;
  OutcomeScope scope = OutcomeScope::harness_only;
  std::string_view public_diagnostic_code;
  dt::DateValueStateV3 state = dt::DateValueStateV3::value;
  std::int32_t day = 0;
  std::vector<platform::byte> reencoded;
};

OracleDecodeResult HarnessFailure(std::string_view gate,
                                  std::uint8_t passed_gate_mask) {
  OracleDecodeResult result;
  result.gate = gate;
  result.passed_gate_mask = passed_gate_mask;
  result.scope = OutcomeScope::harness_only;
  return result;
}

OracleDecodeResult NestedFailure(std::string_view diagnostic_code,
                                 std::uint8_t passed_gate_mask) {
  OracleDecodeResult result;
  result.gate = "T05";
  result.passed_gate_mask = passed_gate_mask;
  result.scope = OutcomeScope::nested_public;
  result.public_diagnostic_code = diagnostic_code;
  return result;
}

OracleDecodeResult DecodeOracleRecord(
    const std::shared_ptr<const dt::DateValidatedProfileHandleV3>& profile,
    bool null_allowed,
    std::span<const platform::byte> encoded) {
  // T01: the outer harness validates its own bounded structure only. It does
  // not assign a public datatype diagnostic.
  if (encoded.size() < kOracleHeaderBytes ||
      encoded.size() > kOracleMaximumBytes ||
      std::memcmp(encoded.data(), "SBDTOR01", 8) != 0 ||
      platform::LoadLittle16(encoded.data() + 8) != 1 ||
      platform::LoadLittle16(encoded.data() + 10) != kOracleHeaderBytes ||
      encoded[13] != 0 || encoded[14] != 0 || encoded[15] != 0 ||
      platform::LoadLittle32(encoded.data() + 116) != 0) {
    return HarnessFailure("T01", 0);
  }
  const auto frame_bytes = platform::LoadLittle64(encoded.data() + 160);
  if (frame_bytes > kOracleMaximumFrameBytes ||
      frame_bytes != encoded.size() - kOracleHeaderBytes) {
    return HarnessFailure("T01", 0);
  }
  const auto frame = encoded.subspan(kOracleHeaderBytes,
                                     static_cast<std::size_t>(frame_bytes));

  // T02: outer evidence authenticates the exact header prefix and inner frame.
  const auto evidence = OracleEvidence(encoded.first(168), frame);
  if (!std::equal(evidence.begin(), evidence.end(), encoded.begin() + 168))
    return HarnessFailure("T02", 0x01);

  // T03: validate the complete supplied profile handle, then the exact receipt,
  // descriptor, type, codec, generations, and full-profile fingerprint stored
  // by the harness. Names and aliases are intentionally absent.
  if (!dt::ValidateDateProfileHandleV3(*profile).ok() ||
      !ExactUuid(encoded.data() + 16,
                 profile->receipt.catalog_snapshot_uuid) ||
      platform::LoadLittle64(encoded.data() + 32) !=
          profile->receipt.catalog_generation ||
      platform::LoadLittle64(encoded.data() + 40) !=
          profile->receipt.registry_generation) {
    return HarnessFailure("T03", 0x03);
  }
  const auto& identity = profile->identity.legacy_fields;
  if (!ExactUuid(encoded.data() + 48, identity.descriptor_uuid) ||
      platform::LoadLittle64(encoded.data() + 64) !=
          identity.descriptor_generation ||
      !ExactUuid(encoded.data() + 72, identity.type_uuid) ||
      platform::LoadLittle64(encoded.data() + 88) !=
          identity.type_generation ||
      !ExactUuid(encoded.data() + 96, identity.codec_uuid) ||
      platform::LoadLittle32(encoded.data() + 112) !=
          identity.codec_version ||
      platform::LoadLittle64(encoded.data() + 120) !=
          identity.codec_generation ||
      !std::equal(profile->profile_fingerprint.begin(),
                  profile->profile_fingerprint.end(), encoded.begin() + 128)) {
    return HarnessFailure("T03", 0x03);
  }

  // T04: outer state is a harness fact, not a public datatype state result.
  if (encoded[12] > 1) return HarnessFailure("T04", 0x07);
  const auto outer_state = encoded[12] == 0
      ? dt::DateValueStateV3::value : dt::DateValueStateV3::sql_null;

  // T05: only the nested production SBDVAL01 decoder may publish a datatype
  // diagnostic. The frame is passed unchanged with exact profile/nullability.
  const auto decoded =
      dt::DecodeDateSbdvalComposedNoAllocV3(*profile, null_allowed, frame);
  if (!decoded.ok())
    return NestedFailure(decoded.diagnostic.diagnostic_code, 0x0f);
  const dt::DateOwnedValueV3 inner_value{profile, decoded.value.state,
                                              decoded.value.day};
  const auto inner_reencoded = dt::EncodeDateSbdvalComposedV3(
      inner_value, null_allowed);
  if (!inner_reencoded.ok() || inner_reencoded.bytes !=
          std::vector<platform::byte>(frame.begin(), frame.end())) {
    return NestedFailure("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", 0x0f);
  }

  // T06: outer state agrees with the successfully decoded inner state.
  if (outer_state != decoded.value.state)
    return HarnessFailure("T06", 0x1f);

  // T07: rebuild the full test-only envelope and evidence byte-for-byte.
  const auto reencoded = EncodeOracleRecord(*profile, outer_state, frame);
  if (reencoded != std::vector<platform::byte>(encoded.begin(), encoded.end()))
    return HarnessFailure("T07", 0x3f);

  OracleDecodeResult result;
  result.ok = true;
  result.gate = "success";
  result.passed_gate_mask = 0x7f;
  result.scope = OutcomeScope::harness_only;
  result.state = decoded.value.state;
  result.day = decoded.value.day;
  result.reencoded = reencoded;
  return result;
}

std::uint8_t PassedMaskBefore(std::string_view gate) {
  if (gate == "T01") return 0;
  if (gate == "T02") return 0x01;
  if (gate == "T03") return 0x03;
  if (gate == "T04") return 0x07;
  if (gate == "T05") return 0x0f;
  if (gate == "T06") return 0x1f;
  if (gate == "T07") return 0x3f;
  throw std::runtime_error("unknown SBDTOR01 gate fixture");
}

std::string_view DiagnosticUuid(std::string_view code) {
  if (code == "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID")
    return "01a1008e-c200-7003-8000-000000000003";
  if (code == "CTI.TEMPORAL.DESCRIPTOR_INVALID")
    return "01a1008e-c200-7001-8000-000000000001";
  if (code == "DATATYPE.NULL_STATE.INVALID")
    return "588fafd0-6386-55cf-844e-f5b0dc22c700";
  if (code == "DATATYPE.NULL_NOT_ADMITTED")
    return "aba0828d-77c9-50de-9842-7f2047100214";
  return {};
}

bool IsOneOf(std::string_view value,
             std::initializer_list<std::string_view> candidates) {
  return std::find(candidates.begin(), candidates.end(), value) !=
         candidates.end();
}

std::string_view DeclaredOutcome(std::string_view name) {
  if (IsOneOf(name, {"bad_header_extent", "bad_magic", "bad_version",
                     "evidence_hash_flip", "frame_length_mismatch",
                     "reserved3_nonzero", "reserved_u32_nonzero",
                     "trailing_byte"}))
    return "oracle_harness_structure_invalid";
  if (IsOneOf(name, {"catalog_generation_mismatch",
                     "codec_generation_mismatch", "codec_identity_mismatch",
                     "codec_version_mismatch",
                     "descriptor_generation_mismatch",
                     "descriptor_identity_mismatch", "profile_mismatch",
                     "receipt_mismatch", "registry_generation_mismatch",
                     "type_generation_mismatch", "type_identity_mismatch"}))
    return "oracle_harness_authority_invalid";
  if (name == "evidence_integrity_plus_inner_state")
    return "oracle_evidence_precedes_inner_state";
  if (name == "inner_NULL_not_admitted")
    return "oracle_nested_sbdval_null_not_admitted";
  if (IsOneOf(name, {"inner_VALUE_payload_missing",
                     "inner_bad_header_extent", "inner_bad_magic",
                     "inner_checksum_corrupt", "inner_payload_extent_mismatch",
                     "inner_reserved_nonzero", "inner_trailing_byte",
                     "inner_truncated_31", "inner_unknown_flags"}))
    return "oracle_nested_sbdval_structure_invalid";
  if (name == "inner_bad_type")
    return "oracle_nested_sbdval_authority_mismatch";
  if (name == "inner_payload_bearing_NULL")
    return "oracle_nested_sbdval_null_state_invalid";
  if (IsOneOf(name, {"outer_inner_state_mismatch", "outer_state_invalid"}))
    return "oracle_harness_state_invalid";
  if (name == "outer_structure_plus_authority")
    return "oracle_outer_structure_precedes_authority";
  return {};
}

void AppendLengthPrefixed(std::vector<platform::byte>* image,
                          std::span<const platform::byte> record) {
  std::array<platform::byte, 4> extent{};
  platform::StoreLittle32(extent.data(),
                          static_cast<platform::u32>(record.size()));
  image->insert(image->end(), extent.begin(), extent.end());
  image->insert(image->end(), record.begin(), record.end());
}

std::vector<platform::byte> FileDeviceCloseReopen(
    const fs::path& filename, std::span<const platform::byte> image) {
  scratchbird::tests::OwnedTempDirectory directory;
  const auto path = directory.path() / filename.filename();
  disk::FileDevice writer;
  Require(writer.Open(path.string(), disk::FileOpenMode::create_new).ok(),
          "open conformance image for write");
  const auto write = writer.WriteAt(0, image.data(), image.size());
  Require(write.ok() && write.bytes_transferred == image.size() &&
              writer.Sync().ok() && writer.Close().ok(),
          "persist and independently close conformance image");

  disk::FileDevice reader;
  Require(reader.Open(path.string(),
                      disk::FileOpenMode::open_existing_read_only).ok(),
          "independently reopen conformance image");
  std::vector<platform::byte> restored(image.size());
  const auto read = reader.ReadAt(0, restored.data(), restored.size());
  Require(read.ok() && read.bytes_transferred == restored.size() &&
              reader.Close().ok(),
          "read and independently close conformance image");
  directory.Cleanup();
  return restored;
}

void TestComponentFrames(const std::shared_ptr<const dt::DateValidatedProfileHandleV3>& profile) {
  std::vector<platform::byte> image;
  for (const auto& vector : kComponentVectors) {
    const dt::DateOwnedValueV3 value{profile, vector.state, vector.day};
    const auto expected_sbdval = Hex(vector.sbdval);
    const auto expected_sbdpv = Hex(vector.sbdpv);

    const auto sbdval = dt::EncodeDateSbdvalComposedV3(value, true);
    Require(sbdval.ok() && sbdval.bytes == expected_sbdval,
            "sealed SBDVAL01 vector");
    const auto decoded_sbdval =
        dt::DecodeDateSbdvalComposedNoAllocV3(*profile, true, sbdval.bytes);
    Require(decoded_sbdval.ok() &&
                decoded_sbdval.value.state == vector.state &&
                decoded_sbdval.value.day == vector.day,
            "SBDVAL01 composed round trip");

    const auto sbdpv = dt::EncodeDateSbdpvComposedV3(value, true);
    Require(sbdpv.ok() && sbdpv.bytes == expected_sbdpv,
            "sealed SBDPV001 vector");
    const auto decoded_sbdpv =
        dt::DecodeDateSbdpvComposedNoAllocV3(*profile, true, sbdpv.bytes);
    Require(decoded_sbdpv.ok() &&
                decoded_sbdpv.value.state == vector.state &&
                decoded_sbdpv.value.day == vector.day,
            "SBDPV001 composed round trip");

    AppendLengthPrefixed(&image, sbdval.bytes);
    AppendLengthPrefixed(&image, sbdpv.bytes);
  }
  const auto path = fs::temp_directory_path() / "sb-base-date-component.test";
  Require(FileDeviceCloseReopen(path, image) == image,
          "component close/reopen byte equality");
}

void TestRawProfileRefusals() {
  const auto raw_value = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::date, false, false, {0, 0, 0, 0}});
  Require(!raw_value.ok() &&
              raw_value.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
          "raw date SBDVAL profile refusal");
  const auto raw_null = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::date, true, false, {}});
  Require(!raw_null.ok() &&
              raw_null.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
          "raw date NULL SBDVAL profile refusal");
  const auto physical_value = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::date, dt::DatatypePhysicalValueState::value,
       {0, 0, 0, 0}});
  Require(!physical_value.ok() &&
              physical_value.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
          "raw date SBDPV profile refusal");
}

void TestOraclePositives(const std::shared_ptr<const dt::DateValidatedProfileHandleV3>& profile) {
  std::vector<platform::byte> image;
  for (std::size_t index = 0; index < kOraclePositiveVectors.size(); ++index) {
    const auto& vector = kOraclePositiveVectors[index];
    Require(vector.name == kOraclePositiveNames[index],
            "SBDTOR01 exact positive roster");
    const dt::DateOwnedValueV3 value{profile, vector.state, vector.day};
    const auto production_frame = dt::EncodeDateSbdvalComposedV3(value, true);
    const auto expected_frame = Hex(vector.frame_hex);
    Require(production_frame.ok() && production_frame.bytes == expected_frame,
            "SBDTOR01 must wrap unchanged production SBDVAL01 bytes");

    const auto encoded = EncodeOracleRecord(
        *profile, vector.state, production_frame.bytes);
    Require(encoded == Hex(vector.record_hex),
            "sealed SBDTOR01 positive bytes");
    const auto decoded = DecodeOracleRecord(profile, true, encoded);
    Require(decoded.ok && decoded.passed_gate_mask == 0x7f &&
                decoded.state == vector.state && decoded.day == vector.day &&
                decoded.reencoded == encoded,
            "SBDTOR01 positive gate trace and byte-identical reencode");
    AppendLengthPrefixed(&image, encoded);
  }

  const auto path = fs::temp_directory_path() / "sb-base-date-sbdtor01.test";
  const auto restored = FileDeviceCloseReopen(path, image);
  Require(restored == image, "SBDTOR01 FileDevice close/reopen bytes");

  std::size_t offset = 0;
  std::size_t records = 0;
  while (offset < restored.size()) {
    Require(restored.size() - offset >= 4,
            "SBDTOR01 restored length prefix");
    const auto extent = platform::LoadLittle32(restored.data() + offset);
    offset += 4;
    Require(extent <= restored.size() - offset,
            "SBDTOR01 restored checked extent");
    const auto record = std::span<const platform::byte>(
        restored.data() + offset, extent);
    const auto decoded = DecodeOracleRecord(profile, true, record);
    Require(decoded.ok && decoded.passed_gate_mask == 0x7f &&
                decoded.reencoded ==
                    std::vector<platform::byte>(record.begin(), record.end()),
            "SBDTOR01 restored decode/reencode");
    offset += extent;
    ++records;
  }
  Require(offset == restored.size() && records == kOraclePositiveVectors.size(),
          "SBDTOR01 restored exact four-record roster");
}

void TestOracleMutations(const std::shared_ptr<const dt::DateValidatedProfileHandleV3>& profile) {
  std::size_t harness_only = 0;
  std::size_t nested_public = 0;
  for (std::size_t index = 0; index < kOracleMutations.size(); ++index) {
    const auto& mutation = kOracleMutations[index];
    Require(!mutation.name.empty() &&
                mutation.name == kOracleMutationNames[index] &&
                mutation.expected == DeclaredOutcome(mutation.name),
            "SBDTOR01 exact mutation roster/outcome");
    const auto encoded = Hex(mutation.record_hex);
    const auto decoded =
        DecodeOracleRecord(profile, mutation.null_allowed, encoded);
    Require(!decoded.ok && decoded.gate == mutation.gate &&
                decoded.passed_gate_mask == PassedMaskBefore(mutation.gate) &&
                decoded.scope == mutation.scope,
            "SBDTOR01 mutation earliest gate/scope");
    if (mutation.scope == OutcomeScope::nested_public) {
      ++nested_public;
      Require(!mutation.public_diagnostic_code.empty() &&
                  decoded.public_diagnostic_code ==
                      mutation.public_diagnostic_code &&
                  DiagnosticUuid(decoded.public_diagnostic_code) ==
                      mutation.public_diagnostic_uuid,
              "SBDTOR01 nested public diagnostic code/UUID");
    } else {
      ++harness_only;
      Require(mutation.public_diagnostic_code.empty() &&
                  mutation.public_diagnostic_uuid.empty() &&
                  decoded.public_diagnostic_code.empty(),
              "SBDTOR01 outer harness failure exposed public diagnostic");
    }
  }
  Require(harness_only == 24 && nested_public == 11,
          "SBDTOR01 exact mutation outcome partition");
}

}  // namespace

int main() try {
  TestFixtureIsolationAndFailureCleanup();
  const auto profile = std::make_shared<const dt::DateValidatedProfileHandleV3>(Profile());
  TestComponentFrames(profile);
  TestRawProfileRefusals();
  TestOraclePositives(profile);
  TestOracleMutations(profile);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
