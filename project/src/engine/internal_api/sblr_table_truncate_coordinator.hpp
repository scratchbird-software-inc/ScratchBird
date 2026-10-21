#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_table_truncate_runtime.hpp"
namespace scratchbird::engine::internal_api{
struct SblrTableTruncateCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrTableTruncateDescriptorV1 descriptor;EngineApiDiagnostic diagnostic;};

// The engine-resolved target this descriptor replaces the content generation
// of, as published onto the live receipt by BindStatementTableTruncateTargetV1
// (SBSQL-TRUNCATE-TABLE-V1 section 4 steps 2-3). The coordinator never accepts
// a target identity from a carrier or a parser: an absent or zero target is a
// refusal, not a default.
struct SblrTableTruncateResolvedTargetV1{
  EngineUuid persisted_descriptor_uuid;
  std::uint64_t persisted_descriptor_generation=0;
};

// Issues the immutable engine-owned record the section 6.1 carrier references,
// and the carrier itself. Refuses -- rather than reporting ok with an
// unencodable body -- when the target is absent, any captured authority field
// is unavailable, or the canonical encode fails.
SblrTableTruncateCoordinationResult CompileSblrTableTruncateDescriptor(const EngineRequestContext&,const EngineUuid&,std::uint64_t,std::uint32_t,std::uint64_t,const SblrTableTruncateResolvedTargetV1&);
SblrTableTruncateCoordinationResult ConsumeSblrTableTruncateDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrTableTruncateDescriptorV1&);}
