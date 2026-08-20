#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {
using ReturnResultSetUuid = std::array<std::uint8_t, 16>;
using ReturnResultSetSha = std::array<std::uint8_t, 32>;

struct SblrReturnResultSetRequestV1 {
  ReturnResultSetUuid receipt{};
  std::uint64_t occurrence{0};
  std::uint32_t return_occurrence{0};
};
struct SblrReturnResultSetDescriptorV1 {
  std::array<std::uint8_t, 360> body{};
  ReturnResultSetSha evidence{};
  std::uint64_t availability{0};
};
struct SblrReturnResultSetResultV1 {
  std::array<std::uint8_t, 168> body{};
  ReturnResultSetSha evidence{};
  std::uint64_t availability{0};
  ReturnResultSetUuid publication_barrier{};
};

std::vector<std::uint8_t> EncodeSblrReturnResultSetRequestV1(const SblrReturnResultSetRequestV1&);
bool DecodeSblrReturnResultSetRequestV1(const std::uint8_t*, std::size_t, SblrReturnResultSetRequestV1*, std::string*);
std::vector<std::uint8_t> EncodeSblrReturnResultSetDescriptorV1(const SblrReturnResultSetDescriptorV1&, bool operand);
bool DecodeSblrReturnResultSetDescriptorV1(const std::uint8_t*, std::size_t, SblrReturnResultSetDescriptorV1*, std::string*, bool operand);
std::vector<std::uint8_t> EncodeSblrReturnResultSetResultV1(const SblrReturnResultSetResultV1&);
bool DecodeSblrReturnResultSetResultV1(const std::uint8_t*, std::size_t, SblrReturnResultSetResultV1*, std::string*);
}
