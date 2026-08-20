#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_domain_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlCreateDomainCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlCreateDomainDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlCreateDomainCoordinationResult CompileSblrDdlCreateDomainDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlCreateDomainCoordinationResult ConsumeSblrDdlCreateDomainDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateDomainDescriptorV1&); }
