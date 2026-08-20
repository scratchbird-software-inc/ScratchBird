// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "sblr_opcode_stream.hpp"

#include <array>
#include <limits>

namespace scratchbird::engine::sblr {
namespace {

using Bytes = std::vector<std::uint8_t>;

void U16(Bytes& out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v));
  out.push_back(static_cast<std::uint8_t>(v >> 8));
}
void U32(Bytes& out, std::uint32_t v) {
  for (unsigned n = 0; n != 4; ++n) out.push_back(static_cast<std::uint8_t>(v >> (n * 8)));
}
void U64(Bytes& out, std::uint64_t v) {
  for (unsigned n = 0; n != 8; ++n) out.push_back(static_cast<std::uint8_t>(v >> (n * 8)));
}
bool Read16(std::string_view in, std::size_t off, std::uint16_t* v) {
  if (off + 2 > in.size()) return false;
  *v = static_cast<std::uint8_t>(in[off]) |
       (static_cast<std::uint16_t>(static_cast<std::uint8_t>(in[off + 1])) << 8);
  return true;
}
bool Read32(std::string_view in, std::size_t off, std::uint32_t* v) {
  if (off + 4 > in.size()) return false;
  *v = 0;
  for (unsigned n = 0; n != 4; ++n) *v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[off + n])) << (n * 8);
  return true;
}
bool Read64(std::string_view in, std::size_t off, std::uint64_t* v) {
  if (off + 8 > in.size()) return false;
  *v = 0;
  for (unsigned n = 0; n != 8; ++n) *v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(in[off + n])) << (n * 8);
  return true;
}

bool ParseUuid(std::string_view text, std::array<std::uint8_t, 16>* out) {
  if (text.size() != 36) return false;
  std::size_t p = 0;
  for (std::size_t i = 0; i != text.size();) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (text[i++] != '-') return false;
      continue;
    }
    auto hex = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return -1;
    };
    if (i + 1 >= text.size()) return false;
    const int hi = hex(text[i++]), lo = hex(text[i++]);
    if (hi < 0 || lo < 0 || p == out->size()) return false;
    (*out)[p++] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  bool nonzero = false;
  for (auto b : *out) nonzero = nonzero || b != 0;
  return p == out->size() && nonzero;
}

std::string FormatUuid(const std::uint8_t* bytes) {
  constexpr char h[] = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (std::size_t i = 0; i != 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
    out.push_back(h[bytes[i] >> 4]);
    out.push_back(h[bytes[i] & 15]);
  }
  return out;
}

SblrOpcodeStreamResult Fail(std::string id, std::string detail) {
  SblrOpcodeStreamResult result;
  result.diagnostic_id = std::move(id);
  result.detail = std::move(detail);
  return result;
}

bool IsFrame(const SblrOperationEnvelope& op, bool begin,
             const std::array<std::uint8_t, 16>& package_uuid) {
  const auto code = begin ? 0x0001u : 0x0002u;
  const auto mnemonic = begin ? "SBLR_PACKAGE_BEGIN" : "SBLR_PACKAGE_END";
  const auto key = begin ? "engine.op.package_begin" : "engine.op.package_end";
  return op.opcode_code == code && op.opcode == mnemonic && op.operation_id == key &&
         op.operands.size() == 1 && op.operands[0].ordinal == 1 &&
         op.operands[0].value_kind == SblrValueKind::descriptor_ref &&
         op.operands[0].type == (begin ? "package.header" : "package.footer") &&
         op.operands[0].name == "package_descriptor" &&
         op.operands[0].value_body.size() == package_uuid.size() &&
         std::equal(package_uuid.begin(), package_uuid.end(), op.operands[0].value_body.begin());
}

bool IsFrameShape(const SblrOperationEnvelope& op, bool begin) {
  const auto code = begin ? 0x0001u : 0x0002u;
  const auto mnemonic = begin ? "SBLR_PACKAGE_BEGIN" : "SBLR_PACKAGE_END";
  const auto key = begin ? "engine.op.package_begin" : "engine.op.package_end";
  return op.opcode_code == code && op.opcode == mnemonic &&
         op.operation_id == key && op.operands.size() == 1 &&
         op.operands[0].ordinal == 1 &&
         op.operands[0].value_kind == SblrValueKind::descriptor_ref &&
         op.operands[0].type == (begin ? "package.header" : "package.footer") &&
         op.operands[0].name == "package_descriptor" &&
         op.operands[0].value_body.size() == 16;
}

}  // namespace

std::vector<std::uint8_t> EncodeSblrOpcodeStream(const SblrOpcodeStream& stream) {
  if (stream.operations.size() < 2 || stream.operations.size() > kSblrOperationMaximumOperands) return {};
  std::array<std::uint8_t, 16> package{}, registry{};
  if (!ParseUuid(stream.package_descriptor_uuid, &package) ||
      !ParseUuid(stream.registry_snapshot_uuid, &registry) ||
      !IsFrame(stream.operations.front(), true, package) ||
      !IsFrame(stream.operations.back(), false, package)) return {};
  Bytes records;
  for (std::size_t i = 0; i != stream.operations.size(); ++i) {
    const auto& op = stream.operations[i];
    if (op.registry_snapshot_uuid != stream.registry_snapshot_uuid ||
        (i != 0 && i + 1 != stream.operations.size() &&
         (op.opcode_code == 0x0001u || op.opcode_code == 0x0002u))) return {};
    const std::string encoded = EncodeSblrEnvelope(op);
    constexpr std::size_t kFixedBytes =
        kSblrOpcodeStreamHeaderSize + kSblrOpcodeStreamTrailerSize;
    if (encoded.empty() ||
        encoded.size() > kSblrOperationMaximumBytes - kFixedBytes - 8 ||
        records.size() >
            kSblrOperationMaximumBytes - kFixedBytes - 8 - encoded.size()) {
      return {};
    }
    U64(records, encoded.size());
    records.insert(records.end(), encoded.begin(), encoded.end());
  }
  Bytes out;
  U32(out, kSblrOpcodeStreamMagic); U16(out, 1); U16(out, 0);
  U16(out, kSblrOpcodeStreamHeaderSize); U16(out, 0); U32(out, 0);
  U32(out, static_cast<std::uint32_t>(stream.operations.size())); U32(out, 0);
  out.insert(out.end(), package.begin(), package.end());
  out.insert(out.end(), registry.begin(), registry.end());
  U64(out, records.size());
  out.insert(out.end(), records.begin(), records.end());
  U32(out, kSblrOpcodeStreamTrailerMagic);
  U32(out, SblrCrc32c(out.data(), out.size()));
  U64(out, out.size() + sizeof(std::uint64_t));
  return out.size() <= kSblrOperationMaximumBytes ? out : Bytes{};
}

SblrOpcodeStreamResult DecodeSblrOpcodeStream(std::string_view bytes) {
  if (bytes.size() < kSblrOpcodeStreamHeaderSize + kSblrOpcodeStreamTrailerSize ||
      bytes.size() > kSblrOperationMaximumBytes) return Fail("SBLR.OPERAND_INVALID", "SBOS size is outside canonical bounds");
  std::uint32_t magic = 0, flags = 0, count = 0, reserved = 0, trailer = 0, crc = 0;
  std::uint16_t major = 0, minor = 0, header = 0, reserved0 = 0;
  std::uint64_t records_size = 0, total = 0;
  if (!Read32(bytes, 0, &magic) || !Read16(bytes, 4, &major) || !Read16(bytes, 6, &minor) ||
      !Read16(bytes, 8, &header) || !Read16(bytes, 10, &reserved0) || !Read32(bytes, 12, &flags) ||
      !Read32(bytes, 16, &count) || !Read32(bytes, 20, &reserved) || !Read64(bytes, 56, &records_size) ||
      !Read32(bytes, bytes.size() - 16, &trailer) || !Read32(bytes, bytes.size() - 12, &crc) ||
      !Read64(bytes, bytes.size() - 8, &total) || magic != kSblrOpcodeStreamMagic || major != 1 ||
      minor != 0 || header != 64 || reserved0 != 0 || flags != 0 || reserved != 0 || count < 2 ||
      count > kSblrOperationMaximumOperands || records_size != bytes.size() - 80 ||
      trailer != kSblrOpcodeStreamTrailerMagic || total != bytes.size() ||
      SblrCrc32c(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size() - 12) != crc) {
    return Fail("SBLR.OPERAND_INVALID", "SBOS header trailer count size or CRC is invalid");
  }
  SblrOpcodeStream stream;
  stream.package_descriptor_uuid = FormatUuid(reinterpret_cast<const std::uint8_t*>(bytes.data() + 24));
  stream.registry_snapshot_uuid = FormatUuid(reinterpret_cast<const std::uint8_t*>(bytes.data() + 40));
  std::array<std::uint8_t, 16> package{}, registry{};
  if (!ParseUuid(stream.package_descriptor_uuid, &package) || !ParseUuid(stream.registry_snapshot_uuid, &registry))
    return Fail("DATATYPE.DESCRIPTOR_INVALID", "SBOS package or registry UUID is zero");
  std::size_t offset = 64;
  for (std::uint32_t i = 0; i != count; ++i) {
    std::uint64_t size = 0;
    if (!Read64(bytes, offset, &size) || size > bytes.size() || offset + 8 > bytes.size() - 16 ||
        size > bytes.size() - 16 - offset - 8) return Fail("SBLR.OPERAND_INVALID", "SBOS record size is invalid");
    offset += 8;
    const auto decoded = DecodeSblrEnvelope(bytes.substr(offset, static_cast<std::size_t>(size)));
    if (!decoded.ok) {
      const auto code = decoded.diagnostics.empty()
                            ? "SBLR.OPERAND_INVALID"
                            : decoded.diagnostics.front().code;
      const auto detail = decoded.diagnostics.empty()
                              ? "unknown canonical SBOP validation failure"
                              : decoded.diagnostics.front().message;
      return Fail(code,
                  "SBOS contains a noncanonical SBOP record at index " +
                      std::to_string(i) + ": " + detail);
    }
    if (decoded.envelope.registry_snapshot_uuid != stream.registry_snapshot_uuid)
      return Fail("DATATYPE.DESCRIPTOR_INVALID", "SBOS record registry generation differs");
    stream.operations.push_back(decoded.envelope);
    offset += static_cast<std::size_t>(size);
  }
  if (offset != bytes.size() - 16 ||
      !IsFrameShape(stream.operations.front(), true) ||
      !IsFrameShape(stream.operations.back(), false))
    return Fail("SBLR.OPERAND_INVALID", "SBOS framing pair is absent or mismatched");
  for (std::size_t i = 1; i + 1 < stream.operations.size(); ++i)
    if (stream.operations[i].opcode_code == 0x0001u || stream.operations[i].opcode_code == 0x0002u)
      return Fail("SBLR.OPERAND_INVALID", "SBOS contains nested package framing");
  if (!IsFrame(stream.operations.front(), true, package) ||
      !IsFrame(stream.operations.back(), false, package))
    return Fail("DATATYPE.DESCRIPTOR_INVALID", "SBOS framing descriptor identity differs");
  auto canonical = EncodeSblrOpcodeStream(stream);
  if (canonical.size() != bytes.size())
    return Fail("SBLR.OPERATION.NONCANONICAL", "SBOS canonical re-encoding size differs");
  const auto mismatch = std::mismatch(
      canonical.begin(), canonical.end(), bytes.begin(),
      [](std::uint8_t left, char right) {
        return left == static_cast<std::uint8_t>(right);
      });
  if (mismatch.first != canonical.end())
    return Fail("SBLR.OPERATION.NONCANONICAL", "SBOS canonical re-encoding differs at byte " +
                                                std::to_string(mismatch.first - canonical.begin()) +
                                                " expected=" + std::to_string(*mismatch.first) +
                                                " actual=" + std::to_string(static_cast<std::uint8_t>(
                                                    bytes[mismatch.first - canonical.begin()])));
  SblrOpcodeStreamResult result;
  result.ok = true; result.stream = std::move(stream); result.canonical_bytes = std::move(canonical);
  return result;
}

SblrOpcodeStreamResult AdmitSblrOpcodeStream(std::string_view bytes,
                                             const SblrOpcodeStreamAdmission& admission) {
  auto result = DecodeSblrOpcodeStream(bytes);
  if (!result.ok) return result;
  if (result.stream.registry_snapshot_uuid != admission.admitted_registry_snapshot_uuid)
    return Fail("DATATYPE.DESCRIPTOR_INVALID", "SBOS registry generation is stale");
  if (!admission.descriptor_class_accepted) return Fail("DATATYPE.DESCRIPTOR_INVALID", "sblr.package.v1 descriptor evidence is absent");
  if (!admission.authenticated) return Fail("SECURITY.ACCESS_DENIED", "authenticated package principal is absent");
  if (!admission.gateway_pass_through) return Fail("PROCESS.CLUSTER_PATH_ABSENT", "gateway did not pass through local package framing");
  if (!admission.executor_evidence_accepted) return Fail("SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING", "package framing executor evidence is absent");
  if (!admission.resource_budget_available) return Fail("RESOURCE.BUDGET_EXCEEDED", "package admission budget is exhausted");
  if (admission.cancelled) return Fail("PROCESS.CANCELLED", "package admission was cancelled before dispatch");
  return result;
}

}  // namespace scratchbird::engine::sblr
