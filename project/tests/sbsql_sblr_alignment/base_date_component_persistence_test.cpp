// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "datatype_date.hpp"
#include "datatype_physical_encoding.hpp"
#include "disk_device.hpp"
#include "hash_digest_parts.hpp"

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

platform::Uuid StatementReceipt() {
  platform::Uuid value{};
  value.bytes[0] = 1;
  value.bytes[1] = 0x9d;
  value.bytes[6] = 0x70;
  value.bytes[8] = 0x80;
  value.bytes[14] = 0xd7;
  value.bytes[15] = 0x07;
  return value;
}

dt::DateValidatedProfileHandleV1 Profile() {
  auto result = dt::BuildCurrentDateValidatedProfileHandleV1(StatementReceipt());
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
  dt::DateValueStateV1 state;
  std::int32_t day;
  std::string_view sbdval;
  std::string_view sbdpv;
};

constexpr std::array<ComponentVector, 4> kComponentVectors{{
    {dt::DateValueStateV1::value, 0,
     "53424456414c3031900100000000200004000000000000003331a286a046543100000000",
     "5342445056303031900100000100000004000000eac93b7d00000000"},
    {dt::DateValueStateV1::value, std::numeric_limits<std::int32_t>::min(),
     "53424456414c303190010000000020000400000000000000b357a186a0c6533100000080",
     "53424450563030319001000001000000040000006a003bfd00000080"},
    {dt::DateValueStateV1::value, std::numeric_limits<std::int32_t>::max(),
     "53424456414c3031900100000000200004000000000000000f2d562864985844ffffff7f",
     "534244505630303190010000010000000400000006a24d95ffffff7f"},
    {dt::DateValueStateV1::sql_null, 0,
     "53424456414c30319001000001002000000000000000000083039d73b00f6514",
     "5342445056303031900100000000000000000000dda0e66d"},
}};

struct OraclePositiveVector {
  std::string_view name;
  dt::DateValueStateV1 state;
  std::int32_t day;
  std::string_view frame_hex;
  std::string_view record_hex;
};

constexpr std::array<OraclePositiveVector, 4> kOraclePositiveVectors{{
    {"epoch", dt::DateValueStateV1::value, 0,
     "53424456414c3031900100000000200004000000000000003331a286a046543100000000",
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2400000000000000fc109ca7c55431fb4ef2441314d449057837fe2c2a9cec9408ccbf02295bbc9a53424456414c3031900100000000200004000000000000003331a286a046543100000000" },
    {"maximum", dt::DateValueStateV1::value, 2147483647,
     "53424456414c3031900100000000200004000000000000000f2d562864985844ffffff7f",
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2400000000000000f233df9f6a1cf23afb68e84e4e8548226f04d75ea5ea9cac5df47fdf2eef5bba53424456414c3031900100000000200004000000000000000f2d562864985844ffffff7f" },
    {"minimum", dt::DateValueStateV1::value, -2147483648,
     "53424456414c303190010000000020000400000000000000b357a186a0c6533100000080",
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2400000000000000518e6c62eb9824f489faedee2561b51c13b4380a4211f1094088675a7d169fb353424456414c303190010000000020000400000000000000b357a186a0c6533100000080" },
    {"sql_null", dt::DateValueStateV1::sql_null, 0,
     "53424456414c30319001000001002000000000000000000083039d73b00f6514",
     "534244544f5230310100c80001000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2000000000000000bc0355ac32810efa02b2144e1985c0fe35ab6adb1744c7d504309011e1aafc0253424456414c30319001000001002000000000000000000083039d73b00f6514" }
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
     "534244544f5230310100c70000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2400000000000000fc109ca7c55431fb4ef2441314d449057837fe2c2a9cec9408ccbf02295bbc9a53424456414c3031900100000000200004000000000000003331a286a046543100000000" },
    {"bad_magic", "T01", "oracle_harness_structure_invalid",
     OutcomeScope::harness_only, "", "", true,
     "584244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2400000000000000fc109ca7c55431fb4ef2441314d449057837fe2c2a9cec9408ccbf02295bbc9a53424456414c3031900100000000200004000000000000003331a286a046543100000000" },
    {"bad_version", "T01", "oracle_harness_structure_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310200c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2400000000000000fc109ca7c55431fb4ef2441314d449057837fe2c2a9cec9408ccbf02295bbc9a53424456414c3031900100000000200004000000000000003331a286a046543100000000" },
    {"catalog_generation_mismatch", "T03", "oracle_harness_authority_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70706000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2400000000000000aa31794b032ac03a7ee3c14f65c012db5e95d795b9155223e0137ffca31739b553424456414c3031900100000000200004000000000000003331a286a046543100000000" },
    {"codec_generation_mismatch", "T03", "oracle_harness_authority_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000002000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f24000000000000001c3c5307d99bde948e3d8c28435ceff73834890aeae1c88b97f3a6a516dc99ae53424456414c3031900100000000200004000000000000003331a286a046543100000000" },
    {"codec_identity_mismatch", "T03", "oracle_harness_authority_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c010000000000000000000000000000000000000000000000010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f240000000000000025c6e21aaa8513802b580a52f62fa612e8b1697f20e56c4c46f1dc881aae2a3753424456414c3031900100000000200004000000000000003331a286a046543100000000" },
    {"codec_version_mismatch", "T03", "oracle_harness_authority_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d020000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f24000000000000003c958179ac0d04544de26dbcf2ed0cd89ffa52b8c7fef68d9b450ace07f4ca8153424456414c3031900100000000200004000000000000003331a286a046543100000000" },
    {"descriptor_generation_mismatch", "T03", "oracle_harness_authority_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000200000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f24000000000000007a3efb692b394a7b8bf8ab212383e0f9089d439521703a765709c4a7a6c91f9c53424456414c3031900100000000200004000000000000003331a286a046543100000000" },
    {"descriptor_identity_mismatch", "T03", "oracle_harness_authority_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000000000000000000000000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2400000000000000351ef8a663cfc2c84ecf54ed7fa9da6fcc169ab6fba6a5490f96767aa1987da953424456414c3031900100000000200004000000000000003331a286a046543100000000" },
    {"evidence_hash_flip", "T02", "oracle_harness_structure_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2400000000000000fd109ca7c55431fb4ef2441314d449057837fe2c2a9cec9408ccbf02295bbc9a53424456414c3031900100000000200004000000000000003331a286a046543100000000" },
    {"evidence_integrity_plus_inner_state", "T02", "oracle_evidence_precedes_inner_state",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2400000000000000000000000000000000000000000000000000000000000000000000000000000053424456414c3031900100000100200004000000000000003331a286a046543100000000" },
    {"frame_length_mismatch", "T01", "oracle_harness_structure_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2300000000000000fc109ca7c55431fb4ef2441314d449057837fe2c2a9cec9408ccbf02295bbc9a53424456414c3031900100000000200004000000000000003331a286a046543100000000" },
    {"inner_NULL_not_admitted", "T05", "oracle_nested_sbdval_null_not_admitted",
     OutcomeScope::nested_public, "DATATYPE.NULL_NOT_ADMITTED", "aba0828d-77c9-50de-9842-7f2047100214", false,
     "534244544f5230310100c80001000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2000000000000000bc0355ac32810efa02b2144e1985c0fe35ab6adb1744c7d504309011e1aafc0253424456414c30319001000001002000000000000000000083039d73b00f6514" },
    {"inner_VALUE_payload_missing", "T05", "oracle_nested_sbdval_structure_invalid",
     OutcomeScope::nested_public, "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "01a1008e-c200-7003-8000-000000000003", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f200000000000000099b7043dcb342aea376fb979d02b26191f377a4ebffce934ccaac176c6084bb153424456414c30319001000000002000000000000000000083039d73b00f6514" },
    {"inner_bad_header_extent", "T05", "oracle_nested_sbdval_structure_invalid",
     OutcomeScope::nested_public, "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "01a1008e-c200-7003-8000-000000000003", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2400000000000000d49ce06ebf1357f866f89d656c0a6160fd363346805664d1d506970e02f4fbab53424456414c30319001000000001f0004000000000000003331a286a046543100000000" },
    {"inner_bad_magic", "T05", "oracle_nested_sbdval_structure_invalid",
     OutcomeScope::nested_public, "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "01a1008e-c200-7003-8000-000000000003", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2400000000000000056eb4e59333dfcf9bc8787038cf8db156aa5099bfe5fa9159ba2e822689f67458424456414c3031900100000000200004000000000000003331a286a046543100000000" },
    {"inner_bad_type", "T05", "oracle_nested_sbdval_authority_mismatch",
     OutcomeScope::nested_public, "CTI.TEMPORAL.DESCRIPTOR_INVALID", "01a1008e-c200-7001-8000-000000000001", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f24000000000000007254d1f1a22e6a21684f9e0b925f0982173826c0a6ec7ca87118c672b7d4c50d53424456414c30312d0100000000200004000000000000003331a286a046543100000000" },
    {"inner_checksum_corrupt", "T05", "oracle_nested_sbdval_structure_invalid",
     OutcomeScope::nested_public, "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "01a1008e-c200-7003-8000-000000000003", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2400000000000000abd3e8529421718edf000822eab9e9cc61bdc035c2c3660de32f14eb7366a93853424456414c3031900100000000200004000000000000003231a286a046543100000000" },
    {"inner_payload_bearing_NULL", "T05", "oracle_nested_sbdval_null_state_invalid",
     OutcomeScope::nested_public, "DATATYPE.NULL_STATE.INVALID", "588fafd0-6386-55cf-844e-f5b0dc22c700", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2400000000000000fe93af8416995b87e9ede338893c6c3ca0bbee042b0a3b6c5d0e7e1c30bd68f453424456414c3031900100000100200004000000000000003331a286a046543100000000" },
    {"inner_payload_extent_mismatch", "T05", "oracle_nested_sbdval_structure_invalid",
     OutcomeScope::nested_public, "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "01a1008e-c200-7003-8000-000000000003", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f240000000000000014b78d7a754eef7b5f231f7527a956ec5b382b733dfabb021c6d1f409a52efdb53424456414c3031900100000000200005000000000000003331a286a046543100000000" },
    {"inner_reserved_nonzero", "T05", "oracle_nested_sbdval_structure_invalid",
     OutcomeScope::nested_public, "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "01a1008e-c200-7003-8000-000000000003", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2400000000000000849fea26ff78bde86113fc4898b666bf1e8b105ce9fabd78e863054f585a857353424456414c3031900100000000200004000000010000003331a286a046543100000000" },
    {"inner_trailing_byte", "T01", "oracle_nested_sbdval_structure_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f250000000000000058c77b848b52826868ce4bd6b2063c364204cc8c8a34cb98e7b4c88b555fd6d253424456414c3031900100000000200004000000000000003331a286a04654310000000000" },
    {"inner_truncated_31", "T05", "oracle_nested_sbdval_structure_invalid",
     OutcomeScope::nested_public, "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "01a1008e-c200-7003-8000-000000000003", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f1f0000000000000026f67f3a47959dce9e2471ffd5e58475a6b45dfac2c97a3296e8dc003162b59453424456414c3031900100000000200004000000000000003331a286a04654" },
    {"inner_unknown_flags", "T05", "oracle_nested_sbdval_structure_invalid",
     OutcomeScope::nested_public, "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "01a1008e-c200-7003-8000-000000000003", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2400000000000000c30ef75439fd01e096285a877361b3048ba59a10e847384e6349065cf427bbda53424456414c3031900100000200200004000000000000003331a286a046543100000000" },
    {"outer_inner_state_mismatch", "T06", "oracle_harness_state_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80001000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2400000000000000db777413d5dc5cbe94ab7f01f2bdaaa7a67e6524d0618dcae4ea8f1df581184953424456414c3031900100000000200004000000000000003331a286a046543100000000" },
    {"outer_state_invalid", "T04", "oracle_harness_state_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80002000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f240000000000000003848671c0a7241938e215ea29c707127fbf5c2f400a590028643065cc7c9b8153424456414c3031900100000000200004000000000000003331a286a046543100000000" },
    {"outer_structure_plus_authority", "T01", "oracle_outer_structure_precedes_authority",
     OutcomeScope::harness_only, "", "", true,
     "584244544f5230310100c800000000000000000000000000000000000000000007000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2400000000000000fc109ca7c55431fb4ef2441314d449057837fe2c2a9cec9408ccbf02295bbc9a53424456414c3031900100000000200004000000000000003331a286a046543100000000" },
    {"profile_mismatch", "T03", "oracle_harness_authority_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000000000000000000000000000000000000000000000000000000000000000000000240000000000000092ee464744ad27946b42dc491c2078984853b54496d5dc93e9040d0139ffb22453424456414c3031900100000000200004000000000000003331a286a046543100000000" },
    {"receipt_mismatch", "T03", "oracle_harness_authority_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70607000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2400000000000000d254db5ff092eff7b97b85d9a7b6decbf08232d485363b3c841aedbc19afbf8553424456414c3031900100000000200004000000000000003331a286a046543100000000" },
    {"registry_generation_mismatch", "T03", "oracle_harness_authority_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000600000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f240000000000000026514fd14b685c6a678f188312ad62a1d38a3ffec82994e78909ded803601d7d53424456414c3031900100000000200004000000000000003331a286a046543100000000" },
    {"reserved3_nonzero", "T01", "oracle_harness_structure_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000010000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f24000000000000008842076dcda83ea829793c76d0903b3a99d1f2987fe61a16074ed23d52afecc053424456414c3031900100000000200004000000000000003331a286a046543100000000" },
    {"reserved_u32_nonzero", "T01", "oracle_harness_structure_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000100000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2400000000000000a3d02fb69102e89aa3eea8663bb0163c257c47f99282aa6a6a96621409f7a2ac53424456414c3031900100000000200004000000000000003331a286a046543100000000" },
    {"trailing_byte", "T01", "oracle_harness_structure_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2400000000000000fc109ca7c55431fb4ef2441314d449057837fe2c2a9cec9408ccbf02295bbc9a53424456414c3031900100000000200004000000000000003331a286a04654310000000000" },
    {"type_generation_mismatch", "T03", "oracle_harness_authority_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000019d000000007000800000000000d81c0200000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f24000000000000006a0a3c7f6dec46184a5f230db097708e16bf166a22efc5c001089388d69a991653424456414c3031900100000000200004000000000000003331a286a046543100000000" },
    {"type_identity_mismatch", "T03", "oracle_harness_authority_invalid",
     OutcomeScope::harness_only, "", "", true,
     "534244544f5230310100c80000000000019d000000007000800000000000d70707000000000000000700000000000000900100006461746580000000000000000100000000000000000000000000000000000000000000000100000000000000019d000000007000800000000000d81d010000000000000001000000000000009ea10cf4602ccf1fb57e5bdb4d8caf00bc93afed86494f21316e54a00da0826f2400000000000000fd4f0e85b8e78b329e1b285a19757c43854ca1b1fa72d10da23a78f28228c3dd53424456414c3031900100000000200004000000000000003331a286a046543100000000" }
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
    const dt::DateValidatedProfileHandleV1& profile,
    dt::DateValueStateV1 state,
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
  dt::DateValueStateV1 state = dt::DateValueStateV1::value;
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
    const std::shared_ptr<const dt::DateValidatedProfileHandleV1>& profile,
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
  if (!dt::ValidateDateProfileHandleV1(*profile).ok() ||
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
      ? dt::DateValueStateV1::value : dt::DateValueStateV1::sql_null;

  // T05: only the nested production SBDVAL01 decoder may publish a datatype
  // diagnostic. The frame is passed unchanged with exact profile/nullability.
  const auto decoded =
      dt::DecodeDateSbdvalComposedNoAllocV1(*profile, null_allowed, frame);
  if (!decoded.ok())
    return NestedFailure(decoded.diagnostic.diagnostic_code, 0x0f);
  const dt::DateOwnedValueV1 inner_value{profile, decoded.value.state,
                                              decoded.value.day};
  const auto inner_reencoded = dt::EncodeDateSbdvalComposedV1(
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
    const fs::path& path, std::span<const platform::byte> image) {
  std::error_code error;
  fs::remove(path, error);
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
  fs::remove(path, error);
  return restored;
}

void TestComponentFrames(const std::shared_ptr<const dt::DateValidatedProfileHandleV1>& profile) {
  std::vector<platform::byte> image;
  for (const auto& vector : kComponentVectors) {
    const dt::DateOwnedValueV1 value{profile, vector.state, vector.day};
    const auto expected_sbdval = Hex(vector.sbdval);
    const auto expected_sbdpv = Hex(vector.sbdpv);

    const auto sbdval = dt::EncodeDateSbdvalComposedV1(value, true);
    Require(sbdval.ok() && sbdval.bytes == expected_sbdval,
            "sealed SBDVAL01 vector");
    const auto decoded_sbdval =
        dt::DecodeDateSbdvalComposedNoAllocV1(*profile, true, sbdval.bytes);
    Require(decoded_sbdval.ok() &&
                decoded_sbdval.value.state == vector.state &&
                decoded_sbdval.value.day == vector.day,
            "SBDVAL01 composed round trip");

    const auto sbdpv = dt::EncodeDateSbdpvComposedV1(value, true);
    Require(sbdpv.ok() && sbdpv.bytes == expected_sbdpv,
            "sealed SBDPV001 vector");
    const auto decoded_sbdpv =
        dt::DecodeDateSbdpvComposedNoAllocV1(*profile, true, sbdpv.bytes);
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

void TestOraclePositives(const std::shared_ptr<const dt::DateValidatedProfileHandleV1>& profile) {
  std::vector<platform::byte> image;
  for (std::size_t index = 0; index < kOraclePositiveVectors.size(); ++index) {
    const auto& vector = kOraclePositiveVectors[index];
    Require(vector.name == kOraclePositiveNames[index],
            "SBDTOR01 exact positive roster");
    const dt::DateOwnedValueV1 value{profile, vector.state, vector.day};
    const auto production_frame = dt::EncodeDateSbdvalComposedV1(value, true);
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

void TestOracleMutations(const std::shared_ptr<const dt::DateValidatedProfileHandleV1>& profile) {
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

int main() {
  const auto profile = std::make_shared<const dt::DateValidatedProfileHandleV1>(Profile());
  TestComponentFrames(profile);
  TestRawProfileRefusals();
  TestOraclePositives(profile);
  TestOracleMutations(profile);
  return 0;
}
