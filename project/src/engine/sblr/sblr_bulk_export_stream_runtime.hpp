#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr { using BulkExportUuid=std::array<std::uint8_t,16>; using BulkExportSha=std::array<std::uint8_t,32>; struct SblrBulkExportStreamRequestV1{BulkExportUuid receipt{};std::uint64_t occurrence=0;std::uint32_t export_occurrence=0;};struct SblrBulkExportStreamDescriptorV1{std::array<std::uint8_t,368> canonical_body{};BulkExportSha evidence{};std::uint64_t availability_generation=0;};struct SblrBulkExportStreamResultV1{std::array<std::uint8_t,136> canonical_body{};BulkExportSha evidence{};std::uint64_t availability_generation=0;};std::vector<std::uint8_t>EncodeSblrBulkExportStreamRequestV1(const SblrBulkExportStreamRequestV1&);bool DecodeSblrBulkExportStreamRequestV1(const std::uint8_t*,std::size_t,SblrBulkExportStreamRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrBulkExportStreamDescriptorV1(const SblrBulkExportStreamDescriptorV1&,bool);bool DecodeSblrBulkExportStreamDescriptorV1(const std::uint8_t*,std::size_t,SblrBulkExportStreamDescriptorV1*,std::string*,bool);std::vector<std::uint8_t>EncodeSblrBulkExportStreamResultV1(const SblrBulkExportStreamResultV1&);bool DecodeSblrBulkExportStreamResultV1(const std::uint8_t*,std::size_t,SblrBulkExportStreamResultV1*,std::string*);}
