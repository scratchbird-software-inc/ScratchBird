#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using LogicalSnapshotDeserializeUuid=std::array<std::uint8_t,16>;
using LogicalSnapshotDeserializeSha=std::array<std::uint8_t,32>;
struct SblrDatabaseDeserializeLogicalSnapshotRequestV1 { LogicalSnapshotDeserializeUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t snapshot_occurrence=0; };
struct SblrDatabaseDeserializeLogicalSnapshotDescriptorV1 { std::array<std::uint8_t,400> body{}; LogicalSnapshotDeserializeSha evidence{}; std::uint64_t availability=0; };
struct SblrDatabaseDeserializeLogicalSnapshotResultV1 { std::array<std::uint8_t,248> body{}; LogicalSnapshotDeserializeSha evidence{}; std::uint64_t availability=0; LogicalSnapshotDeserializeUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDatabaseDeserializeLogicalSnapshotRequestV1(const SblrDatabaseDeserializeLogicalSnapshotRequestV1&);
bool DecodeSblrDatabaseDeserializeLogicalSnapshotRequestV1(const std::uint8_t*,std::size_t,SblrDatabaseDeserializeLogicalSnapshotRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDatabaseDeserializeLogicalSnapshotDescriptorV1(const SblrDatabaseDeserializeLogicalSnapshotDescriptorV1&,bool);
bool DecodeSblrDatabaseDeserializeLogicalSnapshotDescriptorV1(const std::uint8_t*,std::size_t,SblrDatabaseDeserializeLogicalSnapshotDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDatabaseDeserializeLogicalSnapshotResultV1(const SblrDatabaseDeserializeLogicalSnapshotResultV1&);
bool DecodeSblrDatabaseDeserializeLogicalSnapshotResultV1(const std::uint8_t*,std::size_t,SblrDatabaseDeserializeLogicalSnapshotResultV1*,std::string*);
}
