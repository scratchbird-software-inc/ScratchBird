#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_drop_cast_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrDdlDropCastCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlDropCastDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrDdlDropCastCoordinationResult CompileSblrDdlDropCastDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrDdlDropCastCoordinationResult ConsumeSblrDdlDropCastDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropCastDescriptorV1&);}
