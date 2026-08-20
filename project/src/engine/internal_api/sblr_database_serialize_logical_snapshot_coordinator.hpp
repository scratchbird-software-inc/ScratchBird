#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_database_serialize_logical_snapshot_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDatabaseSerializeLogicalSnapshotCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDatabaseSerializeLogicalSnapshotDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDatabaseSerializeLogicalSnapshotCoordinationResult CompileSblrDatabaseSerializeLogicalSnapshotDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDatabaseSerializeLogicalSnapshotCoordinationResult ConsumeSblrDatabaseSerializeLogicalSnapshotDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDatabaseSerializeLogicalSnapshotDescriptorV1&); }
