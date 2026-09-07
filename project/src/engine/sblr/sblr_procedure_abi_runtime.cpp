#include "sblr_procedure_abi_runtime.hpp"

#include "core/hash/hash_digest.hpp"

#include <algorithm>
#include <cstring>
#include <span>
#include <string_view>

namespace scratchbird::engine::sblr {
namespace {

void Put(std::vector<std::uint8_t>* out, std::uint64_t value,
         std::size_t bytes) {
  for (std::size_t index = 0; index < bytes; ++index) {
    out->push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
  }
}

void PutAt(std::vector<std::uint8_t>* out, std::size_t offset,
           std::uint64_t value, std::size_t bytes) {
  for (std::size_t index = 0; index < bytes; ++index) {
    (*out)[offset + index] =
        static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

std::uint64_t Get(const std::uint8_t* bytes, std::size_t count) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < count; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
  }
  return value;
}

template <class T>
bool Nonzero(const T& value) {
  return std::ranges::any_of(value,
                             [](std::uint8_t byte) { return byte != 0; });
}

bool UuidV7(const ProcedureAbiUuid& value) {
  return Nonzero(value) && (value[6] & 0xf0U) == 0x70U &&
         (value[8] & 0xc0U) == 0x80U;
}

bool ValidUtf8(std::string_view value) {
  const auto* bytes =
      reinterpret_cast<const unsigned char*>(value.data());
  std::size_t index = 0;
  while (index < value.size()) {
    const auto lead = bytes[index];
    if (lead == 0) return false;
    if (lead < 0x80U) {
      ++index;
      continue;
    }
    std::size_t continuation = 0;
    std::uint32_t codepoint = 0;
    if ((lead & 0xe0U) == 0xc0U) {
      continuation = 1;
      codepoint = lead & 0x1fU;
      if (codepoint < 2) return false;
    } else if ((lead & 0xf0U) == 0xe0U) {
      continuation = 2;
      codepoint = lead & 0x0fU;
    } else if ((lead & 0xf8U) == 0xf0U) {
      continuation = 3;
      codepoint = lead & 0x07U;
    } else {
      return false;
    }
    if (index + continuation >= value.size()) return false;
    for (std::size_t offset = 1; offset <= continuation; ++offset) {
      const auto byte = bytes[index + offset];
      if ((byte & 0xc0U) != 0x80U) return false;
      codepoint = (codepoint << 6U) | (byte & 0x3fU);
    }
    if ((continuation == 2 && codepoint < 0x800U) ||
        (continuation == 3 && codepoint < 0x10000U) ||
        codepoint > 0x10ffffU ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
      return false;
    }
    index += continuation + 1;
  }
  return !value.empty();
}

ProcedureAbiSha Evidence(std::string_view domain, const std::uint8_t* bytes,
                         std::size_t size) {
  std::vector<std::uint8_t> material(domain.begin(), domain.end());
  material.insert(material.end(), bytes, bytes + size);
  return scratchbird::core::hash::ComputeSha256Digest(material).digest;
}

std::vector<std::uint8_t> Header(const char* magic, std::size_t size) {
  std::vector<std::uint8_t> out(magic, magic + 4);
  Put(&out, 1, 2);
  Put(&out, 16, 2);
  Put(&out, size, 4);
  Put(&out, 0, 4);
  out.resize(size, 0);
  return out;
}

bool ValidHeader(const std::uint8_t* bytes, std::size_t size,
                 const char* magic, std::size_t expected) {
  return bytes != nullptr && size == expected &&
         std::equal(bytes, bytes + 4, magic) && Get(bytes + 4, 2) == 1 &&
         Get(bytes + 6, 2) == 16 && Get(bytes + 8, 4) == expected &&
         std::ranges::all_of(std::span(bytes + 12, 4),
                             [](std::uint8_t byte) { return byte == 0; });
}

bool ValidParameter(const SblrProcedureAbiParameterV1& parameter) {
  return parameter.ordinal == 1 &&
         parameter.mode == SblrProcedureParameterModeV1::kIn &&
         parameter.nullable &&
         parameter.name_utf8.size() >= 1 &&
         parameter.name_utf8.size() <= 64 &&
         parameter.name_utf8.find('.') == std::string::npos &&
         ValidUtf8(parameter.name_utf8) &&
         parameter.canonical_type_name == "int64" &&
         Nonzero(parameter.datatype_descriptor_uuid) &&
         parameter.datatype_descriptor_generation != 0 &&
         Nonzero(parameter.type_uuid);
}

bool ValidArgument(const SblrProcedureArgumentV1& argument) {
  return argument.ordinal == 1 &&
         argument.state == SblrProcedureArgumentStateV1::kValuePresent &&
         Nonzero(argument.datatype_descriptor_uuid) &&
         argument.datatype_descriptor_generation != 0 &&
         Nonzero(argument.type_uuid) &&
         argument.canonical_value_bytes.size() == 8;
}

}  // namespace

std::vector<std::uint8_t> EncodeSblrProcedureAbiV1(
    const SblrProcedureAbiV1& value) {
  if (!UuidV7(value.abi_uuid) || value.abi_generation == 0 ||
      !UuidV7(value.procedure_uuid) || value.procedure_generation == 0 ||
      value.parameters.size() > 1 || value.output_parameter_count != 0 ||
      (!value.parameters.empty() && !ValidParameter(value.parameters.front())) ||
      value.abi_uuid == value.procedure_uuid) {
    return {};
  }
  auto out = Header("PABI", kSblrProcedureAbiV1Bytes);
  std::copy(value.abi_uuid.begin(), value.abi_uuid.end(), out.begin() + 16);
  PutAt(&out, 32, value.abi_generation, 8);
  std::copy(value.procedure_uuid.begin(), value.procedure_uuid.end(),
            out.begin() + 40);
  PutAt(&out, 56, value.procedure_generation, 8);
  PutAt(&out, 64, value.parameters.size(), 2);
  PutAt(&out, 66, value.output_parameter_count, 2);
  if (!value.parameters.empty()) {
    const auto& parameter = value.parameters.front();
    PutAt(&out, 72, parameter.ordinal, 2);
    out[74] = static_cast<std::uint8_t>(parameter.mode);
    out[75] = parameter.nullable ? 1 : 0;
    out[76] = parameter.quoted ? 1 : 0;
    PutAt(&out, 78, parameter.name_utf8.size(), 2);
    PutAt(&out, 80, parameter.canonical_type_name.size(), 2);
    std::copy(parameter.datatype_descriptor_uuid.begin(),
              parameter.datatype_descriptor_uuid.end(), out.begin() + 84);
    PutAt(&out, 100, parameter.datatype_descriptor_generation, 8);
    std::copy(parameter.type_uuid.begin(), parameter.type_uuid.end(),
              out.begin() + 108);
    std::copy(parameter.name_utf8.begin(), parameter.name_utf8.end(),
              out.begin() + 124);
    std::copy(parameter.canonical_type_name.begin(),
              parameter.canonical_type_name.end(), out.begin() + 188);
  }
  const auto evidence = Evidence("ScratchBird.ProcedureAbi.V1",
                                 out.data() + 16, 196);
  if (Nonzero(value.evidence) && value.evidence != evidence) return {};
  std::copy(evidence.begin(), evidence.end(), out.begin() + 212);
  return out;
}

bool DecodeSblrProcedureAbiV1(const std::uint8_t* bytes, std::size_t size,
                              SblrProcedureAbiV1* out,
                              std::string* detail) {
  const auto refuse = [&](const char* reason) {
    if (detail != nullptr) *detail = reason;
    return false;
  };
  if (out == nullptr ||
      !ValidHeader(bytes, size, "PABI", kSblrProcedureAbiV1Bytes) ||
      std::ranges::any_of(std::span(bytes + 68, 4),
                          [](std::uint8_t byte) { return byte != 0; }) ||
      std::ranges::any_of(std::span(bytes + 244, 12),
                          [](std::uint8_t byte) { return byte != 0; })) {
    return refuse("PABI v1 header or reserved bytes invalid");
  }
  SblrProcedureAbiV1 value;
  std::copy_n(bytes + 16, 16, value.abi_uuid.begin());
  value.abi_generation = Get(bytes + 32, 8);
  std::copy_n(bytes + 40, 16, value.procedure_uuid.begin());
  value.procedure_generation = Get(bytes + 56, 8);
  const auto parameter_count = Get(bytes + 64, 2);
  value.output_parameter_count = static_cast<std::uint16_t>(Get(bytes + 66, 2));
  if (parameter_count > 1 || value.output_parameter_count != 0) {
    return refuse("PABI v1 signature count invalid");
  }
  if (parameter_count == 0) {
    if (std::ranges::any_of(std::span(bytes + 72, 140),
                            [](std::uint8_t byte) { return byte != 0; })) {
      return refuse("PABI v1 empty signature record invalid");
    }
  } else {
    SblrProcedureAbiParameterV1 parameter;
    parameter.ordinal = static_cast<std::uint16_t>(Get(bytes + 72, 2));
    parameter.mode = static_cast<SblrProcedureParameterModeV1>(bytes[74]);
    if (bytes[75] > 1 || bytes[76] > 1 || bytes[77] != 0 ||
        Get(bytes + 82, 2) != 0) {
      return refuse("PABI v1 parameter flags invalid");
    }
    parameter.nullable = bytes[75] == 1;
    parameter.quoted = bytes[76] == 1;
    const auto name_size = Get(bytes + 78, 2);
    const auto type_size = Get(bytes + 80, 2);
    if (name_size < 1 || name_size > 64 || type_size < 1 || type_size > 24 ||
        std::ranges::any_of(
            std::span(bytes + 124 + name_size, 64 - name_size),
            [](std::uint8_t byte) { return byte != 0; }) ||
        std::ranges::any_of(
            std::span(bytes + 188 + type_size, 24 - type_size),
            [](std::uint8_t byte) { return byte != 0; })) {
      return refuse("PABI v1 parameter text extent invalid");
    }
    std::copy_n(bytes + 84, 16, parameter.datatype_descriptor_uuid.begin());
    parameter.datatype_descriptor_generation = Get(bytes + 100, 8);
    std::copy_n(bytes + 108, 16, parameter.type_uuid.begin());
    parameter.name_utf8.assign(reinterpret_cast<const char*>(bytes + 124),
                               name_size);
    parameter.canonical_type_name.assign(
        reinterpret_cast<const char*>(bytes + 188), type_size);
    if (!ValidParameter(parameter)) {
      return refuse("PABI v1 parameter authority invalid");
    }
    value.parameters.push_back(std::move(parameter));
  }
  std::copy_n(bytes + 212, 32, value.evidence.begin());
  const auto expected = Evidence("ScratchBird.ProcedureAbi.V1", bytes + 16,
                                 196);
  if (value.evidence != expected) return refuse("PABI v1 evidence invalid");
  const auto canonical = EncodeSblrProcedureAbiV1(value);
  if (canonical.size() != size ||
      !std::equal(canonical.begin(), canonical.end(), bytes)) {
    return refuse("PABI v1 canonical re-encoding differs");
  }
  *out = std::move(value);
  return true;
}

std::vector<std::uint8_t> EncodeSblrProcedureArgumentVectorV1(
    const SblrProcedureArgumentVectorV1& value) {
  if (!UuidV7(value.argument_vector_uuid) ||
      value.argument_vector_generation == 0 ||
      !UuidV7(value.procedure_abi_uuid) ||
      value.procedure_abi_generation == 0 || !UuidV7(value.invocation_uuid) ||
      value.invocation_generation == 0 || value.arguments.size() > 1 ||
      (!value.arguments.empty() && !ValidArgument(value.arguments.front())) ||
      value.argument_vector_uuid == value.procedure_abi_uuid ||
      value.argument_vector_uuid == value.invocation_uuid ||
      value.procedure_abi_uuid == value.invocation_uuid) {
    return {};
  }
  auto out = Header("PARG", kSblrProcedureArgumentVectorV1Bytes);
  std::copy(value.argument_vector_uuid.begin(), value.argument_vector_uuid.end(),
            out.begin() + 16);
  PutAt(&out, 32, value.argument_vector_generation, 8);
  std::copy(value.procedure_abi_uuid.begin(), value.procedure_abi_uuid.end(),
            out.begin() + 40);
  PutAt(&out, 56, value.procedure_abi_generation, 8);
  std::copy(value.invocation_uuid.begin(), value.invocation_uuid.end(),
            out.begin() + 64);
  PutAt(&out, 80, value.invocation_generation, 8);
  PutAt(&out, 88, value.arguments.size(), 2);
  if (!value.arguments.empty()) {
    const auto& argument = value.arguments.front();
    PutAt(&out, 92, argument.ordinal, 2);
    out[94] = static_cast<std::uint8_t>(argument.state);
    std::copy(argument.datatype_descriptor_uuid.begin(),
              argument.datatype_descriptor_uuid.end(), out.begin() + 96);
    PutAt(&out, 112, argument.datatype_descriptor_generation, 8);
    std::copy(argument.type_uuid.begin(), argument.type_uuid.end(),
              out.begin() + 120);
    PutAt(&out, 136, argument.canonical_value_bytes.size(), 2);
    std::copy(argument.canonical_value_bytes.begin(),
              argument.canonical_value_bytes.end(), out.begin() + 140);
  }
  const auto evidence = Evidence("ScratchBird.ProcedureArgumentVector.V1",
                                 out.data() + 16, 140);
  if (Nonzero(value.evidence) && value.evidence != evidence) return {};
  std::copy(evidence.begin(), evidence.end(), out.begin() + 156);
  return out;
}

bool DecodeSblrProcedureArgumentVectorV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrProcedureArgumentVectorV1* out, std::string* detail) {
  const auto refuse = [&](const char* reason) {
    if (detail != nullptr) *detail = reason;
    return false;
  };
  if (out == nullptr ||
      !ValidHeader(bytes, size, "PARG",
                   kSblrProcedureArgumentVectorV1Bytes) ||
      Get(bytes + 90, 2) != 0 || bytes[95] != 0 || Get(bytes + 138, 2) != 0 ||
      std::ranges::any_of(std::span(bytes + 188, 4),
                          [](std::uint8_t byte) { return byte != 0; })) {
    return refuse("PARG v1 header or reserved bytes invalid");
  }
  SblrProcedureArgumentVectorV1 value;
  std::copy_n(bytes + 16, 16, value.argument_vector_uuid.begin());
  value.argument_vector_generation = Get(bytes + 32, 8);
  std::copy_n(bytes + 40, 16, value.procedure_abi_uuid.begin());
  value.procedure_abi_generation = Get(bytes + 56, 8);
  std::copy_n(bytes + 64, 16, value.invocation_uuid.begin());
  value.invocation_generation = Get(bytes + 80, 8);
  const auto argument_count = Get(bytes + 88, 2);
  if (argument_count > 1) return refuse("PARG v1 argument count invalid");
  if (argument_count == 0) {
    if (std::ranges::any_of(std::span(bytes + 92, 64),
                            [](std::uint8_t byte) { return byte != 0; })) {
      return refuse("PARG v1 empty argument record invalid");
    }
  } else {
    SblrProcedureArgumentV1 argument;
    argument.ordinal = static_cast<std::uint16_t>(Get(bytes + 92, 2));
    argument.state = static_cast<SblrProcedureArgumentStateV1>(bytes[94]);
    std::copy_n(bytes + 96, 16, argument.datatype_descriptor_uuid.begin());
    argument.datatype_descriptor_generation = Get(bytes + 112, 8);
    std::copy_n(bytes + 120, 16, argument.type_uuid.begin());
    const auto value_size = Get(bytes + 136, 2);
    if (value_size != 8 ||
        std::ranges::any_of(std::span(bytes + 148, 8),
                            [](std::uint8_t byte) { return byte != 0; })) {
      return refuse("PARG v1 value extent invalid");
    }
    argument.canonical_value_bytes.assign(bytes + 140,
                                          bytes + 140 + value_size);
    if (!ValidArgument(argument)) {
      return refuse("PARG v1 argument authority invalid");
    }
    value.arguments.push_back(std::move(argument));
  }
  std::copy_n(bytes + 156, 32, value.evidence.begin());
  const auto expected = Evidence("ScratchBird.ProcedureArgumentVector.V1",
                                 bytes + 16, 140);
  if (value.evidence != expected) return refuse("PARG v1 evidence invalid");
  const auto canonical = EncodeSblrProcedureArgumentVectorV1(value);
  if (canonical.size() != size ||
      !std::equal(canonical.begin(), canonical.end(), bytes)) {
    return refuse("PARG v1 canonical re-encoding differs");
  }
  *out = std::move(value);
  return true;
}

std::array<std::uint8_t, 8> EncodeSblrProcedureInt64Value(
    std::int64_t value) {
  std::array<std::uint8_t, 8> bytes{};
  const auto encoded = static_cast<std::uint64_t>(value);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(encoded >> (index * 8U));
  }
  return bytes;
}

bool DecodeSblrProcedureInt64Value(const std::vector<std::uint8_t>& bytes,
                                   std::int64_t* out) {
  if (out == nullptr || bytes.size() != 8) return false;
  *out = static_cast<std::int64_t>(Get(bytes.data(), bytes.size()));
  return true;
}

}  // namespace scratchbird::engine::sblr
