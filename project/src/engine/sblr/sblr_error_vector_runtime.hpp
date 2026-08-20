#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using SblrErrorUuidV1=std::array<std::uint8_t,16>;using SblrErrorSha256V1=std::array<std::uint8_t,32>;
struct SblrErrorVectorEntryV1{std::uint64_t occurrence_ordinal=0;SblrErrorUuidV1 diagnostic_uuid{};std::uint64_t diagnostic_generation=0;std::uint32_t precedence_ordinal=0;std::uint8_t severity_code=0,redaction_class=0;std::uint32_t safe_field_count=0;SblrErrorSha256V1 safe_fields_sha256{},entry_sha256{};};
struct SblrErrorVectorDescriptorV1{SblrErrorUuidV1 descriptor_uuid{},registry_snapshot_uuid{},statement_receipt_uuid{},diagnostic_registry_snapshot_uuid{};std::uint64_t descriptor_generation=0,registry_generation=0,diagnostic_registry_generation=0;SblrErrorSha256V1 vector_sha256{};std::vector<SblrErrorVectorEntryV1> entries;};
struct SblrErrorVectorIssueRequestV1{SblrErrorUuidV1 statement_receipt_uuid{},registry_snapshot_uuid{},diagnostic_registry_snapshot_uuid{};std::uint64_t registry_generation=0,diagnostic_registry_generation=0;SblrErrorSha256V1 entries_sha256{};std::vector<SblrErrorVectorEntryV1> entries;};
struct SblrErrorVectorIssueResultV1{SblrErrorUuidV1 descriptor_uuid{};std::uint64_t descriptor_generation=0,registry_generation=0;std::vector<std::uint8_t> canonical_ervd;};
std::vector<std::uint8_t> EncodeSblrErrorVectorDescriptorV1(SblrErrorVectorDescriptorV1*);
bool DecodeSblrErrorVectorDescriptorV1(const std::uint8_t*,std::size_t,SblrErrorVectorDescriptorV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrErrorVectorIssueRequestV1(SblrErrorVectorIssueRequestV1*);
bool DecodeSblrErrorVectorIssueRequestV1(const std::uint8_t*,std::size_t,SblrErrorVectorIssueRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrErrorVectorIssueResultV1(const SblrErrorVectorIssueResultV1&);
bool DecodeSblrErrorVectorIssueResultV1(const std::uint8_t*,std::size_t,SblrErrorVectorIssueResultV1*,std::string*);
}
