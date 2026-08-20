#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using ProjectUuid = std::array<std::uint8_t, 16>;
using ProjectSha = std::array<std::uint8_t, 32>;
struct SblrProjectRequestV1 { ProjectUuid receipt{}; std::uint64_t occurrence = 0; std::uint32_t projection_occurrence = 0; };
struct SblrProjectDescriptorV1 { std::array<std::uint8_t, 392> body{}; ProjectSha evidence{}; std::uint64_t availability = 0; };
struct SblrProjectResultV1 { std::array<std::uint8_t, 240> body{}; ProjectSha evidence{}; std::uint64_t availability = 0; ProjectUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrProjectRequestV1(const SblrProjectRequestV1&);
bool DecodeSblrProjectRequestV1(const std::uint8_t*, std::size_t, SblrProjectRequestV1*, std::string*);
std::vector<std::uint8_t> EncodeSblrProjectDescriptorV1(const SblrProjectDescriptorV1&, bool);
bool DecodeSblrProjectDescriptorV1(const std::uint8_t*, std::size_t, SblrProjectDescriptorV1*, std::string*, bool);
std::vector<std::uint8_t> EncodeSblrProjectResultV1(const SblrProjectResultV1&);
bool DecodeSblrProjectResultV1(const std::uint8_t*, std::size_t, SblrProjectResultV1*, std::string*);
}
