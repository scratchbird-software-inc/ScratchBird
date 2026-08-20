#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr { using BulkImportUuid=std::array<std::uint8_t,16>; using BulkImportSha=std::array<std::uint8_t,32>; struct SblrBulkImportStreamRequestV1{BulkImportUuid receipt{};std::uint64_t occurrence=0;std::uint32_t import_occurrence=0;};struct SblrBulkImportStreamDescriptorV1{std::array<std::uint8_t,368> canonical_body{};BulkImportSha evidence{};std::uint64_t availability_generation=0;};struct SblrBulkImportStreamResultV1{std::array<std::uint8_t,136> canonical_body{};BulkImportSha evidence{};std::uint64_t availability_generation=0;};std::vector<std::uint8_t>EncodeSblrBulkImportStreamRequestV1(const SblrBulkImportStreamRequestV1&);bool DecodeSblrBulkImportStreamRequestV1(const std::uint8_t*,std::size_t,SblrBulkImportStreamRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrBulkImportStreamDescriptorV1(const SblrBulkImportStreamDescriptorV1&,bool);bool DecodeSblrBulkImportStreamDescriptorV1(const std::uint8_t*,std::size_t,SblrBulkImportStreamDescriptorV1*,std::string*,bool);std::vector<std::uint8_t>EncodeSblrBulkImportStreamResultV1(const SblrBulkImportStreamResultV1&);bool DecodeSblrBulkImportStreamResultV1(const std::uint8_t*,std::size_t,SblrBulkImportStreamResultV1*,std::string*);}
