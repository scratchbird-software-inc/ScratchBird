#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_table_truncate_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrTableTruncateCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrTableTruncateDescriptorV1 descriptor;EngineApiDiagnostic diagnostic;};SblrTableTruncateCoordinationResult CompileSblrTableTruncateDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrTableTruncateCoordinationResult ConsumeSblrTableTruncateDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrTableTruncateDescriptorV1&);}
