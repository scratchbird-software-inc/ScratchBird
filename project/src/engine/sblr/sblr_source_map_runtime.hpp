#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using SblrSourceMapUuidV1=std::array<std::uint8_t,16>;
using SblrSourceMapSha256V1=std::array<std::uint8_t,32>;
enum class SblrSourceMapDecodeStatusV1{ok,operand_invalid,resource_exceeded};
struct SblrSourceMapEntryV1{std::uint64_t node_id=0,parent_node_id=0;SblrSourceMapUuidV1 source_artifact_uuid{};std::uint64_t source_artifact_generation=0,byte_offset=0,byte_length=0;std::uint32_t line=0,column=0;std::uint8_t redaction_class=0,flags=0;SblrSourceMapSha256V1 entry_sha256{};};
struct SblrSourceMapDescriptorVectorV1{SblrSourceMapUuidV1 descriptor_uuid{},registry_snapshot_uuid{},statement_receipt_uuid{};std::uint64_t descriptor_generation=0,registry_generation=0;SblrSourceMapSha256V1 bound_ast_sha256{},vector_sha256{};std::vector<SblrSourceMapEntryV1> entries;};
struct SblrSourceMapDecodeResultV1{SblrSourceMapDecodeStatusV1 status=SblrSourceMapDecodeStatusV1::operand_invalid;SblrSourceMapDescriptorVectorV1 vector;std::vector<std::uint8_t> canonical_bytes;std::string detail;};
struct SblrSourceMapBoundAstNodeV1{std::uint64_t node_id=0,parent_node_id=0;std::uint16_t node_kind_code=0;};
struct SblrSourceMapBoundAstV1{SblrSourceMapUuidV1 statement_receipt_uuid{};SblrSourceMapSha256V1 node_records_sha256{};std::vector<SblrSourceMapBoundAstNodeV1> nodes;};
struct SblrSourceMapIssueRequestV1{SblrSourceMapUuidV1 statement_receipt_uuid{},registry_snapshot_uuid{};std::uint64_t registry_generation=0;SblrSourceMapSha256V1 bound_ast_sha256{},entries_sha256{};std::vector<std::uint8_t> canonical_bound_ast;std::vector<SblrSourceMapEntryV1> entries;};
struct SblrSourceMapIssueResultV1{SblrSourceMapUuidV1 descriptor_uuid{};std::uint64_t descriptor_generation=0,registry_generation=0;std::vector<std::uint8_t> canonical_smvd;};
std::vector<std::uint8_t> EncodeSblrSourceMapDescriptorVectorV1(SblrSourceMapDescriptorVectorV1*);
SblrSourceMapDecodeResultV1 DecodeSblrSourceMapDescriptorVectorV1(const std::uint8_t*,std::size_t);
std::vector<std::uint8_t> EncodeSblrSourceMapBoundAstV1(SblrSourceMapBoundAstV1*);
bool DecodeSblrSourceMapBoundAstV1(const std::uint8_t*,std::size_t,SblrSourceMapBoundAstV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrSourceMapIssueRequestV1(SblrSourceMapIssueRequestV1*);
bool DecodeSblrSourceMapIssueRequestV1(const std::uint8_t*,std::size_t,SblrSourceMapIssueRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrSourceMapIssueResultV1(const SblrSourceMapIssueResultV1&);
bool DecodeSblrSourceMapIssueResultV1(const std::uint8_t*,std::size_t,SblrSourceMapIssueResultV1*,std::string*);
}
