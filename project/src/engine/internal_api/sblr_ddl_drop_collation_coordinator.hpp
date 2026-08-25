#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_drop_collation_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrDdlDropCollationCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlDropCollationDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrDdlDropCollationCoordinationResult CompileSblrDdlDropCollationDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrDdlDropCollationCoordinationResult ConsumeSblrDdlDropCollationDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropCollationDescriptorV1&);}
