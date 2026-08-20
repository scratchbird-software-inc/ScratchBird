#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_database_deserialize_logical_snapshot_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDatabaseDeserializeLogicalSnapshotCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDatabaseDeserializeLogicalSnapshotDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDatabaseDeserializeLogicalSnapshotCoordinationResult CompileSblrDatabaseDeserializeLogicalSnapshotDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDatabaseDeserializeLogicalSnapshotCoordinationResult ConsumeSblrDatabaseDeserializeLogicalSnapshotDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDatabaseDeserializeLogicalSnapshotDescriptorV1&); }
