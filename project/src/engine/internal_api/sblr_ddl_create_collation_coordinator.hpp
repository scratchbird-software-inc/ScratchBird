#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_collation_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrDdlCreateCollationCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlCreateCollationDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrDdlCreateCollationCoordinationResult CompileSblrDdlCreateCollationDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrDdlCreateCollationCoordinationResult ConsumeSblrDdlCreateCollationDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateCollationDescriptorV1&);}
