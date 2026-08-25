#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_versioned_tag_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrVersionedTagCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrVersionedTagDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrVersionedTagCoordinationResult CompileSblrVersionedTagDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrVersionedTagCoordinationResult ConsumeSblrVersionedTagDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrVersionedTagDescriptorV1&);}
