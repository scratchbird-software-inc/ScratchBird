#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

using CatalogUuid = std::array<std::uint8_t, 16>;
using CatalogSha = std::array<std::uint8_t, 32>;

struct SblrCatalogIntrospectRequestV1 {
  CatalogUuid receipt{};
  std::uint64_t occurrence = 0;
  std::uint32_t object_occurrence = 0;
};

constexpr std::uint16_t kSblrCatalogIntrospectObjectKindTableV1 = 1;
constexpr std::uint16_t kSblrCatalogIntrospectProfileShowObjectDetailV1 = 1;
constexpr std::uint16_t kSblrCatalogIntrospectDetailFlagV1 = 0x0001;

// CIDD/CIDO v1 is a fixed 488-byte carrier.  The parser may copy CIDD to CIDO
// by changing only the four-byte magic; every authority field is engine-owned.
struct SblrCatalogIntrospectDescriptorV1 {
  std::uint16_t object_kind = 0;
  std::uint16_t profile = 0;
  std::uint16_t flags = 0;
  CatalogUuid object_uuid{};
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::string canonical_path_utf8;
  CatalogSha evidence{};
  std::uint64_t availability = 0;
};

// CIRS v1 is the terminal capability for the separately paged detail rowset.
// It binds the row material and cursor identities without exposing raw object
// UUIDs in the public rows themselves.
struct SblrCatalogIntrospectResultV1 {
  CatalogUuid request_uuid{};
  CatalogUuid readable_projection_uuid{};
  CatalogUuid row_descriptor_uuid{};
  CatalogUuid result_set_uuid{};
  CatalogUuid object_uuid{};
  CatalogUuid statement_snapshot_uuid{};
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t row_count = 0;
  std::uint16_t object_kind = 0;
  std::uint16_t profile = 0;
  std::uint16_t flags = 0;
  CatalogSha row_material_sha256{};
  CatalogSha descriptor_evidence_sha256{};
  CatalogSha evidence{};
  std::uint64_t availability = 0;
  CatalogUuid publication_barrier{};
};
std::vector<std::uint8_t> EncodeSblrCatalogIntrospectRequestV1(
    const SblrCatalogIntrospectRequestV1& request);
bool DecodeSblrCatalogIntrospectRequestV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrCatalogIntrospectRequestV1* output, std::string* detail);
std::vector<std::uint8_t> EncodeSblrCatalogIntrospectDescriptorV1(
    const SblrCatalogIntrospectDescriptorV1& descriptor, bool operand);
bool DecodeSblrCatalogIntrospectDescriptorV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrCatalogIntrospectDescriptorV1* output, std::string* detail,
    bool operand);
std::vector<std::uint8_t> EncodeSblrCatalogIntrospectResultV1(
    const SblrCatalogIntrospectResultV1& result);
bool DecodeSblrCatalogIntrospectResultV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrCatalogIntrospectResultV1* output, std::string* detail);

}  // namespace scratchbird::engine::sblr
