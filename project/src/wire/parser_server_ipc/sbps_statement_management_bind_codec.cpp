#include "sbps_statement_management_bind_codec.hpp"

#include "hash_digest.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string_view>

namespace scratchbird::wire::sbps_statement_management {
namespace {

constexpr std::size_t kPrepareRequestPrefixBytes = 160;
constexpr std::size_t kPrepareAckBytes = 176;
constexpr std::size_t kExecuteDirectRequestPrefixBytes = 184;
constexpr std::size_t kExecuteDirectAckBytes = 176;
constexpr std::size_t kQueryExplainBindRequestPrefixBytes = 184;
constexpr std::size_t kQueryExplainBindAckBytes = 176;
constexpr std::size_t kNameResolveBindRequestPrefixBytes = 160;
constexpr std::size_t kNameResolveBindAckBytes = 176;
constexpr std::size_t kCatalogEpochCheckBindRequestPrefixBytes = 128;
constexpr std::size_t kCatalogEpochCheckBindAckBytes = 256;
constexpr std::size_t kDatabaseAttachBindRequestPrefixBytes = 96;
constexpr std::size_t kDatabaseAttachBindAckBytes = 256;
constexpr std::size_t kParseTextBindRequestPrefixBytes = 224;
constexpr std::size_t kParseTextBindAckBytes = 208;
constexpr std::size_t kFreeRequestPrefixBytes = 96;
constexpr std::size_t kFreeAckBytes = 208;
constexpr std::size_t kCancelRequestPrefixBytes = 112;
constexpr std::size_t kCancelAckBytes = 272;
constexpr std::size_t kParameterBindRequestPrefixBytes = 256;
constexpr std::size_t kMaximumStatementNameBytes = 256;
constexpr std::size_t kMaximumParameterDemandBytes = 64 * 1024;
constexpr std::size_t kMaximumCanonicalCarrierBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaximumParameterValueBytes = 32 * 1024 * 1024;
constexpr std::size_t kMaximumParseTextInputBytes = 16 * 1024 * 1024;

bool Fail(std::string* detail, const char* reason) {
  if (detail != nullptr) *detail = reason;
  return false;
}

template <typename T>
bool NonZero(const T& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

void PutU16(std::vector<std::uint8_t>* output, std::uint16_t value) {
  output->push_back(static_cast<std::uint8_t>(value));
  output->push_back(static_cast<std::uint8_t>(value >> 8U));
}

void PutU32(std::vector<std::uint8_t>* output, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    output->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void PutU64(std::vector<std::uint8_t>* output, std::uint64_t value) {
  for (unsigned shift = 0; shift != 64; shift += 8) {
    output->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

std::uint16_t GetU16(const std::uint8_t* value) {
  return static_cast<std::uint16_t>(value[0]) |
         static_cast<std::uint16_t>(value[1]) << 8U;
}

std::uint32_t GetU32(const std::uint8_t* value) {
  std::uint32_t output = 0;
  for (unsigned index = 0; index != 4; ++index) {
    output |= static_cast<std::uint32_t>(value[index]) << (index * 8U);
  }
  return output;
}

std::uint64_t GetU64(const std::uint8_t* value) {
  std::uint64_t output = 0;
  for (unsigned index = 0; index != 8; ++index) {
    output |= static_cast<std::uint64_t>(value[index]) << (index * 8U);
  }
  return output;
}

template <std::size_t N>
void Put(std::vector<std::uint8_t>* output,
         const std::array<std::uint8_t, N>& value) {
  output->insert(output->end(), value.begin(), value.end());
}

template <std::size_t N>
void Get(const std::uint8_t* input, std::array<std::uint8_t, N>* output) {
  std::copy_n(input, N, output->begin());
}

Hash256 Hash(const std::uint8_t* bytes, std::size_t size) {
  std::vector<std::uint8_t> material(bytes, bytes + size);
  return scratchbird::core::hash::ComputeSha256Digest(material).digest;
}

Hash256 Hash(const std::vector<std::uint8_t>& bytes) {
  return scratchbird::core::hash::ComputeSha256Digest(bytes).digest;
}

bool IsValidUtf8Extent(std::string_view text, std::size_t maximum_bytes) {
  if (text.empty() || text.size() > maximum_bytes ||
      text.find('\0') != std::string_view::npos) {
    return false;
  }
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
  std::size_t offset = 0;
  while (offset < text.size()) {
    const auto lead = bytes[offset++];
    if (lead < 0x80) continue;
    if (lead >= 0xc2 && lead <= 0xdf) {
      if (offset >= text.size() || (bytes[offset++] & 0xc0U) != 0x80U)
        return false;
      continue;
    }
    if (lead >= 0xe0 && lead <= 0xef) {
      if (offset + 1 >= text.size() ||
          (bytes[offset] & 0xc0U) != 0x80U ||
          (bytes[offset + 1] & 0xc0U) != 0x80U ||
          (lead == 0xe0 && bytes[offset] < 0xa0) ||
          (lead == 0xed && bytes[offset] >= 0xa0)) return false;
      offset += 2;
      continue;
    }
    if (lead >= 0xf0 && lead <= 0xf4) {
      if (offset + 2 >= text.size() ||
          (bytes[offset] & 0xc0U) != 0x80U ||
          (bytes[offset + 1] & 0xc0U) != 0x80U ||
          (bytes[offset + 2] & 0xc0U) != 0x80U ||
          (lead == 0xf0 && bytes[offset] < 0x90) ||
          (lead == 0xf4 && bytes[offset] >= 0x90)) return false;
      offset += 3;
      continue;
    }
    return false;
  }
  return true;
}

bool IsValidUtf8(std::string_view text) {
  return IsValidUtf8Extent(text, kMaximumStatementNameBytes);
}

bool IsRegularIdentifier(std::string_view text) {
  if (!IsValidUtf8(text) ||
      !((text[0] >= 'A' && text[0] <= 'Z') ||
        (text[0] >= 'a' && text[0] <= 'z') || text[0] == '_')) {
    return false;
  }
  return std::all_of(text.begin() + 1, text.end(), [](unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$';
  });
}

bool ValidName(std::string_view name, bool quoted) {
  return quoted ? IsValidUtf8(name) : IsRegularIdentifier(name);
}

std::vector<std::uint8_t> Header(std::string_view magic,
                                 std::size_t prefix_bytes,
                                 std::size_t total_bytes) {
  std::vector<std::uint8_t> output;
  output.reserve(total_bytes);
  output.insert(output.end(), magic.begin(), magic.end());
  PutU16(&output, 1);
  PutU16(&output, static_cast<std::uint16_t>(prefix_bytes));
  PutU32(&output, static_cast<std::uint32_t>(total_bytes));
  PutU32(&output, 0);
  return output;
}

bool HeaderValid(const std::uint8_t* bytes, std::size_t size,
                 std::string_view magic, std::size_t prefix_bytes,
                 bool fixed) {
  return bytes != nullptr && size >= prefix_bytes && magic.size() == 4 &&
         std::equal(magic.begin(), magic.end(), bytes) &&
         GetU16(bytes + 4) == 1 && GetU16(bytes + 6) == prefix_bytes &&
         GetU32(bytes + 8) == size && GetU32(bytes + 12) == 0 &&
         (!fixed || size == prefix_bytes);
}

template <std::size_t Begin, std::size_t End>
Hash256 Evidence(std::vector<std::uint8_t> bytes) {
  static_assert(Begin < End);
  if (bytes.size() < End) return {};
  std::fill(bytes.begin() + Begin, bytes.begin() + End, 0);
  return Hash(bytes.data(), bytes.size());
}

}  // namespace

bool EncodePrepareBindRequestV1(const PrepareBindRequestV1& value,
                                std::vector<std::uint8_t>* output,
                                std::string* detail) {
  if (output == nullptr) return Fail(detail, "stmt_prepare_bind.output_missing");
  output->clear();
  const auto suffix_bytes = value.statement_name.size() +
      value.declared_parameter_type_demands.size() +
      value.canonical_container_bytes.size() +
      value.canonical_execution_envelope_bytes.size();
  if (!NonZero(value.authenticated_receipt_uuid) || value.occurrence == 0 ||
      !ValidName(value.statement_name, value.quoted) ||
      value.declared_parameter_type_demands.size() > kMaximumParameterDemandBytes ||
      value.canonical_container_bytes.empty() ||
      value.canonical_execution_envelope_bytes.empty() ||
      suffix_bytes > kMaximumCanonicalCarrierBytes ||
      suffix_bytes > std::numeric_limits<std::uint32_t>::max()) {
    return Fail(detail, "stmt_prepare_bind.fields_invalid");
  }
  const auto container_hash = Hash(value.canonical_container_bytes.data(),
                                   value.canonical_container_bytes.size());
  const auto execution_hash = Hash(
      value.canonical_execution_envelope_bytes.data(),
      value.canonical_execution_envelope_bytes.size());
  if ((NonZero(value.canonical_container_sha256) &&
       value.canonical_container_sha256 != container_hash) ||
      (NonZero(value.canonical_execution_envelope_sha256) &&
       value.canonical_execution_envelope_sha256 != execution_hash)) {
    return Fail(detail, "stmt_prepare_bind.body_hash_invalid");
  }
  *output = Header("SPBQ", kPrepareRequestPrefixBytes,
                   kPrepareRequestPrefixBytes + suffix_bytes);
  Put(output, value.authenticated_receipt_uuid);
  PutU64(output, value.occurrence);
  PutU32(output, value.quoted ? 1U : 0U);
  PutU32(output, static_cast<std::uint32_t>(value.statement_name.size()));
  PutU32(output, static_cast<std::uint32_t>(
                     value.declared_parameter_type_demands.size()));
  PutU32(output, static_cast<std::uint32_t>(
                     value.canonical_container_bytes.size()));
  PutU32(output, static_cast<std::uint32_t>(
                     value.canonical_execution_envelope_bytes.size()));
  PutU32(output, 0);
  Put(output, container_hash);
  Put(output, execution_hash);
  output->insert(output->end(), 32, 0);
  output->insert(output->end(), value.statement_name.begin(),
                 value.statement_name.end());
  output->insert(output->end(), value.declared_parameter_type_demands.begin(),
                 value.declared_parameter_type_demands.end());
  output->insert(output->end(), value.canonical_container_bytes.begin(),
                 value.canonical_container_bytes.end());
  output->insert(output->end(), value.canonical_execution_envelope_bytes.begin(),
                 value.canonical_execution_envelope_bytes.end());
  const auto evidence = Evidence<128, 160>(*output);
  if (NonZero(value.request_evidence_sha256) &&
      value.request_evidence_sha256 != evidence) {
    output->clear();
    return Fail(detail, "stmt_prepare_bind.request_evidence_invalid");
  }
  std::copy(evidence.begin(), evidence.end(), output->begin() + 128);
  return true;
}

bool DecodePrepareBindRequestV1(const std::uint8_t* bytes, std::size_t size,
                                PrepareBindRequestV1* output,
                                std::string* detail) {
  if (output == nullptr ||
      !HeaderValid(bytes, size, "SPBQ", kPrepareRequestPrefixBytes, false)) {
    return Fail(detail, "stmt_prepare_bind.header_invalid");
  }
  PrepareBindRequestV1 value;
  Get(bytes + 16, &value.authenticated_receipt_uuid);
  value.occurrence = GetU64(bytes + 32);
  const auto flags = GetU32(bytes + 40);
  const auto name_bytes = GetU32(bytes + 44);
  const auto parameter_bytes = GetU32(bytes + 48);
  const auto container_bytes = GetU32(bytes + 52);
  const auto execution_bytes = GetU32(bytes + 56);
  if ((flags & ~1U) != 0 || GetU32(bytes + 60) != 0 ||
      name_bytes == 0 || name_bytes > kMaximumStatementNameBytes ||
      parameter_bytes > kMaximumParameterDemandBytes ||
      container_bytes == 0 || execution_bytes == 0 ||
      static_cast<std::uint64_t>(name_bytes) + parameter_bytes +
              container_bytes + execution_bytes !=
          size - kPrepareRequestPrefixBytes) {
    return Fail(detail, "stmt_prepare_bind.extent_invalid");
  }
  value.quoted = (flags & 1U) != 0;
  Get(bytes + 64, &value.canonical_container_sha256);
  Get(bytes + 96, &value.canonical_execution_envelope_sha256);
  Get(bytes + 128, &value.request_evidence_sha256);
  std::size_t offset = kPrepareRequestPrefixBytes;
  value.statement_name.assign(reinterpret_cast<const char*>(bytes + offset),
                              name_bytes);
  offset += name_bytes;
  value.declared_parameter_type_demands.assign(bytes + offset,
                                                bytes + offset + parameter_bytes);
  offset += parameter_bytes;
  value.canonical_container_bytes.assign(bytes + offset,
                                         bytes + offset + container_bytes);
  offset += container_bytes;
  value.canonical_execution_envelope_bytes.assign(
      bytes + offset, bytes + offset + execution_bytes);
  std::vector<std::uint8_t> canonical;
  if (!EncodePrepareBindRequestV1(value, &canonical, detail) ||
      canonical.size() != size || !std::equal(canonical.begin(), canonical.end(), bytes)) {
    return Fail(detail, "stmt_prepare_bind.noncanonical");
  }
  *output = std::move(value);
  return true;
}

Hash256 PrepareBindRequestEvidenceV1(const PrepareBindRequestV1& value) {
  auto copy = value;
  copy.request_evidence_sha256 = {};
  std::vector<std::uint8_t> bytes;
  return EncodePrepareBindRequestV1(copy, &bytes) ? Evidence<128, 160>(bytes)
                                                  : Hash256{};
}

bool EncodePrepareBindAckV1(const PrepareBindAckV1& value,
                            std::vector<std::uint8_t>* output,
                            std::string* detail) {
  if (output == nullptr) return Fail(detail, "stmt_prepare_bind_ack.output_missing");
  output->clear();
  if (!NonZero(value.authenticated_receipt_uuid) || value.occurrence == 0 ||
      !NonZero(value.binding_uuid) || value.binding_generation == 0 ||
      !NonZero(value.statement_name_uuid) || !NonZero(value.descriptor_sha256) ||
      !NonZero(value.request_evidence_sha256)) {
    return Fail(detail, "stmt_prepare_bind_ack.fields_invalid");
  }
  *output = Header("SPBA", kPrepareAckBytes, kPrepareAckBytes);
  Put(output, value.authenticated_receipt_uuid);
  PutU64(output, value.occurrence);
  Put(output, value.binding_uuid);
  PutU64(output, value.binding_generation);
  Put(output, value.statement_name_uuid);
  Put(output, value.descriptor_sha256);
  Put(output, value.request_evidence_sha256);
  output->insert(output->end(), 32, 0);
  const auto evidence = Evidence<144, 176>(*output);
  if (NonZero(value.acknowledgement_evidence_sha256) &&
      value.acknowledgement_evidence_sha256 != evidence) {
    output->clear();
    return Fail(detail, "stmt_prepare_bind_ack.evidence_invalid");
  }
  std::copy(evidence.begin(), evidence.end(), output->begin() + 144);
  return true;
}

bool DecodePrepareBindAckV1(const std::uint8_t* bytes, std::size_t size,
                            PrepareBindAckV1* output, std::string* detail) {
  if (output == nullptr ||
      !HeaderValid(bytes, size, "SPBA", kPrepareAckBytes, true)) {
    return Fail(detail, "stmt_prepare_bind_ack.header_invalid");
  }
  PrepareBindAckV1 value;
  Get(bytes + 16, &value.authenticated_receipt_uuid);
  value.occurrence = GetU64(bytes + 32);
  Get(bytes + 40, &value.binding_uuid);
  value.binding_generation = GetU64(bytes + 56);
  Get(bytes + 64, &value.statement_name_uuid);
  Get(bytes + 80, &value.descriptor_sha256);
  Get(bytes + 112, &value.request_evidence_sha256);
  Get(bytes + 144, &value.acknowledgement_evidence_sha256);
  std::vector<std::uint8_t> canonical;
  if (!EncodePrepareBindAckV1(value, &canonical, detail) ||
      !std::equal(canonical.begin(), canonical.end(), bytes)) {
    return Fail(detail, "stmt_prepare_bind_ack.noncanonical");
  }
  *output = value;
  return true;
}

Hash256 PrepareBindAckEvidenceV1(const PrepareBindAckV1& value) {
  auto copy = value;
  copy.acknowledgement_evidence_sha256 = {};
  std::vector<std::uint8_t> bytes;
  return EncodePrepareBindAckV1(copy, &bytes) ? Evidence<144, 176>(bytes)
                                              : Hash256{};
}

bool EncodeExecuteDirectBindRequestV1(
    const ExecuteDirectBindRequestV1& value,
    std::vector<std::uint8_t>* output, std::string* detail) {
  if (output == nullptr) {
    return Fail(detail, "stmt_execute_direct_bind.output_missing");
  }
  output->clear();
  if (!NonZero(value.authenticated_receipt_uuid) || value.occurrence == 0 ||
      value.canonical_container_bytes.empty() ||
      value.canonical_execution_envelope_bytes.empty() ||
      value.canonical_container_bytes.size() > kMaximumCanonicalCarrierBytes ||
      value.canonical_execution_envelope_bytes.size() >
          kMaximumCanonicalCarrierBytes ||
      value.canonical_parameter_bytes.size() > kMaximumParameterDemandBytes) {
    return Fail(detail, "stmt_execute_direct_bind.fields_invalid");
  }
  const auto container_hash = Hash(value.canonical_container_bytes);
  const auto execution_hash = Hash(value.canonical_execution_envelope_bytes);
  const auto parameter_hash = Hash(value.canonical_parameter_bytes);
  if ((NonZero(value.canonical_container_sha256) &&
       value.canonical_container_sha256 != container_hash) ||
      (NonZero(value.canonical_execution_envelope_sha256) &&
       value.canonical_execution_envelope_sha256 != execution_hash) ||
      (NonZero(value.canonical_parameter_sha256) &&
       value.canonical_parameter_sha256 != parameter_hash)) {
    return Fail(detail, "stmt_execute_direct_bind.body_hash_invalid");
  }
  const std::uint64_t suffix_size =
      static_cast<std::uint64_t>(value.canonical_container_bytes.size()) +
      value.canonical_execution_envelope_bytes.size() +
      value.canonical_parameter_bytes.size();
  if (suffix_size > kMaximumCanonicalCarrierBytes ||
      suffix_size > std::numeric_limits<std::uint32_t>::max()) {
    return Fail(detail, "stmt_execute_direct_bind.extent_invalid");
  }
  *output = Header("SDBQ", kExecuteDirectRequestPrefixBytes,
                   kExecuteDirectRequestPrefixBytes + suffix_size);
  Put(output, value.authenticated_receipt_uuid);
  PutU64(output, value.occurrence);
  PutU32(output, static_cast<std::uint32_t>(
                     value.canonical_container_bytes.size()));
  PutU32(output, static_cast<std::uint32_t>(
                     value.canonical_execution_envelope_bytes.size()));
  PutU32(output,
         static_cast<std::uint32_t>(value.canonical_parameter_bytes.size()));
  PutU32(output, 0);
  Put(output, container_hash);
  Put(output, execution_hash);
  Put(output, parameter_hash);
  output->insert(output->end(), 32, 0);
  output->insert(output->end(), value.canonical_container_bytes.begin(),
                 value.canonical_container_bytes.end());
  output->insert(output->end(),
                 value.canonical_execution_envelope_bytes.begin(),
                 value.canonical_execution_envelope_bytes.end());
  output->insert(output->end(), value.canonical_parameter_bytes.begin(),
                 value.canonical_parameter_bytes.end());
  const auto evidence = Evidence<152, 184>(*output);
  if (NonZero(value.request_evidence_sha256) &&
      value.request_evidence_sha256 != evidence) {
    output->clear();
    return Fail(detail, "stmt_execute_direct_bind.request_evidence_invalid");
  }
  std::copy(evidence.begin(), evidence.end(), output->begin() + 152);
  return true;
}

bool DecodeExecuteDirectBindRequestV1(
    const std::uint8_t* bytes, std::size_t size,
    ExecuteDirectBindRequestV1* output, std::string* detail) {
  if (output == nullptr ||
      !HeaderValid(bytes, size, "SDBQ", kExecuteDirectRequestPrefixBytes,
                   false)) {
    return Fail(detail, "stmt_execute_direct_bind.header_invalid");
  }
  ExecuteDirectBindRequestV1 value;
  Get(bytes + 16, &value.authenticated_receipt_uuid);
  value.occurrence = GetU64(bytes + 32);
  const auto container_size = GetU32(bytes + 40);
  const auto execution_size = GetU32(bytes + 44);
  const auto parameter_size = GetU32(bytes + 48);
  if (GetU32(bytes + 52) != 0 || container_size == 0 ||
      execution_size == 0 ||
      static_cast<std::uint64_t>(container_size) + execution_size +
              parameter_size !=
          size - kExecuteDirectRequestPrefixBytes) {
    return Fail(detail, "stmt_execute_direct_bind.extent_invalid");
  }
  Get(bytes + 56, &value.canonical_container_sha256);
  Get(bytes + 88, &value.canonical_execution_envelope_sha256);
  Get(bytes + 120, &value.canonical_parameter_sha256);
  Get(bytes + 152, &value.request_evidence_sha256);
  std::size_t offset = kExecuteDirectRequestPrefixBytes;
  value.canonical_container_bytes.assign(bytes + offset,
                                         bytes + offset + container_size);
  offset += container_size;
  value.canonical_execution_envelope_bytes.assign(
      bytes + offset, bytes + offset + execution_size);
  offset += execution_size;
  value.canonical_parameter_bytes.assign(bytes + offset,
                                         bytes + offset + parameter_size);
  std::vector<std::uint8_t> canonical;
  if (!EncodeExecuteDirectBindRequestV1(value, &canonical, detail) ||
      canonical.size() != size ||
      !std::equal(canonical.begin(), canonical.end(), bytes)) {
    return Fail(detail, "stmt_execute_direct_bind.noncanonical");
  }
  *output = std::move(value);
  return true;
}

Hash256 ExecuteDirectBindRequestEvidenceV1(
    const ExecuteDirectBindRequestV1& value) {
  auto copy = value;
  copy.request_evidence_sha256 = {};
  std::vector<std::uint8_t> bytes;
  return EncodeExecuteDirectBindRequestV1(copy, &bytes)
             ? Evidence<152, 184>(bytes)
             : Hash256{};
}

bool EncodeExecuteDirectBindAckV1(const ExecuteDirectBindAckV1& value,
                                  std::vector<std::uint8_t>* output,
                                  std::string* detail) {
  if (output == nullptr) {
    return Fail(detail, "stmt_execute_direct_bind_ack.output_missing");
  }
  output->clear();
  if (!NonZero(value.authenticated_receipt_uuid) || value.occurrence == 0 ||
      !NonZero(value.binding_uuid) || value.binding_generation == 0 ||
      !NonZero(value.descriptor_sha256) ||
      !NonZero(value.request_evidence_sha256)) {
    return Fail(detail, "stmt_execute_direct_bind_ack.fields_invalid");
  }
  *output = Header("SDBA", kExecuteDirectAckBytes, kExecuteDirectAckBytes);
  Put(output, value.authenticated_receipt_uuid);
  PutU64(output, value.occurrence);
  Put(output, value.binding_uuid);
  PutU64(output, value.binding_generation);
  Put(output, value.result_descriptor_uuid);
  Put(output, value.descriptor_sha256);
  Put(output, value.request_evidence_sha256);
  output->insert(output->end(), 32, 0);
  const auto evidence = Evidence<144, 176>(*output);
  if (NonZero(value.acknowledgement_evidence_sha256) &&
      value.acknowledgement_evidence_sha256 != evidence) {
    output->clear();
    return Fail(detail, "stmt_execute_direct_bind_ack.evidence_invalid");
  }
  std::copy(evidence.begin(), evidence.end(), output->begin() + 144);
  return true;
}

bool DecodeExecuteDirectBindAckV1(const std::uint8_t* bytes,
                                  std::size_t size,
                                  ExecuteDirectBindAckV1* output,
                                  std::string* detail) {
  if (output == nullptr ||
      !HeaderValid(bytes, size, "SDBA", kExecuteDirectAckBytes, true)) {
    return Fail(detail, "stmt_execute_direct_bind_ack.header_invalid");
  }
  ExecuteDirectBindAckV1 value;
  Get(bytes + 16, &value.authenticated_receipt_uuid);
  value.occurrence = GetU64(bytes + 32);
  Get(bytes + 40, &value.binding_uuid);
  value.binding_generation = GetU64(bytes + 56);
  Get(bytes + 64, &value.result_descriptor_uuid);
  Get(bytes + 80, &value.descriptor_sha256);
  Get(bytes + 112, &value.request_evidence_sha256);
  Get(bytes + 144, &value.acknowledgement_evidence_sha256);
  std::vector<std::uint8_t> canonical;
  if (!EncodeExecuteDirectBindAckV1(value, &canonical, detail) ||
      !std::equal(canonical.begin(), canonical.end(), bytes)) {
    return Fail(detail, "stmt_execute_direct_bind_ack.noncanonical");
  }
  *output = value;
  return true;
}

Hash256 ExecuteDirectBindAckEvidenceV1(
    const ExecuteDirectBindAckV1& value) {
  auto copy = value;
  copy.acknowledgement_evidence_sha256 = {};
  std::vector<std::uint8_t> bytes;
  return EncodeExecuteDirectBindAckV1(copy, &bytes)
             ? Evidence<144, 176>(bytes)
             : Hash256{};
}

bool EncodeQueryExplainBindRequestV1(
    const QueryExplainBindRequestV1& value,
    std::vector<std::uint8_t>* output, std::string* detail) {
  if (output == nullptr) {
    return Fail(detail, "query_explain_bind.output_missing");
  }
  output->clear();
  if (!NonZero(value.authenticated_receipt_uuid) || value.occurrence == 0 ||
      (value.format != 1 && value.format != 2) ||
      value.canonical_container_bytes.empty() ||
      value.canonical_execution_envelope_bytes.empty() ||
      value.canonical_container_bytes.size() > kMaximumCanonicalCarrierBytes ||
      value.canonical_execution_envelope_bytes.size() >
          kMaximumCanonicalCarrierBytes) {
    return Fail(detail, "query_explain_bind.fields_invalid");
  }
  const std::uint64_t suffix_size =
      static_cast<std::uint64_t>(value.canonical_container_bytes.size()) +
      value.canonical_execution_envelope_bytes.size();
  if (suffix_size > kMaximumCanonicalCarrierBytes ||
      suffix_size > std::numeric_limits<std::uint32_t>::max()) {
    return Fail(detail, "query_explain_bind.extent_invalid");
  }
  const auto container_hash = Hash(value.canonical_container_bytes);
  const auto execution_hash = Hash(value.canonical_execution_envelope_bytes);
  if ((NonZero(value.canonical_container_sha256) &&
       value.canonical_container_sha256 != container_hash) ||
      (NonZero(value.canonical_execution_envelope_sha256) &&
       value.canonical_execution_envelope_sha256 != execution_hash)) {
    return Fail(detail, "query_explain_bind.body_hash_invalid");
  }
  *output = Header("QEBQ", kQueryExplainBindRequestPrefixBytes,
                   kQueryExplainBindRequestPrefixBytes + suffix_size);
  Put(output, value.authenticated_receipt_uuid);
  PutU64(output, value.occurrence);
  PutU32(output, static_cast<std::uint32_t>(
                     value.canonical_container_bytes.size()));
  PutU32(output, static_cast<std::uint32_t>(
                     value.canonical_execution_envelope_bytes.size()));
  PutU32(output, value.verbose ? 1U : 0U);
  PutU32(output, value.format);
  Put(output, container_hash);
  Put(output, execution_hash);
  output->insert(output->end(), 32, 0);
  output->insert(output->end(), 32, 0);
  output->insert(output->end(), value.canonical_container_bytes.begin(),
                 value.canonical_container_bytes.end());
  output->insert(output->end(),
                 value.canonical_execution_envelope_bytes.begin(),
                 value.canonical_execution_envelope_bytes.end());
  const auto evidence = Evidence<120, 152>(*output);
  if (NonZero(value.request_evidence_sha256) &&
      value.request_evidence_sha256 != evidence) {
    output->clear();
    return Fail(detail, "query_explain_bind.request_evidence_invalid");
  }
  std::copy(evidence.begin(), evidence.end(), output->begin() + 120);
  return true;
}

bool DecodeQueryExplainBindRequestV1(
    const std::uint8_t* bytes, std::size_t size,
    QueryExplainBindRequestV1* output, std::string* detail) {
  if (output == nullptr ||
      !HeaderValid(bytes, size, "QEBQ", kQueryExplainBindRequestPrefixBytes,
                   false)) {
    return Fail(detail, "query_explain_bind.header_invalid");
  }
  QueryExplainBindRequestV1 value;
  Get(bytes + 16, &value.authenticated_receipt_uuid);
  value.occurrence = GetU64(bytes + 32);
  const auto container_size = GetU32(bytes + 40);
  const auto execution_size = GetU32(bytes + 44);
  const auto option_flags = GetU32(bytes + 48);
  const auto format = GetU32(bytes + 52);
  if ((option_flags & ~1U) != 0 || format > 255 ||
      (format != 1 && format != 2) || container_size == 0 ||
      execution_size == 0 ||
      static_cast<std::uint64_t>(container_size) + execution_size !=
          size - kQueryExplainBindRequestPrefixBytes ||
      !std::all_of(bytes + 152, bytes + 184,
                   [](std::uint8_t byte) { return byte == 0; })) {
    return Fail(detail, "query_explain_bind.extent_invalid");
  }
  value.verbose = (option_flags & 1U) != 0;
  value.format = static_cast<std::uint8_t>(format);
  Get(bytes + 56, &value.canonical_container_sha256);
  Get(bytes + 88, &value.canonical_execution_envelope_sha256);
  Get(bytes + 120, &value.request_evidence_sha256);
  std::size_t offset = kQueryExplainBindRequestPrefixBytes;
  value.canonical_container_bytes.assign(bytes + offset,
                                         bytes + offset + container_size);
  offset += container_size;
  value.canonical_execution_envelope_bytes.assign(
      bytes + offset, bytes + offset + execution_size);
  std::vector<std::uint8_t> canonical;
  if (!EncodeQueryExplainBindRequestV1(value, &canonical, detail) ||
      canonical.size() != size ||
      !std::equal(canonical.begin(), canonical.end(), bytes)) {
    return Fail(detail, "query_explain_bind.noncanonical");
  }
  *output = std::move(value);
  return true;
}

Hash256 QueryExplainBindRequestEvidenceV1(
    const QueryExplainBindRequestV1& value) {
  auto copy = value;
  copy.request_evidence_sha256 = {};
  std::vector<std::uint8_t> bytes;
  return EncodeQueryExplainBindRequestV1(copy, &bytes)
             ? Evidence<120, 152>(bytes)
             : Hash256{};
}

bool EncodeQueryExplainBindAckV1(const QueryExplainBindAckV1& value,
                                 std::vector<std::uint8_t>* output,
                                 std::string* detail) {
  if (output == nullptr) {
    return Fail(detail, "query_explain_bind_ack.output_missing");
  }
  output->clear();
  if (!NonZero(value.authenticated_receipt_uuid) || value.occurrence == 0 ||
      !NonZero(value.binding_uuid) || value.binding_generation == 0 ||
      !NonZero(value.explain_uuid) ||
      !NonZero(value.canonical_query_sblr_sha256) ||
      !NonZero(value.request_evidence_sha256)) {
    return Fail(detail, "query_explain_bind_ack.fields_invalid");
  }
  *output = Header("QEBA", kQueryExplainBindAckBytes,
                   kQueryExplainBindAckBytes);
  Put(output, value.authenticated_receipt_uuid);
  PutU64(output, value.occurrence);
  Put(output, value.binding_uuid);
  PutU64(output, value.binding_generation);
  Put(output, value.explain_uuid);
  Put(output, value.canonical_query_sblr_sha256);
  Put(output, value.request_evidence_sha256);
  output->insert(output->end(), 32, 0);
  const auto evidence = Evidence<144, 176>(*output);
  if (NonZero(value.acknowledgement_evidence_sha256) &&
      value.acknowledgement_evidence_sha256 != evidence) {
    output->clear();
    return Fail(detail, "query_explain_bind_ack.evidence_invalid");
  }
  std::copy(evidence.begin(), evidence.end(), output->begin() + 144);
  return true;
}

bool DecodeQueryExplainBindAckV1(const std::uint8_t* bytes,
                                 std::size_t size,
                                 QueryExplainBindAckV1* output,
                                 std::string* detail) {
  if (output == nullptr ||
      !HeaderValid(bytes, size, "QEBA", kQueryExplainBindAckBytes, true)) {
    return Fail(detail, "query_explain_bind_ack.header_invalid");
  }
  QueryExplainBindAckV1 value;
  Get(bytes + 16, &value.authenticated_receipt_uuid);
  value.occurrence = GetU64(bytes + 32);
  Get(bytes + 40, &value.binding_uuid);
  value.binding_generation = GetU64(bytes + 56);
  Get(bytes + 64, &value.explain_uuid);
  Get(bytes + 80, &value.canonical_query_sblr_sha256);
  Get(bytes + 112, &value.request_evidence_sha256);
  Get(bytes + 144, &value.acknowledgement_evidence_sha256);
  std::vector<std::uint8_t> canonical;
  if (!EncodeQueryExplainBindAckV1(value, &canonical, detail) ||
      canonical.size() != size ||
      !std::equal(canonical.begin(), canonical.end(), bytes)) {
    return Fail(detail, "query_explain_bind_ack.noncanonical");
  }
  *output = value;
  return true;
}

Hash256 QueryExplainBindAckEvidenceV1(const QueryExplainBindAckV1& value) {
  auto copy = value;
  copy.acknowledgement_evidence_sha256 = {};
  std::vector<std::uint8_t> bytes;
  return EncodeQueryExplainBindAckV1(copy, &bytes)
             ? Evidence<144, 176>(bytes)
             : Hash256{};
}

bool EncodeFreeBindRequestV1(const FreeBindRequestV1& value,
                             std::vector<std::uint8_t>* output,
                             std::string* detail) {
  if (output == nullptr) return Fail(detail, "stmt_free_bind.output_missing");
  output->clear();
  if (!NonZero(value.authenticated_receipt_uuid) || value.occurrence == 0 ||
      !ValidName(value.statement_name, value.quoted)) {
    return Fail(detail, "stmt_free_bind.fields_invalid");
  }
  *output = Header("SFBQ", kFreeRequestPrefixBytes,
                   kFreeRequestPrefixBytes + value.statement_name.size());
  Put(output, value.authenticated_receipt_uuid);
  PutU64(output, value.occurrence);
  PutU32(output, value.quoted ? 1U : 0U);
  PutU32(output, static_cast<std::uint32_t>(value.statement_name.size()));
  output->insert(output->end(), 32, 0);
  output->insert(output->end(), 16, 0);
  output->insert(output->end(), value.statement_name.begin(),
                 value.statement_name.end());
  const auto evidence = Evidence<48, 80>(*output);
  if (NonZero(value.request_evidence_sha256) &&
      value.request_evidence_sha256 != evidence) {
    output->clear();
    return Fail(detail, "stmt_free_bind.request_evidence_invalid");
  }
  std::copy(evidence.begin(), evidence.end(), output->begin() + 48);
  return true;
}

bool DecodeFreeBindRequestV1(const std::uint8_t* bytes, std::size_t size,
                             FreeBindRequestV1* output, std::string* detail) {
  if (output == nullptr ||
      !HeaderValid(bytes, size, "SFBQ", kFreeRequestPrefixBytes, false)) {
    return Fail(detail, "stmt_free_bind.header_invalid");
  }
  FreeBindRequestV1 value;
  Get(bytes + 16, &value.authenticated_receipt_uuid);
  value.occurrence = GetU64(bytes + 32);
  const auto flags = GetU32(bytes + 40);
  const auto name_bytes = GetU32(bytes + 44);
  Get(bytes + 48, &value.request_evidence_sha256);
  if ((flags & ~1U) != 0 || name_bytes == 0 ||
      name_bytes != size - kFreeRequestPrefixBytes ||
      !std::all_of(bytes + 80, bytes + 96,
                   [](std::uint8_t byte) { return byte == 0; })) {
    return Fail(detail, "stmt_free_bind.extent_invalid");
  }
  value.quoted = (flags & 1U) != 0;
  value.statement_name.assign(
      reinterpret_cast<const char*>(bytes + kFreeRequestPrefixBytes), name_bytes);
  std::vector<std::uint8_t> canonical;
  if (!EncodeFreeBindRequestV1(value, &canonical, detail) ||
      canonical.size() != size || !std::equal(canonical.begin(), canonical.end(), bytes)) {
    return Fail(detail, "stmt_free_bind.noncanonical");
  }
  *output = std::move(value);
  return true;
}

Hash256 FreeBindRequestEvidenceV1(const FreeBindRequestV1& value) {
  auto copy = value;
  copy.request_evidence_sha256 = {};
  std::vector<std::uint8_t> bytes;
  return EncodeFreeBindRequestV1(copy, &bytes) ? Evidence<48, 80>(bytes)
                                               : Hash256{};
}

bool EncodeFreeBindAckV1(const FreeBindAckV1& value,
                         std::vector<std::uint8_t>* output,
                         std::string* detail) {
  if (output == nullptr) return Fail(detail, "stmt_free_bind_ack.output_missing");
  output->clear();
  if (!NonZero(value.authenticated_receipt_uuid) || value.occurrence == 0 ||
      !NonZero(value.binding_uuid) || value.binding_generation == 0 ||
      !NonZero(value.statement_uuid) || !NonZero(value.statement_name_uuid) ||
      value.prepared_generation == 0 || !NonZero(value.descriptor_sha256) ||
      !NonZero(value.request_evidence_sha256)) {
    return Fail(detail, "stmt_free_bind_ack.fields_invalid");
  }
  *output = Header("SFBA", kFreeAckBytes, kFreeAckBytes);
  Put(output, value.authenticated_receipt_uuid);
  PutU64(output, value.occurrence);
  Put(output, value.binding_uuid);
  PutU64(output, value.binding_generation);
  Put(output, value.statement_uuid);
  Put(output, value.statement_name_uuid);
  PutU64(output, value.prepared_generation);
  Put(output, value.descriptor_sha256);
  Put(output, value.request_evidence_sha256);
  output->insert(output->end(), 32, 0);
  output->insert(output->end(), 8, 0);
  const auto evidence = Evidence<168, 200>(*output);
  if (NonZero(value.acknowledgement_evidence_sha256) &&
      value.acknowledgement_evidence_sha256 != evidence) {
    output->clear();
    return Fail(detail, "stmt_free_bind_ack.evidence_invalid");
  }
  std::copy(evidence.begin(), evidence.end(), output->begin() + 168);
  return true;
}

bool DecodeFreeBindAckV1(const std::uint8_t* bytes, std::size_t size,
                         FreeBindAckV1* output, std::string* detail) {
  if (output == nullptr ||
      !HeaderValid(bytes, size, "SFBA", kFreeAckBytes, true)) {
    return Fail(detail, "stmt_free_bind_ack.header_invalid");
  }
  FreeBindAckV1 value;
  Get(bytes + 16, &value.authenticated_receipt_uuid);
  value.occurrence = GetU64(bytes + 32);
  Get(bytes + 40, &value.binding_uuid);
  value.binding_generation = GetU64(bytes + 56);
  Get(bytes + 64, &value.statement_uuid);
  Get(bytes + 80, &value.statement_name_uuid);
  value.prepared_generation = GetU64(bytes + 96);
  Get(bytes + 104, &value.descriptor_sha256);
  Get(bytes + 136, &value.request_evidence_sha256);
  Get(bytes + 168, &value.acknowledgement_evidence_sha256);
  if (!std::all_of(bytes + 200, bytes + 208,
                   [](std::uint8_t byte) { return byte == 0; })) {
    return Fail(detail, "stmt_free_bind_ack.reserved_invalid");
  }
  std::vector<std::uint8_t> canonical;
  if (!EncodeFreeBindAckV1(value, &canonical, detail) ||
      !std::equal(canonical.begin(), canonical.end(), bytes)) {
    return Fail(detail, "stmt_free_bind_ack.noncanonical");
  }
  *output = value;
  return true;
}

Hash256 FreeBindAckEvidenceV1(const FreeBindAckV1& value) {
  auto copy = value;
  copy.acknowledgement_evidence_sha256 = {};
  std::vector<std::uint8_t> bytes;
  return EncodeFreeBindAckV1(copy, &bytes) ? Evidence<168, 200>(bytes)
                                          : Hash256{};
}

bool EncodeCancelBindRequestV1(const CancelBindRequestV1& value,
                               std::vector<std::uint8_t>* output,
                               std::string* detail) {
  if (output == nullptr) {
    return Fail(detail, "stmt_cancel_bind.output_missing");
  }
  output->clear();
  if (!NonZero(value.authenticated_receipt_uuid) || value.occurrence == 0 ||
      !ValidName(value.statement_name, value.quoted) || value.reason < 1 ||
      value.reason > 4 || value.mode < 1 || value.mode > 2) {
    return Fail(detail, "stmt_cancel_bind.fields_invalid");
  }
  *output = Header("SCBQ", kCancelRequestPrefixBytes,
                   kCancelRequestPrefixBytes + value.statement_name.size());
  Put(output, value.authenticated_receipt_uuid);
  PutU64(output, value.occurrence);
  PutU32(output, value.quoted ? 1U : 0U);
  PutU32(output, static_cast<std::uint32_t>(value.statement_name.size()));
  output->push_back(value.reason);
  output->push_back(value.mode);
  output->insert(output->end(), 6, 0);
  PutU64(output, value.deadline_monotonic_ns);
  output->insert(output->end(), 32, 0);
  output->insert(output->end(), 16, 0);
  output->insert(output->end(), value.statement_name.begin(),
                 value.statement_name.end());
  const auto evidence = Evidence<64, 96>(*output);
  if (NonZero(value.request_evidence_sha256) &&
      value.request_evidence_sha256 != evidence) {
    output->clear();
    return Fail(detail, "stmt_cancel_bind.request_evidence_invalid");
  }
  std::copy(evidence.begin(), evidence.end(), output->begin() + 64);
  return true;
}

bool DecodeCancelBindRequestV1(const std::uint8_t* bytes, std::size_t size,
                               CancelBindRequestV1* output,
                               std::string* detail) {
  if (output == nullptr ||
      !HeaderValid(bytes, size, "SCBQ", kCancelRequestPrefixBytes, false)) {
    return Fail(detail, "stmt_cancel_bind.header_invalid");
  }
  CancelBindRequestV1 value;
  Get(bytes + 16, &value.authenticated_receipt_uuid);
  value.occurrence = GetU64(bytes + 32);
  const auto flags = GetU32(bytes + 40);
  const auto name_bytes = GetU32(bytes + 44);
  value.reason = bytes[48];
  value.mode = bytes[49];
  value.deadline_monotonic_ns = GetU64(bytes + 56);
  Get(bytes + 64, &value.request_evidence_sha256);
  if ((flags & ~1U) != 0 || name_bytes == 0 ||
      name_bytes != size - kCancelRequestPrefixBytes ||
      !std::all_of(bytes + 50, bytes + 56,
                   [](std::uint8_t byte) { return byte == 0; }) ||
      !std::all_of(bytes + 96, bytes + 112,
                   [](std::uint8_t byte) { return byte == 0; })) {
    return Fail(detail, "stmt_cancel_bind.extent_invalid");
  }
  value.quoted = (flags & 1U) != 0;
  value.statement_name.assign(
      reinterpret_cast<const char*>(bytes + kCancelRequestPrefixBytes),
      name_bytes);
  std::vector<std::uint8_t> canonical;
  if (!EncodeCancelBindRequestV1(value, &canonical, detail) ||
      canonical.size() != size ||
      !std::equal(canonical.begin(), canonical.end(), bytes)) {
    return Fail(detail, "stmt_cancel_bind.noncanonical");
  }
  *output = std::move(value);
  return true;
}

Hash256 CancelBindRequestEvidenceV1(const CancelBindRequestV1& value) {
  auto copy = value;
  copy.request_evidence_sha256 = {};
  std::vector<std::uint8_t> bytes;
  return EncodeCancelBindRequestV1(copy, &bytes) ? Evidence<64, 96>(bytes)
                                                 : Hash256{};
}

bool EncodeCancelBindAckV1(const CancelBindAckV1& value,
                           std::vector<std::uint8_t>* output,
                           std::string* detail) {
  if (output == nullptr) {
    return Fail(detail, "stmt_cancel_bind_ack.output_missing");
  }
  output->clear();
  if (!NonZero(value.authenticated_receipt_uuid) || value.occurrence == 0 ||
      !NonZero(value.binding_uuid) || value.binding_generation == 0 ||
      !NonZero(value.target_execution_uuid) ||
      !NonZero(value.target_statement_uuid) ||
      !NonZero(value.target_statement_receipt_uuid) ||
      !NonZero(value.cancel_operation_uuid) ||
      value.target_execution_generation == 0 || value.reason < 1 ||
      value.reason > 4 || value.mode < 1 || value.mode > 2 ||
      value.executor_availability_generation == 0 ||
      !NonZero(value.descriptor_sha256) ||
      !NonZero(value.request_evidence_sha256)) {
    return Fail(detail, "stmt_cancel_bind_ack.fields_invalid");
  }
  *output = Header("SCBA", kCancelAckBytes, kCancelAckBytes);
  Put(output, value.authenticated_receipt_uuid);
  PutU64(output, value.occurrence);
  Put(output, value.binding_uuid);
  PutU64(output, value.binding_generation);
  Put(output, value.target_execution_uuid);
  Put(output, value.target_statement_uuid);
  Put(output, value.target_statement_receipt_uuid);
  Put(output, value.cancel_operation_uuid);
  Put(output, value.target_transaction_uuid);
  PutU64(output, value.target_execution_generation);
  output->push_back(value.reason);
  output->push_back(value.mode);
  output->insert(output->end(), 2, 0);
  PutU64(output, value.deadline_monotonic_ns);
  PutU64(output, value.executor_availability_generation);
  Put(output, value.descriptor_sha256);
  Put(output, value.request_evidence_sha256);
  output->insert(output->end(), 32, 0);
  output->insert(output->end(), 4, 0);
  const auto evidence = Evidence<236, 268>(*output);
  if (NonZero(value.acknowledgement_evidence_sha256) &&
      value.acknowledgement_evidence_sha256 != evidence) {
    output->clear();
    return Fail(detail, "stmt_cancel_bind_ack.evidence_invalid");
  }
  std::copy(evidence.begin(), evidence.end(), output->begin() + 236);
  return true;
}

bool DecodeCancelBindAckV1(const std::uint8_t* bytes, std::size_t size,
                           CancelBindAckV1* output, std::string* detail) {
  if (output == nullptr ||
      !HeaderValid(bytes, size, "SCBA", kCancelAckBytes, true)) {
    return Fail(detail, "stmt_cancel_bind_ack.header_invalid");
  }
  CancelBindAckV1 value;
  Get(bytes + 16, &value.authenticated_receipt_uuid);
  value.occurrence = GetU64(bytes + 32);
  Get(bytes + 40, &value.binding_uuid);
  value.binding_generation = GetU64(bytes + 56);
  Get(bytes + 64, &value.target_execution_uuid);
  Get(bytes + 80, &value.target_statement_uuid);
  Get(bytes + 96, &value.target_statement_receipt_uuid);
  Get(bytes + 112, &value.cancel_operation_uuid);
  Get(bytes + 128, &value.target_transaction_uuid);
  value.target_execution_generation = GetU64(bytes + 144);
  value.reason = bytes[152];
  value.mode = bytes[153];
  value.deadline_monotonic_ns = GetU64(bytes + 156);
  value.executor_availability_generation = GetU64(bytes + 164);
  Get(bytes + 172, &value.descriptor_sha256);
  Get(bytes + 204, &value.request_evidence_sha256);
  Get(bytes + 236, &value.acknowledgement_evidence_sha256);
  if (!std::all_of(bytes + 154, bytes + 156,
                   [](std::uint8_t byte) { return byte == 0; }) ||
      !std::all_of(bytes + 268, bytes + 272,
                   [](std::uint8_t byte) { return byte == 0; })) {
    return Fail(detail, "stmt_cancel_bind_ack.reserved_invalid");
  }
  std::vector<std::uint8_t> canonical;
  if (!EncodeCancelBindAckV1(value, &canonical, detail) ||
      !std::equal(canonical.begin(), canonical.end(), bytes)) {
    return Fail(detail, "stmt_cancel_bind_ack.noncanonical");
  }
  *output = value;
  return true;
}

Hash256 CancelBindAckEvidenceV1(const CancelBindAckV1& value) {
  auto copy = value;
  copy.acknowledgement_evidence_sha256 = {};
  std::vector<std::uint8_t> bytes;
  return EncodeCancelBindAckV1(copy, &bytes) ? Evidence<236, 268>(bytes)
                                             : Hash256{};
}

bool EncodeParameterBindRequestV1(const ParameterBindRequestV1& value,
                                  std::vector<std::uint8_t>* output,
                                  std::string* detail) {
  if (output == nullptr) {
    return Fail(detail, "parameter_bind_private.output_missing");
  }
  output->clear();
  const bool batch_pair =
      NonZero(value.batch_uuid) == (value.batch_generation != 0);
  const bool dynamic_pair =
      NonZero(value.dynamic_package_uuid) == (value.dynamic_generation != 0);
  if (!NonZero(value.authenticated_receipt_uuid) || value.occurrence == 0 ||
      !ValidName(value.statement_name, value.quoted) ||
      !NonZero(value.prepared_statement_uuid) ||
      value.prepared_generation == 0 || !NonZero(value.parameter_set_uuid) ||
      value.parameter_set_generation == 0 ||
      !NonZero(value.ordered_slot_table_sha256) || !batch_pair ||
      !dynamic_pair || value.value_count == 0 ||
      value.canonical_value_vector.empty() ||
      value.canonical_value_vector.size() > kMaximumParameterValueBytes) {
    return Fail(detail, "parameter_bind_private.fields_invalid");
  }
  const auto value_hash = Hash(value.canonical_value_vector);
  if (NonZero(value.value_vector_sha256) &&
      value.value_vector_sha256 != value_hash) {
    return Fail(detail, "parameter_bind_private.value_hash_invalid");
  }
  const std::uint64_t suffix_size =
      static_cast<std::uint64_t>(value.statement_name.size()) +
      value.canonical_value_vector.size();
  if (suffix_size > kMaximumCanonicalCarrierBytes ||
      suffix_size > std::numeric_limits<std::uint32_t>::max()) {
    return Fail(detail, "parameter_bind_private.extent_invalid");
  }
  *output = Header("SPKQ", kParameterBindRequestPrefixBytes,
                   kParameterBindRequestPrefixBytes + suffix_size);
  Put(output, value.authenticated_receipt_uuid);
  PutU64(output, value.occurrence);
  PutU32(output, value.quoted ? 1U : 0U);
  PutU32(output, static_cast<std::uint32_t>(value.statement_name.size()));
  Put(output, value.prepared_statement_uuid);
  PutU64(output, value.prepared_generation);
  Put(output, value.parameter_set_uuid);
  PutU64(output, value.parameter_set_generation);
  Put(output, value.ordered_slot_table_sha256);
  Put(output, value.batch_uuid);
  PutU64(output, value.batch_generation);
  Put(output, value.dynamic_package_uuid);
  PutU64(output, value.dynamic_generation);
  PutU32(output,
         static_cast<std::uint32_t>(value.canonical_value_vector.size()));
  PutU32(output, value.value_count);
  Put(output, value_hash);
  output->insert(output->end(), 32, 0);
  output->insert(output->end(), 8, 0);
  output->insert(output->end(), value.statement_name.begin(),
                 value.statement_name.end());
  output->insert(output->end(), value.canonical_value_vector.begin(),
                 value.canonical_value_vector.end());
  const auto evidence = Evidence<216, 248>(*output);
  if (NonZero(value.request_evidence_sha256) &&
      value.request_evidence_sha256 != evidence) {
    output->clear();
    return Fail(detail, "parameter_bind_private.request_evidence_invalid");
  }
  std::copy(evidence.begin(), evidence.end(), output->begin() + 216);
  return true;
}

bool DecodeParameterBindRequestV1(const std::uint8_t* bytes,
                                  std::size_t size,
                                  ParameterBindRequestV1* output,
                                  std::string* detail) {
  if (output == nullptr ||
      !HeaderValid(bytes, size, "SPKQ", kParameterBindRequestPrefixBytes,
                   false)) {
    return Fail(detail, "parameter_bind_private.header_invalid");
  }
  ParameterBindRequestV1 value;
  Get(bytes + 16, &value.authenticated_receipt_uuid);
  value.occurrence = GetU64(bytes + 32);
  const auto flags = GetU32(bytes + 40);
  const auto name_bytes = GetU32(bytes + 44);
  Get(bytes + 48, &value.prepared_statement_uuid);
  value.prepared_generation = GetU64(bytes + 64);
  Get(bytes + 72, &value.parameter_set_uuid);
  value.parameter_set_generation = GetU64(bytes + 88);
  Get(bytes + 96, &value.ordered_slot_table_sha256);
  Get(bytes + 128, &value.batch_uuid);
  value.batch_generation = GetU64(bytes + 144);
  Get(bytes + 152, &value.dynamic_package_uuid);
  value.dynamic_generation = GetU64(bytes + 168);
  const auto value_bytes = GetU32(bytes + 176);
  value.value_count = GetU32(bytes + 180);
  Get(bytes + 184, &value.value_vector_sha256);
  Get(bytes + 216, &value.request_evidence_sha256);
  if ((flags & ~1U) != 0 || name_bytes == 0 ||
      name_bytes > kMaximumStatementNameBytes || value_bytes == 0 ||
      value_bytes > kMaximumParameterValueBytes || value.value_count == 0 ||
      static_cast<std::uint64_t>(name_bytes) + value_bytes !=
          size - kParameterBindRequestPrefixBytes ||
      !std::all_of(bytes + 248, bytes + 256,
                   [](std::uint8_t byte) { return byte == 0; })) {
    return Fail(detail, "parameter_bind_private.extent_invalid");
  }
  value.quoted = (flags & 1U) != 0;
  std::size_t offset = kParameterBindRequestPrefixBytes;
  value.statement_name.assign(
      reinterpret_cast<const char*>(bytes + offset), name_bytes);
  offset += name_bytes;
  value.canonical_value_vector.assign(bytes + offset,
                                       bytes + offset + value_bytes);
  std::vector<std::uint8_t> canonical;
  if (!EncodeParameterBindRequestV1(value, &canonical, detail) ||
      canonical.size() != size ||
      !std::equal(canonical.begin(), canonical.end(), bytes)) {
    return Fail(detail, "parameter_bind_private.noncanonical");
  }
  *output = std::move(value);
  return true;
}

Hash256 ParameterBindRequestEvidenceV1(
    const ParameterBindRequestV1& value) {
  auto copy = value;
  copy.request_evidence_sha256 = {};
  std::vector<std::uint8_t> bytes;
  return EncodeParameterBindRequestV1(copy, &bytes)
             ? Evidence<216, 248>(bytes)
             : Hash256{};
}

namespace {

std::vector<std::uint8_t> EncodeNameAtomVector(
    const std::vector<NameResolveNameAtomV1>& atoms, bool allow_empty) {
  std::vector<std::uint8_t> bytes;
  if ((!allow_empty && atoms.empty()) || atoms.size() > 3) return bytes;
  for (const auto& atom : atoms) {
    if (atom.raw_utf8.empty() || atom.raw_utf8.size() > 256 ||
        !ValidName(atom.raw_utf8, atom.quoted)) {
      return {};
    }
    PutU16(&bytes, static_cast<std::uint16_t>(atom.raw_utf8.size()));
    bytes.insert(bytes.end(), atom.raw_utf8.begin(), atom.raw_utf8.end());
    bytes.push_back(atom.quoted ? 1 : 0);
  }
  return bytes;
}

bool DecodeNameAtomVector(const std::uint8_t* bytes, std::size_t size,
                          std::uint8_t count,
                          std::vector<NameResolveNameAtomV1>* output) {
  if (output == nullptr || count > 3 || (count == 0) != (size == 0))
    return false;
  output->clear();
  std::size_t offset = 0;
  for (std::uint8_t index = 0; index < count; ++index) {
    if (size - offset < 3) return false;
    const auto length = GetU16(bytes + offset);
    offset += 2;
    if (length == 0 || length > 256 || size - offset < length + 1)
      return false;
    NameResolveNameAtomV1 atom;
    atom.raw_utf8.assign(reinterpret_cast<const char*>(bytes + offset),
                         length);
    offset += length;
    const auto quoted = bytes[offset++];
    if (quoted > 1 || !ValidName(atom.raw_utf8, quoted != 0)) return false;
    atom.quoted = quoted != 0;
    output->push_back(std::move(atom));
  }
  return offset == size;
}

}  // namespace

bool EncodeNameResolveBindRequestV1(const NameResolveBindRequestV1& value,
                                    std::vector<std::uint8_t>* output,
                                    std::string* detail) {
  if (output == nullptr)
    return Fail(detail, "name_resolve_bind.output_missing");
  output->clear();
  const auto target = EncodeNameAtomVector(value.target_name_atoms, false);
  const auto name_space =
      EncodeNameAtomVector(value.namespace_name_atoms, true);
  if (!NonZero(value.authenticated_receipt_uuid) || value.occurrence == 0 ||
      target.empty() ||
      (!value.namespace_name_atoms.empty() && name_space.empty()) ||
      (value.resolution_mode != 1 && value.resolution_mode != 2) ||
      (value.resolution_mode == 1 && value.target_name_atoms.size() == 1 &&
       value.namespace_name_atoms.empty()) ||
      value.object_class > 16 ||
      target.size() + name_space.size() > 4096) {
    return Fail(detail, "name_resolve_bind.fields_invalid");
  }
  const auto target_hash = Hash(target);
  const auto namespace_hash = Hash(name_space);
  if ((NonZero(value.target_name_atoms_sha256) &&
       value.target_name_atoms_sha256 != target_hash) ||
      (NonZero(value.namespace_name_atoms_sha256) &&
       value.namespace_name_atoms_sha256 != namespace_hash)) {
    return Fail(detail, "name_resolve_bind.atom_hash_invalid");
  }
  *output = Header("SNBQ", kNameResolveBindRequestPrefixBytes,
                   kNameResolveBindRequestPrefixBytes + target.size() +
                       name_space.size());
  Put(output, value.authenticated_receipt_uuid);
  PutU64(output, value.occurrence);
  output->push_back(value.resolution_mode);
  output->push_back(value.object_class);
  output->push_back(static_cast<std::uint8_t>(value.target_name_atoms.size()));
  output->push_back(
      static_cast<std::uint8_t>(value.namespace_name_atoms.size()));
  PutU32(output, static_cast<std::uint32_t>(target.size()));
  PutU32(output, static_cast<std::uint32_t>(name_space.size()));
  output->insert(output->end(), 12, 0);
  Put(output, target_hash);
  Put(output, namespace_hash);
  output->insert(output->end(), 32, 0);
  output->insert(output->end(), target.begin(), target.end());
  output->insert(output->end(), name_space.begin(), name_space.end());
  const auto evidence = Evidence<128, 160>(*output);
  if (NonZero(value.request_evidence_sha256) &&
      value.request_evidence_sha256 != evidence) {
    output->clear();
    return Fail(detail, "name_resolve_bind.request_evidence_invalid");
  }
  std::copy(evidence.begin(), evidence.end(), output->begin() + 128);
  return true;
}

bool DecodeNameResolveBindRequestV1(const std::uint8_t* bytes,
                                    std::size_t size,
                                    NameResolveBindRequestV1* output,
                                    std::string* detail) {
  if (output == nullptr ||
      !HeaderValid(bytes, size, "SNBQ", kNameResolveBindRequestPrefixBytes,
                   false)) {
    return Fail(detail, "name_resolve_bind.header_invalid");
  }
  NameResolveBindRequestV1 value;
  Get(bytes + 16, &value.authenticated_receipt_uuid);
  value.occurrence = GetU64(bytes + 32);
  value.resolution_mode = bytes[40];
  value.object_class = bytes[41];
  const auto target_count = bytes[42];
  const auto namespace_count = bytes[43];
  const auto target_size = GetU32(bytes + 44);
  const auto namespace_size = GetU32(bytes + 48);
  Get(bytes + 64, &value.target_name_atoms_sha256);
  Get(bytes + 96, &value.namespace_name_atoms_sha256);
  Get(bytes + 128, &value.request_evidence_sha256);
  if (!std::all_of(bytes + 52, bytes + 64,
                   [](std::uint8_t byte) { return byte == 0; }) ||
      static_cast<std::uint64_t>(target_size) + namespace_size !=
          size - kNameResolveBindRequestPrefixBytes ||
      !DecodeNameAtomVector(bytes + kNameResolveBindRequestPrefixBytes,
                            target_size, target_count,
                            &value.target_name_atoms) ||
      !DecodeNameAtomVector(bytes + kNameResolveBindRequestPrefixBytes +
                                target_size,
                            namespace_size, namespace_count,
                            &value.namespace_name_atoms)) {
    return Fail(detail, "name_resolve_bind.extent_invalid");
  }
  std::vector<std::uint8_t> canonical;
  if (!EncodeNameResolveBindRequestV1(value, &canonical, detail) ||
      canonical != std::vector<std::uint8_t>(bytes, bytes + size)) {
    return Fail(detail, "name_resolve_bind.noncanonical");
  }
  *output = std::move(value);
  return true;
}

Hash256 NameResolveBindRequestEvidenceV1(
    const NameResolveBindRequestV1& value) {
  auto copy = value;
  copy.request_evidence_sha256 = {};
  std::vector<std::uint8_t> bytes;
  return EncodeNameResolveBindRequestV1(copy, &bytes)
             ? Evidence<128, 160>(bytes)
             : Hash256{};
}

bool EncodeNameResolveBindAckV1(const NameResolveBindAckV1& value,
                                std::vector<std::uint8_t>* output,
                                std::string* detail) {
  if (output == nullptr)
    return Fail(detail, "name_resolve_bind_ack.output_missing");
  output->clear();
  if (!NonZero(value.authenticated_receipt_uuid) || value.occurrence == 0 ||
      !NonZero(value.binding_uuid) || value.binding_generation == 0 ||
      !NonZero(value.resolution_uuid) || !NonZero(value.descriptor_sha256) ||
      !NonZero(value.request_evidence_sha256)) {
    return Fail(detail, "name_resolve_bind_ack.fields_invalid");
  }
  *output = Header("SNBA", kNameResolveBindAckBytes,
                   kNameResolveBindAckBytes);
  Put(output, value.authenticated_receipt_uuid);
  PutU64(output, value.occurrence);
  Put(output, value.binding_uuid);
  PutU64(output, value.binding_generation);
  Put(output, value.resolution_uuid);
  Put(output, value.descriptor_sha256);
  Put(output, value.request_evidence_sha256);
  output->insert(output->end(), 32, 0);
  const auto evidence = Evidence<144, 176>(*output);
  if (NonZero(value.acknowledgement_evidence_sha256) &&
      value.acknowledgement_evidence_sha256 != evidence) {
    output->clear();
    return Fail(detail, "name_resolve_bind_ack.evidence_invalid");
  }
  std::copy(evidence.begin(), evidence.end(), output->begin() + 144);
  return true;
}

bool DecodeNameResolveBindAckV1(const std::uint8_t* bytes, std::size_t size,
                                NameResolveBindAckV1* output,
                                std::string* detail) {
  if (output == nullptr ||
      !HeaderValid(bytes, size, "SNBA", kNameResolveBindAckBytes, true)) {
    return Fail(detail, "name_resolve_bind_ack.header_invalid");
  }
  NameResolveBindAckV1 value;
  Get(bytes + 16, &value.authenticated_receipt_uuid);
  value.occurrence = GetU64(bytes + 32);
  Get(bytes + 40, &value.binding_uuid);
  value.binding_generation = GetU64(bytes + 56);
  Get(bytes + 64, &value.resolution_uuid);
  Get(bytes + 80, &value.descriptor_sha256);
  Get(bytes + 112, &value.request_evidence_sha256);
  Get(bytes + 144, &value.acknowledgement_evidence_sha256);
  std::vector<std::uint8_t> canonical;
  if (!EncodeNameResolveBindAckV1(value, &canonical, detail) ||
      canonical != std::vector<std::uint8_t>(bytes, bytes + size)) {
    return Fail(detail, "name_resolve_bind_ack.noncanonical");
  }
  *output = value;
  return true;
}

Hash256 NameResolveBindAckEvidenceV1(const NameResolveBindAckV1& value) {
  auto copy = value;
  copy.acknowledgement_evidence_sha256 = {};
  std::vector<std::uint8_t> bytes;
  return EncodeNameResolveBindAckV1(copy, &bytes)
             ? Evidence<144, 176>(bytes)
             : Hash256{};
}

bool EncodeCatalogEpochCheckBindRequestV1(
    const CatalogEpochCheckBindRequestV1& value,
    std::vector<std::uint8_t>* output, std::string* detail) {
  if (output == nullptr)
    return Fail(detail, "catalog_epoch_check_bind.output_missing");
  output->clear();
  const auto target = EncodeNameAtomVector(value.target_name_atoms, true);
  if (!NonZero(value.authenticated_receipt_uuid) || value.occurrence == 0 ||
      value.object_scoped != !value.target_name_atoms.empty() ||
      value.target_name_atoms.size() > 3 ||
      (!value.target_name_atoms.empty() && target.empty()) ||
      target.size() > 4096) {
    return Fail(detail, "catalog_epoch_check_bind.fields_invalid");
  }
  const auto target_hash = Hash(target);
  if (NonZero(value.target_name_atoms_sha256) &&
      value.target_name_atoms_sha256 != target_hash) {
    return Fail(detail, "catalog_epoch_check_bind.atom_hash_invalid");
  }
  *output = Header("CEBQ", kCatalogEpochCheckBindRequestPrefixBytes,
                   kCatalogEpochCheckBindRequestPrefixBytes + target.size());
  Put(output, value.authenticated_receipt_uuid);
  PutU64(output, value.occurrence);
  PutU32(output, value.object_scoped ? 1U : 0U);
  output->push_back(
      static_cast<std::uint8_t>(value.target_name_atoms.size()));
  output->insert(output->end(), 3, 0);
  PutU32(output, static_cast<std::uint32_t>(target.size()));
  output->insert(output->end(), 12, 0);
  Put(output, target_hash);
  output->insert(output->end(), 32, 0);
  output->insert(output->end(), target.begin(), target.end());
  const auto evidence = Evidence<96, 128>(*output);
  if (NonZero(value.request_evidence_sha256) &&
      value.request_evidence_sha256 != evidence) {
    output->clear();
    return Fail(detail, "catalog_epoch_check_bind.request_evidence_invalid");
  }
  std::copy(evidence.begin(), evidence.end(), output->begin() + 96);
  return true;
}

bool DecodeCatalogEpochCheckBindRequestV1(
    const std::uint8_t* bytes, std::size_t size,
    CatalogEpochCheckBindRequestV1* output, std::string* detail) {
  if (output == nullptr ||
      !HeaderValid(bytes, size, "CEBQ",
                   kCatalogEpochCheckBindRequestPrefixBytes, false)) {
    return Fail(detail, "catalog_epoch_check_bind.header_invalid");
  }
  CatalogEpochCheckBindRequestV1 value;
  Get(bytes + 16, &value.authenticated_receipt_uuid);
  value.occurrence = GetU64(bytes + 32);
  const auto flags = GetU32(bytes + 40);
  const auto target_count = bytes[44];
  const auto target_size = GetU32(bytes + 48);
  Get(bytes + 64, &value.target_name_atoms_sha256);
  Get(bytes + 96, &value.request_evidence_sha256);
  value.object_scoped = (flags & 1U) != 0;
  if ((flags & ~1U) != 0 ||
      !std::all_of(bytes + 45, bytes + 48,
                   [](std::uint8_t byte) { return byte == 0; }) ||
      !std::all_of(bytes + 52, bytes + 64,
                   [](std::uint8_t byte) { return byte == 0; }) ||
      target_size != size - kCatalogEpochCheckBindRequestPrefixBytes ||
      !DecodeNameAtomVector(
          bytes + kCatalogEpochCheckBindRequestPrefixBytes, target_size,
          target_count, &value.target_name_atoms) ||
      value.object_scoped != !value.target_name_atoms.empty()) {
    return Fail(detail, "catalog_epoch_check_bind.extent_invalid");
  }
  std::vector<std::uint8_t> canonical;
  if (!EncodeCatalogEpochCheckBindRequestV1(value, &canonical, detail) ||
      canonical != std::vector<std::uint8_t>(bytes, bytes + size)) {
    return Fail(detail, "catalog_epoch_check_bind.noncanonical");
  }
  *output = std::move(value);
  return true;
}

Hash256 CatalogEpochCheckBindRequestEvidenceV1(
    const CatalogEpochCheckBindRequestV1& value) {
  auto copy = value;
  copy.request_evidence_sha256 = {};
  std::vector<std::uint8_t> bytes;
  return EncodeCatalogEpochCheckBindRequestV1(copy, &bytes)
             ? Evidence<96, 128>(bytes)
             : Hash256{};
}

bool EncodeCatalogEpochCheckBindAckV1(
    const CatalogEpochCheckBindAckV1& value,
    std::vector<std::uint8_t>* output, std::string* detail) {
  if (output == nullptr)
    return Fail(detail, "catalog_epoch_check_bind_ack.output_missing");
  output->clear();
  const bool object_identity_present =
      NonZero(value.object_uuid) && value.object_generation != 0;
  if (!NonZero(value.authenticated_receipt_uuid) || value.occurrence == 0 ||
      !NonZero(value.binding_uuid) || value.binding_generation == 0 ||
      !NonZero(value.check_uuid) ||
      (NonZero(value.object_uuid) != (value.object_generation != 0)) ||
      !NonZero(value.schema_tree_uuid) ||
      value.schema_tree_generation == 0 ||
      !NonZero(value.visibility_scope_sha256) ||
      !NonZero(value.descriptor_sha256) ||
      !NonZero(value.request_evidence_sha256)) {
    return Fail(detail, "catalog_epoch_check_bind_ack.fields_invalid");
  }
  *output = Header("CEBA", kCatalogEpochCheckBindAckBytes,
                   kCatalogEpochCheckBindAckBytes);
  Put(output, value.authenticated_receipt_uuid);
  PutU64(output, value.occurrence);
  Put(output, value.binding_uuid);
  PutU64(output, value.binding_generation);
  Put(output, value.check_uuid);
  if (object_identity_present) {
    Put(output, value.object_uuid);
  } else {
    output->insert(output->end(), 16, 0);
  }
  PutU64(output, value.object_generation);
  Put(output, value.schema_tree_uuid);
  PutU64(output, value.schema_tree_generation);
  Put(output, value.visibility_scope_sha256);
  Put(output, value.descriptor_sha256);
  Put(output, value.request_evidence_sha256);
  output->insert(output->end(), 32, 0);
  const auto evidence = Evidence<224, 256>(*output);
  if (NonZero(value.acknowledgement_evidence_sha256) &&
      value.acknowledgement_evidence_sha256 != evidence) {
    output->clear();
    return Fail(detail, "catalog_epoch_check_bind_ack.evidence_invalid");
  }
  std::copy(evidence.begin(), evidence.end(), output->begin() + 224);
  return true;
}

bool DecodeCatalogEpochCheckBindAckV1(
    const std::uint8_t* bytes, std::size_t size,
    CatalogEpochCheckBindAckV1* output, std::string* detail) {
  if (output == nullptr ||
      !HeaderValid(bytes, size, "CEBA", kCatalogEpochCheckBindAckBytes,
                   true)) {
    return Fail(detail, "catalog_epoch_check_bind_ack.header_invalid");
  }
  CatalogEpochCheckBindAckV1 value;
  Get(bytes + 16, &value.authenticated_receipt_uuid);
  value.occurrence = GetU64(bytes + 32);
  Get(bytes + 40, &value.binding_uuid);
  value.binding_generation = GetU64(bytes + 56);
  Get(bytes + 64, &value.check_uuid);
  Get(bytes + 80, &value.object_uuid);
  value.object_generation = GetU64(bytes + 96);
  Get(bytes + 104, &value.schema_tree_uuid);
  value.schema_tree_generation = GetU64(bytes + 120);
  Get(bytes + 128, &value.visibility_scope_sha256);
  Get(bytes + 160, &value.descriptor_sha256);
  Get(bytes + 192, &value.request_evidence_sha256);
  Get(bytes + 224, &value.acknowledgement_evidence_sha256);
  std::vector<std::uint8_t> canonical;
  if (!EncodeCatalogEpochCheckBindAckV1(value, &canonical, detail) ||
      canonical != std::vector<std::uint8_t>(bytes, bytes + size)) {
    return Fail(detail, "catalog_epoch_check_bind_ack.noncanonical");
  }
  *output = value;
  return true;
}

Hash256 CatalogEpochCheckBindAckEvidenceV1(
    const CatalogEpochCheckBindAckV1& value) {
  auto copy = value;
  copy.acknowledgement_evidence_sha256 = {};
  std::vector<std::uint8_t> bytes;
  return EncodeCatalogEpochCheckBindAckV1(copy, &bytes)
             ? Evidence<224, 256>(bytes)
             : Hash256{};
}

bool EncodeDatabaseAttachBindRequestV1(
    const DatabaseAttachBindRequestV1& value,
    std::vector<std::uint8_t>* output, std::string* detail) {
  if (output == nullptr)
    return Fail(detail, "database_attach_bind.output_missing");
  output->clear();
  if (!NonZero(value.authenticated_receipt_uuid) || value.occurrence == 0 ||
      (value.mode != 1 && value.mode != 2) || value.alias_scope != 1 ||
      !ValidName(value.storage_reference.raw_utf8,
                 value.storage_reference.quoted) ||
      !ValidName(value.database_alias.raw_utf8, value.database_alias.quoted) ||
      value.storage_reference.raw_utf8.size() >
          std::numeric_limits<std::uint16_t>::max() ||
      value.database_alias.raw_utf8.size() >
          std::numeric_limits<std::uint16_t>::max()) {
    return Fail(detail, "database_attach_bind.fields_invalid");
  }
  const auto suffix_size = value.storage_reference.raw_utf8.size() +
                           value.database_alias.raw_utf8.size();
  *output = Header("DABQ", kDatabaseAttachBindRequestPrefixBytes,
                   kDatabaseAttachBindRequestPrefixBytes + suffix_size);
  Put(output, value.authenticated_receipt_uuid);
  PutU64(output, value.occurrence);
  output->push_back(value.mode);
  output->push_back(value.alias_scope);
  output->push_back(value.storage_reference.quoted ? 1U : 0U);
  output->push_back(value.database_alias.quoted ? 1U : 0U);
  PutU16(output, static_cast<std::uint16_t>(
                     value.storage_reference.raw_utf8.size()));
  PutU16(output,
         static_cast<std::uint16_t>(value.database_alias.raw_utf8.size()));
  output->insert(output->end(), 32, 0);
  output->insert(output->end(), 16, 0);
  output->insert(output->end(), value.storage_reference.raw_utf8.begin(),
                 value.storage_reference.raw_utf8.end());
  output->insert(output->end(), value.database_alias.raw_utf8.begin(),
                 value.database_alias.raw_utf8.end());
  const auto evidence = Evidence<48, 80>(*output);
  if (NonZero(value.request_evidence_sha256) &&
      value.request_evidence_sha256 != evidence) {
    output->clear();
    return Fail(detail, "database_attach_bind.request_evidence_invalid");
  }
  std::copy(evidence.begin(), evidence.end(), output->begin() + 48);
  return true;
}

bool DecodeDatabaseAttachBindRequestV1(
    const std::uint8_t* bytes, std::size_t size,
    DatabaseAttachBindRequestV1* output, std::string* detail) {
  if (output == nullptr ||
      !HeaderValid(bytes, size, "DABQ",
                   kDatabaseAttachBindRequestPrefixBytes, false)) {
    return Fail(detail, "database_attach_bind.header_invalid");
  }
  DatabaseAttachBindRequestV1 value;
  Get(bytes + 16, &value.authenticated_receipt_uuid);
  value.occurrence = GetU64(bytes + 32);
  value.mode = bytes[40];
  value.alias_scope = bytes[41];
  if (bytes[42] > 1 || bytes[43] > 1 ||
      !std::all_of(bytes + 80, bytes + 96,
                   [](std::uint8_t byte) { return byte == 0; })) {
    return Fail(detail, "database_attach_bind.flags_invalid");
  }
  value.storage_reference.quoted = bytes[42] != 0;
  value.database_alias.quoted = bytes[43] != 0;
  const auto storage_size = GetU16(bytes + 44);
  const auto alias_size = GetU16(bytes + 46);
  Get(bytes + 48, &value.request_evidence_sha256);
  if (storage_size == 0 || alias_size == 0 ||
      static_cast<std::uint64_t>(storage_size) + alias_size !=
          size - kDatabaseAttachBindRequestPrefixBytes) {
    return Fail(detail, "database_attach_bind.extent_invalid");
  }
  const auto* suffix = bytes + kDatabaseAttachBindRequestPrefixBytes;
  value.storage_reference.raw_utf8.assign(
      reinterpret_cast<const char*>(suffix), storage_size);
  value.database_alias.raw_utf8.assign(
      reinterpret_cast<const char*>(suffix + storage_size), alias_size);
  std::vector<std::uint8_t> canonical;
  if (!EncodeDatabaseAttachBindRequestV1(value, &canonical, detail) ||
      canonical != std::vector<std::uint8_t>(bytes, bytes + size)) {
    return Fail(detail, "database_attach_bind.noncanonical");
  }
  *output = std::move(value);
  return true;
}

Hash256 DatabaseAttachBindRequestEvidenceV1(
    const DatabaseAttachBindRequestV1& value) {
  auto copy = value;
  copy.request_evidence_sha256 = {};
  std::vector<std::uint8_t> bytes;
  return EncodeDatabaseAttachBindRequestV1(copy, &bytes)
             ? Evidence<48, 80>(bytes)
             : Hash256{};
}

bool EncodeDatabaseAttachBindAckV1(
    const DatabaseAttachBindAckV1& value,
    std::vector<std::uint8_t>* output, std::string* detail) {
  if (output == nullptr)
    return Fail(detail, "database_attach_bind_ack.output_missing");
  output->clear();
  if (!NonZero(value.authenticated_receipt_uuid) || value.occurrence == 0 ||
      !NonZero(value.binding_uuid) || value.binding_generation == 0 ||
      !NonZero(value.attach_uuid) || !NonZero(value.storage_uuid) ||
      !NonZero(value.alias_uuid) || !NonZero(value.database_uuid) ||
      !NonZero(value.catalog_snapshot_uuid) ||
      value.catalog_generation == 0 || !NonZero(value.descriptor_sha256) ||
      !NonZero(value.request_evidence_sha256)) {
    return Fail(detail, "database_attach_bind_ack.fields_invalid");
  }
  *output = Header("DABA", kDatabaseAttachBindAckBytes,
                   kDatabaseAttachBindAckBytes);
  Put(output, value.authenticated_receipt_uuid);
  PutU64(output, value.occurrence);
  Put(output, value.binding_uuid);
  PutU64(output, value.binding_generation);
  Put(output, value.attach_uuid);
  Put(output, value.storage_uuid);
  Put(output, value.alias_uuid);
  Put(output, value.database_uuid);
  Put(output, value.catalog_snapshot_uuid);
  PutU64(output, value.catalog_generation);
  Put(output, value.descriptor_sha256);
  Put(output, value.request_evidence_sha256);
  output->insert(output->end(), 32, 0);
  output->insert(output->end(), 8, 0);
  const auto evidence = Evidence<216, 248>(*output);
  if (NonZero(value.acknowledgement_evidence_sha256) &&
      value.acknowledgement_evidence_sha256 != evidence) {
    output->clear();
    return Fail(detail, "database_attach_bind_ack.evidence_invalid");
  }
  std::copy(evidence.begin(), evidence.end(), output->begin() + 216);
  return true;
}

bool DecodeDatabaseAttachBindAckV1(
    const std::uint8_t* bytes, std::size_t size,
    DatabaseAttachBindAckV1* output, std::string* detail) {
  if (output == nullptr ||
      !HeaderValid(bytes, size, "DABA", kDatabaseAttachBindAckBytes, true)) {
    return Fail(detail, "database_attach_bind_ack.header_invalid");
  }
  if (!std::all_of(bytes + 248, bytes + 256,
                   [](std::uint8_t byte) { return byte == 0; })) {
    return Fail(detail, "database_attach_bind_ack.reserved_invalid");
  }
  DatabaseAttachBindAckV1 value;
  Get(bytes + 16, &value.authenticated_receipt_uuid);
  value.occurrence = GetU64(bytes + 32);
  Get(bytes + 40, &value.binding_uuid);
  value.binding_generation = GetU64(bytes + 56);
  Get(bytes + 64, &value.attach_uuid);
  Get(bytes + 80, &value.storage_uuid);
  Get(bytes + 96, &value.alias_uuid);
  Get(bytes + 112, &value.database_uuid);
  Get(bytes + 128, &value.catalog_snapshot_uuid);
  value.catalog_generation = GetU64(bytes + 144);
  Get(bytes + 152, &value.descriptor_sha256);
  Get(bytes + 184, &value.request_evidence_sha256);
  Get(bytes + 216, &value.acknowledgement_evidence_sha256);
  std::vector<std::uint8_t> canonical;
  if (!EncodeDatabaseAttachBindAckV1(value, &canonical, detail) ||
      canonical != std::vector<std::uint8_t>(bytes, bytes + size)) {
    return Fail(detail, "database_attach_bind_ack.noncanonical");
  }
  *output = value;
  return true;
}

Hash256 DatabaseAttachBindAckEvidenceV1(
    const DatabaseAttachBindAckV1& value) {
  auto copy = value;
  copy.acknowledgement_evidence_sha256 = {};
  std::vector<std::uint8_t> bytes;
  return EncodeDatabaseAttachBindAckV1(copy, &bytes)
             ? Evidence<216, 248>(bytes)
             : Hash256{};
}

bool EncodeParseTextBindRequestV1(const ParseTextBindRequestV1& value,
                                  std::vector<std::uint8_t>* output,
                                  std::string* detail) {
  if (output == nullptr)
    return Fail(detail, "parse_text_bind.output_missing");
  output->clear();
  const auto input_view = std::string_view(value.canonical_input_utf8);
  const bool has_bom = input_view.size() >= 3 &&
      static_cast<std::uint8_t>(input_view[0]) == 0xef &&
      static_cast<std::uint8_t>(input_view[1]) == 0xbb &&
      static_cast<std::uint8_t>(input_view[2]) == 0xbf;
  const std::uint64_t suffix_size =
      static_cast<std::uint64_t>(value.language_profile_id.size()) +
      value.canonical_input_utf8.size() +
      value.canonical_container_bytes.size() +
      value.canonical_execution_envelope_bytes.size();
  if (!NonZero(value.authenticated_receipt_uuid) || value.occurrence == 0 ||
      !IsValidUtf8Extent(value.language_profile_id,
                         kMaximumStatementNameBytes) ||
      !IsValidUtf8Extent(input_view, kMaximumParseTextInputBytes) || has_bom ||
      value.requested_maximum_bytes < value.canonical_input_utf8.size() ||
      value.requested_maximum_bytes > kMaximumParseTextInputBytes ||
      value.requested_maximum_depth == 0 ||
      value.requested_maximum_depth > 1024 ||
      value.canonical_container_bytes.empty() ||
      value.canonical_execution_envelope_bytes.empty() ||
      value.canonical_container_bytes.size() > kMaximumCanonicalCarrierBytes ||
      value.canonical_execution_envelope_bytes.size() >
          kMaximumCanonicalCarrierBytes ||
      suffix_size > kMaximumCanonicalCarrierBytes ||
      suffix_size > std::numeric_limits<std::uint32_t>::max() ||
      value.language_profile_id.size() >
          std::numeric_limits<std::uint16_t>::max() ||
      value.canonical_input_utf8.size() >
          std::numeric_limits<std::uint32_t>::max() ||
      value.canonical_container_bytes.size() >
          std::numeric_limits<std::uint32_t>::max() ||
      value.canonical_execution_envelope_bytes.size() >
          std::numeric_limits<std::uint32_t>::max()) {
    return Fail(detail, "parse_text_bind.fields_invalid");
  }
  const auto input_hash = Hash(
      reinterpret_cast<const std::uint8_t*>(input_view.data()),
      input_view.size());
  const auto container_hash = Hash(value.canonical_container_bytes);
  const auto execution_hash =
      Hash(value.canonical_execution_envelope_bytes);
  if ((NonZero(value.canonical_input_sha256) &&
       value.canonical_input_sha256 != input_hash) ||
      (NonZero(value.canonical_container_sha256) &&
       value.canonical_container_sha256 != container_hash) ||
      (NonZero(value.canonical_execution_envelope_sha256) &&
       value.canonical_execution_envelope_sha256 != execution_hash)) {
    return Fail(detail, "parse_text_bind.body_hash_invalid");
  }
  *output = Header("PTBQ", kParseTextBindRequestPrefixBytes,
                   kParseTextBindRequestPrefixBytes + suffix_size);
  Put(output, value.authenticated_receipt_uuid);
  PutU64(output, value.occurrence);
  PutU32(output, value.requested_maximum_bytes);
  PutU16(output, value.requested_maximum_depth);
  output->push_back(value.allow_donor_extensions ? 1U : 0U);
  output->push_back(0);
  PutU16(output,
         static_cast<std::uint16_t>(value.language_profile_id.size()));
  PutU32(output,
         static_cast<std::uint32_t>(value.canonical_input_utf8.size()));
  PutU32(output,
         static_cast<std::uint32_t>(value.canonical_container_bytes.size()));
  PutU32(output, static_cast<std::uint32_t>(
                     value.canonical_execution_envelope_bytes.size()));
  PutU16(output, 0);
  Put(output, input_hash);
  Put(output, container_hash);
  Put(output, execution_hash);
  output->insert(output->end(), 32, 0);
  output->insert(output->end(), 32, 0);
  output->insert(output->end(), value.language_profile_id.begin(),
                 value.language_profile_id.end());
  output->insert(output->end(), value.canonical_input_utf8.begin(),
                 value.canonical_input_utf8.end());
  output->insert(output->end(), value.canonical_container_bytes.begin(),
                 value.canonical_container_bytes.end());
  output->insert(output->end(),
                 value.canonical_execution_envelope_bytes.begin(),
                 value.canonical_execution_envelope_bytes.end());
  const auto evidence = Evidence<160, 192>(*output);
  if (NonZero(value.request_evidence_sha256) &&
      value.request_evidence_sha256 != evidence) {
    output->clear();
    return Fail(detail, "parse_text_bind.request_evidence_invalid");
  }
  std::copy(evidence.begin(), evidence.end(), output->begin() + 160);
  return true;
}

bool DecodeParseTextBindRequestV1(const std::uint8_t* bytes,
                                  std::size_t size,
                                  ParseTextBindRequestV1* output,
                                  std::string* detail) {
  if (output == nullptr ||
      !HeaderValid(bytes, size, "PTBQ", kParseTextBindRequestPrefixBytes,
                   false)) {
    return Fail(detail, "parse_text_bind.header_invalid");
  }
  ParseTextBindRequestV1 value;
  Get(bytes + 16, &value.authenticated_receipt_uuid);
  value.occurrence = GetU64(bytes + 32);
  value.requested_maximum_bytes = GetU32(bytes + 40);
  value.requested_maximum_depth = GetU16(bytes + 44);
  const auto extension = bytes[46];
  const auto profile_size = GetU16(bytes + 48);
  const auto input_size = GetU32(bytes + 50);
  const auto container_size = GetU32(bytes + 54);
  const auto execution_size = GetU32(bytes + 58);
  Get(bytes + 64, &value.canonical_input_sha256);
  Get(bytes + 96, &value.canonical_container_sha256);
  Get(bytes + 128, &value.canonical_execution_envelope_sha256);
  Get(bytes + 160, &value.request_evidence_sha256);
  const std::uint64_t suffix_size =
      static_cast<std::uint64_t>(profile_size) + input_size +
      container_size + execution_size;
  if (extension > 1 || bytes[47] != 0 || GetU16(bytes + 62) != 0 ||
      !std::all_of(bytes + 192, bytes + 224,
                   [](std::uint8_t byte) { return byte == 0; }) ||
      profile_size == 0 || input_size == 0 || container_size == 0 ||
      execution_size == 0 ||
      suffix_size != size - kParseTextBindRequestPrefixBytes) {
    return Fail(detail, "parse_text_bind.extent_invalid");
  }
  value.allow_donor_extensions = extension != 0;
  std::size_t offset = kParseTextBindRequestPrefixBytes;
  value.language_profile_id.assign(
      reinterpret_cast<const char*>(bytes + offset), profile_size);
  offset += profile_size;
  value.canonical_input_utf8.assign(
      reinterpret_cast<const char*>(bytes + offset), input_size);
  offset += input_size;
  value.canonical_container_bytes.assign(bytes + offset,
                                         bytes + offset + container_size);
  offset += container_size;
  value.canonical_execution_envelope_bytes.assign(
      bytes + offset, bytes + offset + execution_size);
  std::vector<std::uint8_t> canonical;
  if (!EncodeParseTextBindRequestV1(value, &canonical, detail) ||
      canonical.size() != size ||
      !std::equal(canonical.begin(), canonical.end(), bytes)) {
    return Fail(detail, "parse_text_bind.noncanonical");
  }
  *output = std::move(value);
  return true;
}

Hash256 ParseTextBindRequestEvidenceV1(
    const ParseTextBindRequestV1& value) {
  auto copy = value;
  copy.request_evidence_sha256 = {};
  std::vector<std::uint8_t> bytes;
  return EncodeParseTextBindRequestV1(copy, &bytes)
             ? Evidence<160, 192>(bytes)
             : Hash256{};
}

bool EncodeParseTextBindAckV1(const ParseTextBindAckV1& value,
                              std::vector<std::uint8_t>* output,
                              std::string* detail) {
  if (output == nullptr)
    return Fail(detail, "parse_text_bind_ack.output_missing");
  output->clear();
  if (!NonZero(value.authenticated_receipt_uuid) || value.occurrence == 0 ||
      !NonZero(value.binding_uuid) || value.binding_generation == 0 ||
      !NonZero(value.parse_uuid) || !NonZero(value.descriptor_sha256) ||
      !NonZero(value.canonical_input_sha256) ||
      !NonZero(value.request_evidence_sha256)) {
    return Fail(detail, "parse_text_bind_ack.fields_invalid");
  }
  *output = Header("PTBA", kParseTextBindAckBytes,
                   kParseTextBindAckBytes);
  Put(output, value.authenticated_receipt_uuid);
  PutU64(output, value.occurrence);
  Put(output, value.binding_uuid);
  PutU64(output, value.binding_generation);
  Put(output, value.parse_uuid);
  Put(output, value.descriptor_sha256);
  Put(output, value.canonical_input_sha256);
  Put(output, value.request_evidence_sha256);
  output->insert(output->end(), 32, 0);
  const auto evidence = Evidence<176, 208>(*output);
  if (NonZero(value.acknowledgement_evidence_sha256) &&
      value.acknowledgement_evidence_sha256 != evidence) {
    output->clear();
    return Fail(detail, "parse_text_bind_ack.evidence_invalid");
  }
  std::copy(evidence.begin(), evidence.end(), output->begin() + 176);
  return true;
}

bool DecodeParseTextBindAckV1(const std::uint8_t* bytes, std::size_t size,
                              ParseTextBindAckV1* output,
                              std::string* detail) {
  if (output == nullptr ||
      !HeaderValid(bytes, size, "PTBA", kParseTextBindAckBytes, true)) {
    return Fail(detail, "parse_text_bind_ack.header_invalid");
  }
  ParseTextBindAckV1 value;
  Get(bytes + 16, &value.authenticated_receipt_uuid);
  value.occurrence = GetU64(bytes + 32);
  Get(bytes + 40, &value.binding_uuid);
  value.binding_generation = GetU64(bytes + 56);
  Get(bytes + 64, &value.parse_uuid);
  Get(bytes + 80, &value.descriptor_sha256);
  Get(bytes + 112, &value.canonical_input_sha256);
  Get(bytes + 144, &value.request_evidence_sha256);
  Get(bytes + 176, &value.acknowledgement_evidence_sha256);
  std::vector<std::uint8_t> canonical;
  if (!EncodeParseTextBindAckV1(value, &canonical, detail) ||
      canonical.size() != size ||
      !std::equal(canonical.begin(), canonical.end(), bytes)) {
    return Fail(detail, "parse_text_bind_ack.noncanonical");
  }
  *output = value;
  return true;
}

Hash256 ParseTextBindAckEvidenceV1(const ParseTextBindAckV1& value) {
  auto copy = value;
  copy.acknowledgement_evidence_sha256 = {};
  std::vector<std::uint8_t> bytes;
  return EncodeParseTextBindAckV1(copy, &bytes)
             ? Evidence<176, 208>(bytes)
             : Hash256{};
}

}  // namespace scratchbird::wire::sbps_statement_management
