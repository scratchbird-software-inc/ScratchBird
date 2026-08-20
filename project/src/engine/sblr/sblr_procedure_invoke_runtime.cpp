#include "sblr_procedure_invoke_runtime.hpp"

#include "core/hash/hash_digest.hpp"

#include <algorithm>
#include <cstring>

namespace scratchbird::engine::sblr {
namespace {
void Put(std::vector<std::uint8_t>* out, std::uint64_t value, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) out->push_back(value >> (8 * i));
}
std::uint64_t Get(const std::uint8_t* bytes, std::size_t n) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < n; ++i) value |= std::uint64_t(bytes[i]) << (8 * i);
  return value;
}
template <class T> bool Nonzero(const T& value) {
  return std::any_of(value.begin(), value.end(), [](auto byte) { return byte != 0; });
}
std::vector<std::uint8_t> Header(const char* magic, std::size_t bytes) {
  std::vector<std::uint8_t> out(magic, magic + 4);
  Put(&out, 1, 2); Put(&out, bytes, 2); Put(&out, bytes, 4); Put(&out, 0, 4);
  return out;
}
bool ValidHeader(const std::uint8_t* bytes, std::size_t size,
                 const char* magic, std::size_t expected) {
  return bytes && size == expected && std::equal(bytes, bytes + 4, magic) &&
         Get(bytes + 4, 2) == 1 && Get(bytes + 6, 2) == expected &&
         Get(bytes + 8, 4) == expected &&
         std::all_of(bytes + 12, bytes + 16, [](auto byte) { return byte == 0; });
}
ProcedureInvokeSha Evidence(const char* domain, const std::uint8_t* bytes,
                            std::size_t size) {
  std::vector<std::uint8_t> material(domain, domain + std::strlen(domain));
  material.insert(material.end(), bytes, bytes + size);
  return scratchbird::core::hash::ComputeSha256Digest(material).digest;
}
}  // namespace

std::vector<std::uint8_t> EncodeSblrProcedureInvokeRequestV1(
    const SblrProcedureInvokeRequestV1& value) {
  if (!Nonzero(value.receipt) || value.occurrence == 0 ||
      value.invocation_occurrence == 0) return {};
  auto out = Header("PIRQ", 64);
  out.insert(out.end(), value.receipt.begin(), value.receipt.end());
  Put(&out, value.occurrence, 8); Put(&out, value.invocation_occurrence, 4);
  out.insert(out.end(), 20, 0);
  return out;
}
bool DecodeSblrProcedureInvokeRequestV1(const std::uint8_t* bytes,
                                        std::size_t size,
                                        SblrProcedureInvokeRequestV1* out,
                                        std::string* detail) {
  if (!out || !ValidHeader(bytes, size, "PIRQ", 64) ||
      std::any_of(bytes + 44, bytes + 64, [](auto byte) { return byte != 0; })) {
    if (detail) *detail = "PIRQ invalid"; return false;
  }
  SblrProcedureInvokeRequestV1 value;
  std::copy_n(bytes + 16, 16, value.receipt.begin());
  value.occurrence = Get(bytes + 32, 8);
  value.invocation_occurrence = Get(bytes + 40, 4);
  if (EncodeSblrProcedureInvokeRequestV1(value).empty()) return false;
  *out = value; return true;
}
std::vector<std::uint8_t> EncodeSblrProcedureInvokeDescriptorV1(
    const SblrProcedureInvokeDescriptorV1& value, bool operand) {
  if (!Nonzero(value.body) || value.availability == 0) return {};
  auto out = Header(operand ? "PIDO" : "PIDD", 488);
  out.insert(out.end(), value.body.begin(), value.body.end());
  const auto evidence = Evidence("ScratchBird.SblrProcedureInvokeDescriptor.V1",
                                 out.data() + 16, 416);
  if (Nonzero(value.evidence) && value.evidence != evidence) return {};
  out.insert(out.end(), evidence.begin(), evidence.end());
  Put(&out, value.availability, 8); out.insert(out.end(), 16, 0);
  return out;
}
bool DecodeSblrProcedureInvokeDescriptorV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrProcedureInvokeDescriptorV1* out, std::string* detail, bool operand) {
  if (!out || !ValidHeader(bytes, size, operand ? "PIDO" : "PIDD", 488) ||
      std::any_of(bytes + 472, bytes + 488, [](auto byte) { return byte != 0; })) {
    if (detail) *detail = "PID invalid"; return false;
  }
  SblrProcedureInvokeDescriptorV1 value;
  std::copy_n(bytes + 16, 416, value.body.begin());
  std::copy_n(bytes + 432, 32, value.evidence.begin());
  value.availability = Get(bytes + 464, 8);
  if (EncodeSblrProcedureInvokeDescriptorV1(value, operand).empty()) return false;
  *out = value; return true;
}
std::vector<std::uint8_t> EncodeSblrProcedureInvokeResultV1(
    const SblrProcedureInvokeResultV1& value) {
  if (!Nonzero(value.body) || value.body[24] < 1 || value.body[24] > 2 ||
      value.body[25] > 2 || value.availability == 0 || !Nonzero(value.barrier)) return {};
  auto out = Header("PIRS", 320);
  out.insert(out.end(), value.body.begin(), value.body.end());
  const auto evidence = Evidence("ScratchBird.SblrProcedureInvokeExecutorEvidence.V1",
                                 out.data() + 16, 240);
  if (Nonzero(value.evidence) && value.evidence != evidence) return {};
  out.insert(out.end(), evidence.begin(), evidence.end());
  Put(&out, value.availability, 8);
  out.insert(out.end(), value.barrier.begin(), value.barrier.end());
  out.insert(out.end(), 8, 0);
  return out;
}
bool DecodeSblrProcedureInvokeResultV1(const std::uint8_t* bytes,
                                       std::size_t size,
                                       SblrProcedureInvokeResultV1* out,
                                       std::string* detail) {
  if (!out || !ValidHeader(bytes, size, "PIRS", 320) ||
      std::any_of(bytes + 312, bytes + 320, [](auto byte) { return byte != 0; })) {
    if (detail) *detail = "PIRS invalid"; return false;
  }
  SblrProcedureInvokeResultV1 value;
  std::copy_n(bytes + 16, 240, value.body.begin());
  std::copy_n(bytes + 256, 32, value.evidence.begin());
  value.availability = Get(bytes + 288, 8);
  std::copy_n(bytes + 296, 16, value.barrier.begin());
  if (EncodeSblrProcedureInvokeResultV1(value).empty()) return false;
  *out = value; return true;
}
}  // namespace scratchbird::engine::sblr
