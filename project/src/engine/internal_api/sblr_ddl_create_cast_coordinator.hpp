#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_cast_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrDdlCreateCastCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlCreateCastDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrDdlCreateCastCoordinationResult CompileSblrDdlCreateCastDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrDdlCreateCastCoordinationResult ConsumeSblrDdlCreateCastDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateCastDescriptorV1&);}
