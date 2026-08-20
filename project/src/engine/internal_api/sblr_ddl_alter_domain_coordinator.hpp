#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_alter_domain_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlAlterDomainCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlAlterDomainDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlAlterDomainCoordinationResult CompileSblrDdlAlterDomainDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlAlterDomainCoordinationResult ConsumeSblrDdlAlterDomainDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlAlterDomainDescriptorV1&); }
