#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

using ProcedureAbiUuid = std::array<std::uint8_t, 16>;
using ProcedureAbiSha = std::array<std::uint8_t, 32>;

inline constexpr std::size_t kSblrProcedureAbiV1Bytes = 256;
inline constexpr std::size_t kSblrProcedureArgumentVectorV1Bytes = 192;

enum class SblrProcedureParameterModeV1 : std::uint8_t {
  kIn = 1,
};

enum class SblrProcedureArgumentStateV1 : std::uint8_t {
  kValuePresent = 1,
};

struct SblrProcedureAbiParameterV1 {
  std::uint16_t ordinal = 0;
  SblrProcedureParameterModeV1 mode{SblrProcedureParameterModeV1::kIn};
  bool nullable = true;
  bool quoted = false;
  std::string name_utf8;
  std::string canonical_type_name;
  ProcedureAbiUuid datatype_descriptor_uuid{};
  std::uint64_t datatype_descriptor_generation = 0;
  ProcedureAbiUuid type_uuid{};
};

// PABI v1 is deliberately closed to either the existing empty signature or
// the first executable profile: one nullable IN int64 parameter.  Extending
// the parameter count or type set requires a new carrier version.
struct SblrProcedureAbiV1 {
  ProcedureAbiUuid abi_uuid{};
  std::uint64_t abi_generation = 0;
  ProcedureAbiUuid procedure_uuid{};
  std::uint64_t procedure_generation = 0;
  std::vector<SblrProcedureAbiParameterV1> parameters;
  std::uint16_t output_parameter_count = 0;
  ProcedureAbiSha evidence{};
};

struct SblrProcedureArgumentV1 {
  std::uint16_t ordinal = 0;
  SblrProcedureArgumentStateV1 state{
      SblrProcedureArgumentStateV1::kValuePresent};
  ProcedureAbiUuid datatype_descriptor_uuid{};
  std::uint64_t datatype_descriptor_generation = 0;
  ProcedureAbiUuid type_uuid{};
  std::vector<std::uint8_t> canonical_value_bytes;
};

// PARG v1 is similarly closed to zero arguments or one canonical int64 value.
struct SblrProcedureArgumentVectorV1 {
  ProcedureAbiUuid argument_vector_uuid{};
  std::uint64_t argument_vector_generation = 0;
  ProcedureAbiUuid procedure_abi_uuid{};
  std::uint64_t procedure_abi_generation = 0;
  ProcedureAbiUuid invocation_uuid{};
  std::uint64_t invocation_generation = 0;
  std::vector<SblrProcedureArgumentV1> arguments;
  ProcedureAbiSha evidence{};
};

std::vector<std::uint8_t> EncodeSblrProcedureAbiV1(
    const SblrProcedureAbiV1& value);
bool DecodeSblrProcedureAbiV1(const std::uint8_t* bytes, std::size_t size,
                              SblrProcedureAbiV1* out,
                              std::string* detail = nullptr);

std::vector<std::uint8_t> EncodeSblrProcedureArgumentVectorV1(
    const SblrProcedureArgumentVectorV1& value);
bool DecodeSblrProcedureArgumentVectorV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrProcedureArgumentVectorV1* out, std::string* detail = nullptr);

std::array<std::uint8_t, 8> EncodeSblrProcedureInt64Value(
    std::int64_t value);
bool DecodeSblrProcedureInt64Value(const std::vector<std::uint8_t>& bytes,
                                   std::int64_t* out);

}  // namespace scratchbird::engine::sblr
