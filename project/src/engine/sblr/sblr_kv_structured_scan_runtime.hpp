#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using KvStructuredScanUuid=std::array<std::uint8_t,16>; using KvStructuredScanSha=std::array<std::uint8_t,32>;
struct SblrKvStructuredScanRequestV1 { KvStructuredScanUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t scan_occurrence=0; };
struct SblrKvStructuredScanDescriptorV1 { std::array<std::uint8_t,400> body{}; KvStructuredScanSha evidence{}; std::uint64_t availability=0; };
struct SblrKvStructuredScanResultV1 { std::array<std::uint8_t,240> body{}; KvStructuredScanSha evidence{}; std::uint64_t availability=0; KvStructuredScanUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrKvStructuredScanRequestV1(const SblrKvStructuredScanRequestV1&);
bool DecodeSblrKvStructuredScanRequestV1(const std::uint8_t*,std::size_t,SblrKvStructuredScanRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrKvStructuredScanDescriptorV1(const SblrKvStructuredScanDescriptorV1&,bool);
bool DecodeSblrKvStructuredScanDescriptorV1(const std::uint8_t*,std::size_t,SblrKvStructuredScanDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrKvStructuredScanResultV1(const SblrKvStructuredScanResultV1&);
bool DecodeSblrKvStructuredScanResultV1(const std::uint8_t*,std::size_t,SblrKvStructuredScanResultV1*,std::string*);
}
