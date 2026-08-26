#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_versioned_status_read_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrVersionedStatusReadCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrVersionedStatusReadDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrVersionedStatusReadCoordinationResult CompileSblrVersionedStatusReadDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t); SblrVersionedStatusReadCoordinationResult ConsumeSblrVersionedStatusReadDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrVersionedStatusReadDescriptorV1&); }
