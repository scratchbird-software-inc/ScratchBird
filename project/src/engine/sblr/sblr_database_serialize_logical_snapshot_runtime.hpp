#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using LogicalSnapshotSha=std::array<std::uint8_t,32>;
using LogicalSnapshotUuid=std::array<std::uint8_t,16>;
struct SblrDatabaseSerializeLogicalSnapshotRequestV1 { LogicalSnapshotUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t snapshot_occurrence=0; };
struct SblrDatabaseSerializeLogicalSnapshotDescriptorV1 { std::array<std::uint8_t,400> body{}; LogicalSnapshotSha evidence{}; std::uint64_t availability=0; };
struct SblrDatabaseSerializeLogicalSnapshotResultV1 { std::array<std::uint8_t,440> body{}; LogicalSnapshotSha evidence{}; std::uint64_t availability=0; LogicalSnapshotUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDatabaseSerializeLogicalSnapshotRequestV1(const SblrDatabaseSerializeLogicalSnapshotRequestV1&);
bool DecodeSblrDatabaseSerializeLogicalSnapshotRequestV1(const std::uint8_t*,std::size_t,SblrDatabaseSerializeLogicalSnapshotRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDatabaseSerializeLogicalSnapshotDescriptorV1(const SblrDatabaseSerializeLogicalSnapshotDescriptorV1&,bool);
bool DecodeSblrDatabaseSerializeLogicalSnapshotDescriptorV1(const std::uint8_t*,std::size_t,SblrDatabaseSerializeLogicalSnapshotDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDatabaseSerializeLogicalSnapshotResultV1(const SblrDatabaseSerializeLogicalSnapshotResultV1&);
bool DecodeSblrDatabaseSerializeLogicalSnapshotResultV1(const std::uint8_t*,std::size_t,SblrDatabaseSerializeLogicalSnapshotResultV1*,std::string*);
}
