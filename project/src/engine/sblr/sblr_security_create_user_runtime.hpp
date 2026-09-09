#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

using SecurityCreateUserUuid = std::array<std::uint8_t, 16>;
using SecurityCreateUserSha = std::array<std::uint8_t, 32>;

// CREATE USER still owns its historical fixed carrier.  These declarations
// are intentionally independent from privilege-template evolution.
struct SblrSecurityCreateUserRequestV1 {
  SecurityCreateUserUuid receipt{};
  std::uint64_t occurrence = 0;
  std::uint32_t template_occurrence = 0;
};

struct SblrSecurityCreateUserDescriptorV1 {
  std::array<std::uint8_t, 400> body{};
  SecurityCreateUserSha evidence{};
  std::uint64_t availability = 0;
};

struct SblrSecurityCreateUserResultV1 {
  std::array<std::uint8_t, 240> body{};
  SecurityCreateUserSha evidence{};
  std::uint64_t availability = 0;
  SecurityCreateUserUuid publication_barrier{};
};

std::vector<std::uint8_t> EncodeSblrSecurityCreateUserRequestV1(
    const SblrSecurityCreateUserRequestV1&);
bool DecodeSblrSecurityCreateUserRequestV1(
    const std::uint8_t*, std::size_t, SblrSecurityCreateUserRequestV1*,
    std::string*);
std::vector<std::uint8_t> EncodeSblrSecurityCreateUserDescriptorV1(
    const SblrSecurityCreateUserDescriptorV1&, bool);
bool DecodeSblrSecurityCreateUserDescriptorV1(
    const std::uint8_t*, std::size_t, SblrSecurityCreateUserDescriptorV1*,
    std::string*, bool);
std::vector<std::uint8_t> EncodeSblrSecurityCreateUserResultV1(
    const SblrSecurityCreateUserResultV1&);
bool DecodeSblrSecurityCreateUserResultV1(
    const std::uint8_t*, std::size_t, SblrSecurityCreateUserResultV1*,
    std::string*);

}  // namespace scratchbird::engine::sblr
